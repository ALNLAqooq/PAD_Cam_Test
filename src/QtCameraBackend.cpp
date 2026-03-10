#include "QtCameraBackend.h"

#include <QCameraFocus>
#include <QCameraViewfinder>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QImageEncoderSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QVideoFrame>
#include <QVideoProbe>
#include <QVideoWidget>

#include <algorithm>

namespace
{
qint64 resolutionArea(const QSize &size)
{
    return static_cast<qint64>(size.width()) * static_cast<qint64>(size.height());
}

QString sizeToText(const QSize &size)
{
    if (!size.isValid()) {
        return QStringLiteral("-");
    }

    return QStringLiteral("%1 x %2").arg(size.width()).arg(size.height());
}

QString supportToText(bool supported)
{
    return supported ? QStringLiteral("支持") : QStringLiteral("不支持");
}

QString positionToText(QCamera::Position position)
{
    switch (position) {
    case QCamera::FrontFace:
        return QStringLiteral("前置");
    case QCamera::BackFace:
        return QStringLiteral("后置");
    case QCamera::UnspecifiedPosition:
    default:
        return QStringLiteral("未知");
    }
}

QString stateToText(QCamera::State state)
{
    switch (state) {
    case QCamera::UnloadedState:
        return QStringLiteral("Unloaded");
    case QCamera::LoadedState:
        return QStringLiteral("Loaded");
    case QCamera::ActiveState:
        return QStringLiteral("Active");
    default:
        return QStringLiteral("Unknown");
    }
}

QString statusToText(QCamera::Status status)
{
    switch (status) {
    case QCamera::UnavailableStatus:
        return QStringLiteral("Unavailable");
    case QCamera::UnloadedStatus:
        return QStringLiteral("Unloaded");
    case QCamera::LoadingStatus:
        return QStringLiteral("Loading");
    case QCamera::UnloadingStatus:
        return QStringLiteral("Unloading");
    case QCamera::LoadedStatus:
        return QStringLiteral("Loaded");
    case QCamera::StandbyStatus:
        return QStringLiteral("Standby");
    case QCamera::StartingStatus:
        return QStringLiteral("Starting");
    case QCamera::StoppingStatus:
        return QStringLiteral("Stopping");
    case QCamera::ActiveStatus:
        return QStringLiteral("Active");
    default:
        return QStringLiteral("Unknown");
    }
}

QString viewfinderSettingToText(const QCameraViewfinderSettings &setting)
{
    const QSize resolution = setting.resolution();
    const qreal minFps = setting.minimumFrameRate();
    const qreal maxFps = setting.maximumFrameRate();

    QString fpsText = QStringLiteral("fps 未知");
    if (minFps > 0.0 || maxFps > 0.0) {
        fpsText = QStringLiteral("%1 - %2 fps")
                      .arg(minFps, 0, 'f', 2)
                      .arg(maxFps, 0, 'f', 2);
    }

    return QStringLiteral("%1 | %2 | pixelFormat=%3")
        .arg(sizeToText(resolution))
        .arg(fpsText)
        .arg(static_cast<int>(setting.pixelFormat()));
}
}

QtCameraBackend::QtCameraBackend(QObject *parent)
    : CameraBackend(parent)
    , m_camera(nullptr)
    , m_imageCapture(nullptr)
    , m_videoProbe(nullptr)
    , m_viewfinderWidget(nullptr)
    , m_currentCameraIndex(-1)
    , m_focusCapabilitiesLogged(false)
{
}

QtCameraBackend::~QtCameraBackend()
{
    releaseCamera();
}

CameraBackendType QtCameraBackend::backendType() const
{
    return CameraBackendType::QtMultimedia;
}

QString QtCameraBackend::backendDisplayName() const
{
    return QStringLiteral("Qt Multimedia");
}

QWidget *QtCameraBackend::createPreviewWidget(QWidget *parent)
{
    if (!m_viewfinderWidget) {
        m_viewfinderWidget = new QCameraViewfinder(parent);
        m_viewfinderWidget->setMinimumSize(720, 420);
    } else if (m_viewfinderWidget->parentWidget() != parent) {
        m_viewfinderWidget->setParent(parent);
    }

    return m_viewfinderWidget;
}

QWidget *QtCameraBackend::previewWidget() const
{
    return m_viewfinderWidget;
}

void QtCameraBackend::refreshCameras()
{
    m_cameraInfos = QVector<QCameraInfo>::fromList(QCameraInfo::availableCameras());
    m_cameras.clear();

    for (int index = 0; index < m_cameraInfos.size(); ++index) {
        const QCameraInfo &info = m_cameraInfos.at(index);
        CameraDeviceInfo deviceInfo;
        deviceInfo.id = info.deviceName();
        deviceInfo.description = info.description();
        deviceInfo.displayText = QStringLiteral("%1 [%2] {%3}")
                                     .arg(info.description())
                                     .arg(positionToText(info.position()))
                                     .arg(info.deviceName());
        deviceInfo.preferredDefault = (info.position() == QCamera::BackFace);
        m_cameras.push_back(deviceInfo);
    }

    if (m_cameras.isEmpty()) {
        m_capabilityText = QStringLiteral("未找到摄像头设备。");
        emit logMessage(QStringLiteral("未找到摄像头设备。"));
    } else {
        emit logMessage(QStringLiteral("发现 %1 个摄像头设备。").arg(m_cameras.size()));
    }

    emit camerasChanged();
    emit capabilitiesChanged();
}

void QtCameraBackend::openCamera(int cameraIndex)
{
    if (cameraIndex < 0 || cameraIndex >= m_cameraInfos.size()) {
        emit logMessage(QStringLiteral("请先选择一个摄像头。"));
        return;
    }

    releaseCamera();

    const QCameraInfo cameraInfo = m_cameraInfos.at(cameraIndex);
    emit logMessage(QStringLiteral("正在打开摄像头：%1").arg(cameraInfo.description()));

    m_currentCameraIndex = cameraIndex;
    m_focusCapabilitiesLogged = false;
    m_camera = new QCamera(cameraInfo, this);
    m_camera->setCaptureMode(QCamera::CaptureStillImage);
    if (m_viewfinderWidget) {
        m_camera->setViewfinder(m_viewfinderWidget);
    }

    m_imageCapture = new QCameraImageCapture(m_camera, this);
    m_imageCapture->setCaptureDestination(QCameraImageCapture::CaptureToFile);

    m_videoProbe = new QVideoProbe(this);

    connect(m_camera, &QCamera::statusChanged, this, &QtCameraBackend::onCameraStatusChanged);
    connect(m_camera, &QCamera::stateChanged, this, &QtCameraBackend::onCameraStateChanged);
    connect(m_camera, QOverload<QCamera::Error>::of(&QCamera::error), this, &QtCameraBackend::onCameraError);

    connect(m_imageCapture, &QCameraImageCapture::imageSaved, this, &QtCameraBackend::onImageSaved);
    connect(m_imageCapture,
            QOverload<int, QCameraImageCapture::Error, const QString &>::of(&QCameraImageCapture::error),
            this,
            &QtCameraBackend::onImageCaptureError);
    connect(m_imageCapture, &QCameraImageCapture::readyForCaptureChanged, this, &QtCameraBackend::onReadyForCaptureChanged);

    if (m_videoProbe->setSource(m_camera)) {
        connect(m_videoProbe, &QVideoProbe::videoFrameProbed, this, &QtCameraBackend::onVideoFrameProbed);
        emit logMessage(QStringLiteral("预览帧检测已启用。"));
    } else {
        emit logMessage(QStringLiteral("预览帧检测启用失败。"));
    }

    m_camera->load();
    m_camera->start();
}

void QtCameraBackend::stopCamera()
{
    if (!m_camera) {
        return;
    }

    m_camera->stop();
    emit logMessage(QStringLiteral("摄像头已停止。"));
}

void QtCameraBackend::applySettings(int previewIndex, int captureIndex)
{
    if (!m_camera || !m_imageCapture) {
        emit logMessage(QStringLiteral("请先打开摄像头。"));
        return;
    }

    const bool wasActive = (m_camera->state() == QCamera::ActiveState);
    if (wasActive) {
        m_camera->stop();
    }

    if (previewIndex >= 0 && previewIndex < m_viewfinderSettings.size()) {
        const QCameraViewfinderSettings settings = m_viewfinderSettings.at(previewIndex);
        m_camera->setViewfinderSettings(settings);
        emit logMessage(QStringLiteral("已请求预览档位：%1").arg(viewfinderSettingToText(settings)));
    } else {
        emit logMessage(QStringLiteral("当前无可用预览档位。"));
    }

    if (captureIndex >= 0 && captureIndex < m_captureResolutions.size()) {
        QImageEncoderSettings imageSettings = m_imageCapture->encodingSettings();
        imageSettings.setResolution(m_captureResolutions.at(captureIndex));
        m_imageCapture->setEncodingSettings(imageSettings);
        emit logMessage(QStringLiteral("已请求拍照分辨率：%1").arg(sizeToText(m_captureResolutions.at(captureIndex))));
    } else {
        emit logMessage(QStringLiteral("当前无可用拍照分辨率。"));
    }

    m_camera->start();
}

void QtCameraBackend::captureImage()
{
    if (!m_imageCapture) {
        emit logMessage(QStringLiteral("请先打开摄像头。"));
        return;
    }

    if (!m_imageCapture->isReadyForCapture()) {
        emit logMessage(QStringLiteral("摄像头尚未准备好拍照。"));
        return;
    }

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::currentPath();
    }

    QDir dir(baseDir);
    if (!dir.exists(QStringLiteral("QtCamProbe"))) {
        dir.mkpath(QStringLiteral("QtCamProbe"));
    }

    const QString fileName = dir.filePath(QStringLiteral("QtCamProbe/qt_cam_%1.jpg")
                                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));

    const int captureId = m_imageCapture->capture(fileName);
    if (captureId >= 0) {
        emit logMessage(QStringLiteral("已发起拍照：%1").arg(fileName));
    } else {
        emit logMessage(QStringLiteral("拍照请求失败。"));
    }
}

int QtCameraBackend::defaultCameraIndex() const
{
    for (int index = 0; index < m_cameras.size(); ++index) {
        if (m_cameras.at(index).preferredDefault) {
            return index;
        }
    }

    return m_cameras.isEmpty() ? -1 : 0;
}

bool QtCameraBackend::hasOpenedCamera() const
{
    return (m_camera != nullptr);
}

bool QtCameraBackend::isReadyForCapture() const
{
    return m_imageCapture && m_imageCapture->isReadyForCapture();
}

QVector<CameraDeviceInfo> QtCameraBackend::cameras() const
{
    return m_cameras;
}

QVector<PreviewOption> QtCameraBackend::previewOptions() const
{
    return m_previewOptions;
}

QVector<CaptureOption> QtCameraBackend::captureOptions() const
{
    return m_captureOptions;
}

QString QtCameraBackend::capabilityText() const
{
    return m_capabilityText;
}

void QtCameraBackend::onCameraStatusChanged(QCamera::Status status)
{
    emit statusTextChanged(statusToText(status));
    emit logMessage(QStringLiteral("摄像头流程 => %1").arg(statusToText(status)));

    if (status == QCamera::LoadedStatus || status == QCamera::ActiveStatus) {
        refreshCameraCapabilities();
    }

    if (status == QCamera::ActiveStatus && !m_focusCapabilitiesLogged) {
        logFocusCapabilities();
        m_focusCapabilitiesLogged = true;
    }
}

void QtCameraBackend::onCameraStateChanged(QCamera::State state)
{
    emit stateTextChanged(stateToText(state));
}

void QtCameraBackend::onCameraError(QCamera::Error error)
{
    Q_UNUSED(error)
    if (m_camera) {
        emit logMessage(QStringLiteral("摄像头错误：%1").arg(m_camera->errorString()));
    }
}

void QtCameraBackend::onImageSaved(int id, const QString &fileName)
{
    Q_UNUSED(id)

    const QImage image(fileName);
    const QString photoText = image.isNull()
        ? QStringLiteral("已保存：%1").arg(fileName)
        : QStringLiteral("%1 | %2").arg(sizeToText(image.size())).arg(fileName);

    emit photoTextChanged(photoText);
    emit logMessage(QStringLiteral("照片已保存：%1").arg(photoText));
}

void QtCameraBackend::onImageCaptureError(int id, QCameraImageCapture::Error error, const QString &errorString)
{
    Q_UNUSED(id)
    Q_UNUSED(error)
    emit logMessage(QStringLiteral("拍照错误：%1").arg(errorString));
}

void QtCameraBackend::onReadyForCaptureChanged(bool ready)
{
    emit readyForCaptureChanged(ready);
}

void QtCameraBackend::onVideoFrameProbed(const QVideoFrame &frame)
{
    QSize frameSize(frame.width(), frame.height());
    if (!frameSize.isValid()) {
        frameSize = frame.size();
    }

    if (frameSize.isValid()) {
        emit frameTextChanged(sizeToText(frameSize));
    }
}

void QtCameraBackend::releaseCamera()
{
    if (m_videoProbe) {
        disconnect(m_videoProbe, nullptr, this, nullptr);
        m_videoProbe->blockSignals(true);
        m_videoProbe->setSource(static_cast<QMediaObject *>(nullptr));
    }

    if (m_camera) {
        disconnect(m_camera, nullptr, this, nullptr);
        m_camera->setViewfinder(static_cast<QVideoWidget *>(nullptr));
        m_camera->stop();
        m_camera->unload();
    }

    if (m_imageCapture) {
        disconnect(m_imageCapture, nullptr, this, nullptr);
        delete m_imageCapture;
        m_imageCapture = nullptr;
    }

    if (m_videoProbe) {
        delete m_videoProbe;
        m_videoProbe = nullptr;
    }

    if (m_camera) {
        delete m_camera;
        m_camera = nullptr;
    }

    m_viewfinderSettings.clear();
    m_captureResolutions.clear();
    m_previewOptions.clear();
    m_captureOptions.clear();
    m_capabilityText.clear();
    m_currentCameraIndex = -1;
    m_focusCapabilitiesLogged = false;

    emit stateTextChanged(QStringLiteral("-"));
    emit statusTextChanged(QStringLiteral("-"));
    emit frameTextChanged(QStringLiteral("-"));
    emit photoTextChanged(QStringLiteral("-"));
    emit readyForCaptureChanged(false);
    emit capabilitiesChanged();
}

void QtCameraBackend::refreshCameraCapabilities()
{
    if (!m_camera || !m_imageCapture) {
        return;
    }

    m_viewfinderSettings = m_camera->supportedViewfinderSettings();
    std::sort(m_viewfinderSettings.begin(), m_viewfinderSettings.end(),
              [](const QCameraViewfinderSettings &left, const QCameraViewfinderSettings &right) {
                  const qint64 leftArea = resolutionArea(left.resolution());
                  const qint64 rightArea = resolutionArea(right.resolution());
                  if (leftArea != rightArea) {
                      return leftArea > rightArea;
                  }

                  if (left.maximumFrameRate() != right.maximumFrameRate()) {
                      return left.maximumFrameRate() > right.maximumFrameRate();
                  }

                  return static_cast<int>(left.pixelFormat()) < static_cast<int>(right.pixelFormat());
              });

    m_captureResolutions = m_imageCapture->supportedResolutions();
    std::sort(m_captureResolutions.begin(), m_captureResolutions.end(),
              [](const QSize &left, const QSize &right) {
                  return resolutionArea(left) > resolutionArea(right);
              });

    m_previewOptions.clear();
    for (int index = 0; index < m_viewfinderSettings.size(); ++index) {
        const QCameraViewfinderSettings &setting = m_viewfinderSettings.at(index);
        PreviewOption option;
        option.displayText = viewfinderSettingToText(setting);
        option.resolution = setting.resolution();
        option.minimumFrameRate = setting.minimumFrameRate();
        option.maximumFrameRate = setting.maximumFrameRate();
        option.pixelFormat = static_cast<int>(setting.pixelFormat());
        m_previewOptions.push_back(option);
    }

    m_captureOptions.clear();
    for (int index = 0; index < m_captureResolutions.size(); ++index) {
        CaptureOption option;
        option.resolution = m_captureResolutions.at(index);
        option.displayText = sizeToText(option.resolution);
        m_captureOptions.push_back(option);
    }

    QString text;
    QTextStream stream(&text);

    stream << QStringLiteral("=== 当前摄像头 ===") << "\n";
    if (!m_cameras.isEmpty()) {
        const int cameraIndex = (m_currentCameraIndex >= 0 && m_currentCameraIndex < m_cameras.size())
            ? m_currentCameraIndex
            : (defaultCameraIndex() >= 0 ? defaultCameraIndex() : 0);
        stream << m_cameras.value(cameraIndex).displayText << "\n\n";
    } else {
        stream << QStringLiteral("-") << "\n\n";
    }

    stream << QStringLiteral("=== 预览 (Viewfinder) 列表 ===") << "\n";
    if (m_previewOptions.isEmpty()) {
        stream << QStringLiteral("Qt 没有返回任何 viewfinder 档位。") << "\n";
    } else {
        for (int index = 0; index < m_previewOptions.size(); ++index) {
            stream << index + 1 << ". " << m_previewOptions.at(index).displayText << "\n";
        }
    }

    stream << "\n" << QStringLiteral("=== 拍照 (Capture) 分辨率列表 ===") << "\n";
    if (m_captureOptions.isEmpty()) {
        stream << QStringLiteral("Qt 没有返回任何拍照分辨率。") << "\n";
    } else {
        for (int index = 0; index < m_captureOptions.size(); ++index) {
            stream << index + 1 << ". " << m_captureOptions.at(index).displayText << "\n";
        }
    }

    if (!m_previewOptions.isEmpty()) {
        stream << "\n" << QStringLiteral("Qt 最高预览档位：") << m_previewOptions.first().displayText << "\n";
    }

    if (!m_captureOptions.isEmpty()) {
        stream << QStringLiteral("Qt 最高拍照分辨率：") << m_captureOptions.first().displayText << "\n";
    }

    m_capabilityText = text;
    emit capabilitiesChanged();
}

void QtCameraBackend::logFocusCapabilities()
{
    if (!m_camera) {
        return;
    }

    QCameraFocus *focus = m_camera->focus();
    if (!focus) {
        emit logMessage(QStringLiteral("对焦能力检测：Qt 未返回 QCameraFocus 对象。"));
        return;
    }

    const bool focusAvailable = focus->isAvailable();
    const bool supportsAutoFocus = focus->isFocusModeSupported(QCameraFocus::AutoFocus);
    const bool supportsContinuousFocus = focus->isFocusModeSupported(QCameraFocus::ContinuousFocus);
    const bool supportsCenterPoint = focus->isFocusPointModeSupported(QCameraFocus::FocusPointCenter);
    const bool supportsCustomPoint = focus->isFocusPointModeSupported(QCameraFocus::FocusPointCustom);
    const bool supportsTapToFocus = focusAvailable && supportsCustomPoint
        && (supportsAutoFocus || supportsContinuousFocus);

    emit logMessage(QStringLiteral("=== 对焦能力检测 ==="));
    emit logMessage(QStringLiteral("对焦接口可用：%1").arg(supportToText(focusAvailable)));
    emit logMessage(QStringLiteral("支持自动对焦：%1").arg(supportToText(supportsAutoFocus)));
    emit logMessage(QStringLiteral("支持连续对焦：%1").arg(supportToText(supportsContinuousFocus)));
    emit logMessage(QStringLiteral("支持中心对焦点：%1").arg(supportToText(supportsCenterPoint)));
    emit logMessage(QStringLiteral("支持自定义对焦点：%1").arg(supportToText(supportsCustomPoint)));
    emit logMessage(QStringLiteral("支持点击对焦：%1").arg(supportToText(supportsTapToFocus)));
}
