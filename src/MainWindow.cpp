#include "MainWindow.h"

#include <QCameraFocus>
#include <QCameraViewfinder>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QImageEncoderSettings>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoProbe>

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

QString supportToText(bool supported)
{
    return supported ? QStringLiteral("支持") : QStringLiteral("不支持");
}

int defaultRearCameraIndex(const QVector<QCameraInfo> &cameras)
{
    for (int index = 0; index < cameras.size(); ++index) {
        if (cameras.at(index).position() == QCamera::BackFace) {
            return index;
        }
    }

    return cameras.isEmpty() ? -1 : 0;
}
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , m_camera(0)
    , m_imageCapture(0)
    , m_videoProbe(0)
    , m_cameraCombo(0)
    , m_viewfinderCombo(0)
    , m_captureCombo(0)
    , m_refreshButton(0)
    , m_openButton(0)
    , m_stopButton(0)
    , m_applyHighestButton(0)
    , m_applySelectedButton(0)
    , m_captureButton(0)
    , m_viewfinderWidget(0)
    , m_stateValueLabel(0)
    , m_statusValueLabel(0)
    , m_frameValueLabel(0)
    , m_photoValueLabel(0)
    , m_readyValueLabel(0)
    , m_capabilityEdit(0)
    , m_logEdit(0)
    , m_focusCapabilitiesLogged(false)
{
    setupUi();
    refreshCameras();
}

MainWindow::~MainWindow()
{
    releaseCamera();
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("PAD 摄像头分辨率探测"));
    resize(1280, 820);

    m_refreshButton = new QPushButton(QStringLiteral("刷新摄像头"), this);
    m_cameraCombo = new QComboBox(this);
    m_openButton = new QPushButton(QStringLiteral("打开"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_applyHighestButton = new QPushButton(QStringLiteral("应用最高档"), this);
    m_applySelectedButton = new QPushButton(QStringLiteral("应用当前选中"), this);
    m_captureButton = new QPushButton(QStringLiteral("拍照"), this);

    QHBoxLayout *topLayout = new QHBoxLayout;
    topLayout->addWidget(m_refreshButton);
    topLayout->addWidget(m_cameraCombo, 1);
    topLayout->addWidget(m_openButton);
    topLayout->addWidget(m_stopButton);
    topLayout->addWidget(m_applyHighestButton);
    topLayout->addWidget(m_applySelectedButton);
    topLayout->addWidget(m_captureButton);

    m_viewfinderWidget = new QCameraViewfinder(this);
    m_viewfinderWidget->setMinimumSize(720, 420);

    m_stateValueLabel = new QLabel(QStringLiteral("-"), this);
    m_statusValueLabel = new QLabel(QStringLiteral("-"), this);
    m_frameValueLabel = new QLabel(QStringLiteral("-"), this);
    m_photoValueLabel = new QLabel(QStringLiteral("-"), this);
    m_readyValueLabel = new QLabel(QStringLiteral("-"), this);

    QFormLayout *infoLayout = new QFormLayout;
    infoLayout->addRow(QStringLiteral("状态"), m_stateValueLabel);
    infoLayout->addRow(QStringLiteral("流程"), m_statusValueLabel);
    infoLayout->addRow(QStringLiteral("预览帧"), m_frameValueLabel);
    infoLayout->addRow(QStringLiteral("最近照片"), m_photoValueLabel);
    infoLayout->addRow(QStringLiteral("可拍照"), m_readyValueLabel);

    QWidget *leftPanel = new QWidget(this);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->addWidget(m_viewfinderWidget, 1);
    leftLayout->addLayout(infoLayout);

    m_viewfinderCombo = new QComboBox(this);
    m_captureCombo = new QComboBox(this);

    QFormLayout *selectionLayout = new QFormLayout;
    selectionLayout->addRow(QStringLiteral("预览档位"), m_viewfinderCombo);
    selectionLayout->addRow(QStringLiteral("拍照档位"), m_captureCombo);

    QGroupBox *selectionBox = new QGroupBox(QStringLiteral("请求设置"), this);
    selectionBox->setLayout(selectionLayout);

    m_capabilityEdit = new QPlainTextEdit(this);
    m_capabilityEdit->setReadOnly(true);

    QGroupBox *capabilityBox = new QGroupBox(QStringLiteral("Qt 探测到的能力"), this);
    QVBoxLayout *capabilityLayout = new QVBoxLayout(capabilityBox);
    capabilityLayout->addWidget(m_capabilityEdit);

    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);

    QGroupBox *logBox = new QGroupBox(QStringLiteral("运行日志"), this);
    QVBoxLayout *logLayout = new QVBoxLayout(logBox);
    logLayout->addWidget(m_logEdit);

    QWidget *rightPanel = new QWidget(this);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->addWidget(selectionBox);
    rightLayout->addWidget(capabilityBox, 1);
    rightLayout->addWidget(logBox, 1);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);

    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->addLayout(topLayout);
    rootLayout->addWidget(splitter, 1);
    setLayout(rootLayout);

    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::refreshCameras);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::openSelectedCamera);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopCamera);
    connect(m_applyHighestButton, &QPushButton::clicked, this, &MainWindow::applyHighestSettings);
    connect(m_applySelectedButton, &QPushButton::clicked, this, &MainWindow::applySelectedSettings);
    connect(m_captureButton, &QPushButton::clicked, this, &MainWindow::captureImage);

    updateButtonStates();
    logMessage(QStringLiteral("程序已启动。"));
}

void MainWindow::refreshCameras()
{
    m_cameraCombo->clear();
    m_cameras = QVector<QCameraInfo>::fromList(QCameraInfo::availableCameras());

    for (int index = 0; index < m_cameras.size(); ++index) {
        const QCameraInfo &info = m_cameras.at(index);
        const QString text = QStringLiteral("%1 [%2] {%3}")
                                 .arg(info.description())
                                 .arg(positionToText(info.position()))
                                 .arg(info.deviceName());
        m_cameraCombo->addItem(text);
    }

    if (m_cameras.isEmpty()) {
        m_capabilityEdit->setPlainText(QStringLiteral("未找到摄像头设备。"));
        logMessage(QStringLiteral("未找到摄像头设备。"));
        updateButtonStates();
        return;
    }

    m_cameraCombo->setCurrentIndex(defaultRearCameraIndex(m_cameras));
    logMessage(QStringLiteral("发现 %1 个摄像头设备。").arg(m_cameras.size()));
    updateButtonStates();
}

void MainWindow::openSelectedCamera()
{
    const int cameraIndex = m_cameraCombo->currentIndex();
    if (cameraIndex < 0 || cameraIndex >= m_cameras.size()) {
        logMessage(QStringLiteral("请先选择一个摄像头。"));
        return;
    }

    releaseCamera();

    const QCameraInfo cameraInfo = m_cameras.at(cameraIndex);
    logMessage(QStringLiteral("正在打开摄像头：%1").arg(cameraInfo.description()));

    m_focusCapabilitiesLogged = false;
    m_camera = new QCamera(cameraInfo, this);
    m_camera->setCaptureMode(QCamera::CaptureStillImage);
    m_camera->setViewfinder(m_viewfinderWidget);

    m_imageCapture = new QCameraImageCapture(m_camera, this);
    m_imageCapture->setCaptureDestination(QCameraImageCapture::CaptureToFile);

    m_videoProbe = new QVideoProbe(this);

    connect(m_camera, &QCamera::statusChanged, this, &MainWindow::onCameraStatusChanged);
    connect(m_camera, &QCamera::stateChanged, this, &MainWindow::onCameraStateChanged);
    connect(m_camera, QOverload<QCamera::Error>::of(&QCamera::error), this, &MainWindow::onCameraError);

    connect(m_imageCapture, &QCameraImageCapture::imageSaved, this, &MainWindow::onImageSaved);
    connect(m_imageCapture,
            QOverload<int, QCameraImageCapture::Error, const QString &>::of(&QCameraImageCapture::error),
            this,
            &MainWindow::onImageCaptureError);
    connect(m_imageCapture, &QCameraImageCapture::readyForCaptureChanged, this, &MainWindow::onReadyForCaptureChanged);

    if (m_videoProbe->setSource(m_camera)) {
        connect(m_videoProbe, &QVideoProbe::videoFrameProbed, this, &MainWindow::onVideoFrameProbed);
        logMessage(QStringLiteral("预览帧检测已启用。"));
    } else {
        logMessage(QStringLiteral("预览帧检测启用失败。"));
    }

    m_camera->load();
    m_camera->start();
    updateButtonStates();
}

void MainWindow::stopCamera()
{
    if (!m_camera) {
        return;
    }

    m_camera->stop();
    logMessage(QStringLiteral("摄像头已停止。"));
    updateButtonStates();
}

void MainWindow::applyHighestSettings()
{
    if (m_viewfinderCombo->count() > 0) {
        m_viewfinderCombo->setCurrentIndex(0);
    }

    if (m_captureCombo->count() > 0) {
        m_captureCombo->setCurrentIndex(0);
    }

    applySelectedSettings();
}

void MainWindow::applySelectedSettings()
{
    if (!m_camera || !m_imageCapture) {
        logMessage(QStringLiteral("请先打开摄像头。"));
        return;
    }

    const int viewfinderIndex = m_viewfinderCombo->currentIndex();
    const int captureIndex = m_captureCombo->currentIndex();
    const bool wasActive = (m_camera->state() == QCamera::ActiveState);

    if (wasActive) {
        m_camera->stop();
    }

    if (viewfinderIndex >= 0 && viewfinderIndex < m_viewfinderSettings.size()) {
        const QCameraViewfinderSettings settings = m_viewfinderSettings.at(viewfinderIndex);
        m_camera->setViewfinderSettings(settings);
        logMessage(QStringLiteral("已请求预览档位：%1").arg(viewfinderSettingToText(settings)));
    } else {
        logMessage(QStringLiteral("当前无可用预览档位。"));
    }

    if (captureIndex >= 0 && captureIndex < m_captureResolutions.size()) {
        QImageEncoderSettings imageSettings = m_imageCapture->encodingSettings();
        imageSettings.setResolution(m_captureResolutions.at(captureIndex));
        m_imageCapture->setEncodingSettings(imageSettings);
        logMessage(QStringLiteral("已请求拍照分辨率：%1").arg(sizeToText(m_captureResolutions.at(captureIndex))));
    } else {
        logMessage(QStringLiteral("当前无可用拍照分辨率。"));
    }

    m_camera->start();
    updateButtonStates();
}

void MainWindow::captureImage()
{
    if (!m_imageCapture) {
        logMessage(QStringLiteral("请先打开摄像头。"));
        return;
    }

    if (!m_imageCapture->isReadyForCapture()) {
        logMessage(QStringLiteral("摄像头尚未准备好拍照。"));
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
        logMessage(QStringLiteral("已发起拍照：%1").arg(fileName));
    } else {
        logMessage(QStringLiteral("拍照请求失败。"));
    }
}

void MainWindow::onCameraStatusChanged(QCamera::Status status)
{
    m_statusValueLabel->setText(statusToText(status));
    logMessage(QStringLiteral("摄像头流程 => %1").arg(statusToText(status)));

    if (status == QCamera::LoadedStatus || status == QCamera::ActiveStatus) {
        refreshCameraCapabilities();
    }

    if (status == QCamera::ActiveStatus && !m_focusCapabilitiesLogged) {
        logFocusCapabilities();
        m_focusCapabilitiesLogged = true;
    }
}

void MainWindow::onCameraStateChanged(QCamera::State state)
{
    m_stateValueLabel->setText(stateToText(state));
    updateButtonStates();
}

void MainWindow::onCameraError(QCamera::Error error)
{
    Q_UNUSED(error)
    if (m_camera) {
        logMessage(QStringLiteral("摄像头错误：%1").arg(m_camera->errorString()));
    }
}

void MainWindow::onImageSaved(int id, const QString &fileName)
{
    Q_UNUSED(id)

    QImage image(fileName);
    const QString imageText = image.isNull()
        ? QStringLiteral("已保存：%1").arg(fileName)
        : QStringLiteral("%1 | %2").arg(sizeToText(image.size())).arg(fileName);

    m_photoValueLabel->setText(imageText);
    logMessage(QStringLiteral("照片已保存：%1").arg(imageText));
}

void MainWindow::onImageCaptureError(int id, QCameraImageCapture::Error error, const QString &errorString)
{
    Q_UNUSED(id)
    Q_UNUSED(error)
    logMessage(QStringLiteral("拍照错误：%1").arg(errorString));
}

void MainWindow::onReadyForCaptureChanged(bool ready)
{
    m_readyValueLabel->setText(ready ? QStringLiteral("是") : QStringLiteral("否"));
    updateButtonStates();
}

void MainWindow::onVideoFrameProbed(const QVideoFrame &frame)
{
    QSize frameSize(frame.width(), frame.height());
    if (!frameSize.isValid()) {
        frameSize = frame.size();
    }

    if (frameSize.isValid()) {
        m_frameValueLabel->setText(sizeToText(frameSize));
    }
}

void MainWindow::releaseCamera()
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
        m_imageCapture = 0;
    }

    if (m_videoProbe) {
        delete m_videoProbe;
        m_videoProbe = 0;
    }

    if (m_camera) {
        delete m_camera;
        m_camera = 0;
    }

    m_viewfinderSettings.clear();
    m_captureResolutions.clear();
    m_viewfinderCombo->clear();
    m_captureCombo->clear();
    m_stateValueLabel->setText(QStringLiteral("-"));
    m_statusValueLabel->setText(QStringLiteral("-"));
    m_frameValueLabel->setText(QStringLiteral("-"));
    m_photoValueLabel->setText(QStringLiteral("-"));
    m_readyValueLabel->setText(QStringLiteral("-"));
    m_capabilityEdit->clear();
    m_focusCapabilitiesLogged = false;
}

void MainWindow::refreshCameraCapabilities()
{
    if (!m_camera || !m_imageCapture) {
        return;
    }

    const QString selectedViewfinderText = m_viewfinderCombo->currentText();
    const QString selectedCaptureText = m_captureCombo->currentText();

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

    m_viewfinderCombo->clear();
    for (int index = 0; index < m_viewfinderSettings.size(); ++index) {
        m_viewfinderCombo->addItem(viewfinderSettingToText(m_viewfinderSettings.at(index)));
    }

    m_captureCombo->clear();
    for (int index = 0; index < m_captureResolutions.size(); ++index) {
        m_captureCombo->addItem(sizeToText(m_captureResolutions.at(index)));
    }

    if (!m_viewfinderSettings.isEmpty()) {
        const int viewfinderIndex = selectedViewfinderText.isEmpty()
            ? 0
            : m_viewfinderCombo->findText(selectedViewfinderText);
        m_viewfinderCombo->setCurrentIndex(viewfinderIndex >= 0 ? viewfinderIndex : 0);
    }

    if (!m_captureResolutions.isEmpty()) {
        const int captureIndex = selectedCaptureText.isEmpty()
            ? 0
            : m_captureCombo->findText(selectedCaptureText);
        m_captureCombo->setCurrentIndex(captureIndex >= 0 ? captureIndex : 0);
    }

    updateCapabilityText();
    updateButtonStates();
}

void MainWindow::logFocusCapabilities()
{
    if (!m_camera) {
        return;
    }

    QCameraFocus *focus = m_camera->focus();
    if (!focus) {
        logMessage(QStringLiteral("对焦能力检测：Qt 未返回 QCameraFocus 对象。"));
        return;
    }

    const bool focusAvailable = focus->isAvailable();
    const bool supportsAutoFocus = focus->isFocusModeSupported(QCameraFocus::AutoFocus);
    const bool supportsContinuousFocus = focus->isFocusModeSupported(QCameraFocus::ContinuousFocus);
    const bool supportsCenterPoint = focus->isFocusPointModeSupported(QCameraFocus::FocusPointCenter);
    const bool supportsCustomPoint = focus->isFocusPointModeSupported(QCameraFocus::FocusPointCustom);
    const bool supportsTapToFocus = focusAvailable && supportsCustomPoint
        && (supportsAutoFocus || supportsContinuousFocus);

    logMessage(QStringLiteral("=== 对焦能力检测 ==="));
    logMessage(QStringLiteral("对焦接口可用：%1").arg(supportToText(focusAvailable)));
    logMessage(QStringLiteral("支持自动对焦：%1").arg(supportToText(supportsAutoFocus)));
    logMessage(QStringLiteral("支持连续对焦：%1").arg(supportToText(supportsContinuousFocus)));
    logMessage(QStringLiteral("支持中心对焦点：%1").arg(supportToText(supportsCenterPoint)));
    logMessage(QStringLiteral("支持自定义对焦点：%1").arg(supportToText(supportsCustomPoint)));
    logMessage(QStringLiteral("支持点击对焦：%1").arg(supportToText(supportsTapToFocus)));
}

void MainWindow::updateCapabilityText()
{
    QString text;
    QTextStream stream(&text);

    stream << QStringLiteral("=== 当前摄像头 ===") << "\n";
    if (m_cameraCombo->currentIndex() >= 0 && m_cameraCombo->currentIndex() < m_cameraCombo->count()) {
        stream << m_cameraCombo->currentText() << "\n\n";
    }

    stream << QStringLiteral("=== 预览 (Viewfinder) 列表 ===") << "\n";
    if (m_viewfinderSettings.isEmpty()) {
        stream << QStringLiteral("Qt 没有返回任何 viewfinder 档位。") << "\n";
    } else {
        for (int index = 0; index < m_viewfinderSettings.size(); ++index) {
            stream << index + 1 << ". " << viewfinderSettingToText(m_viewfinderSettings.at(index)) << "\n";
        }
    }

    stream << "\n" << QStringLiteral("=== 拍照 (Capture) 分辨率列表 ===") << "\n";
    if (m_captureResolutions.isEmpty()) {
        stream << QStringLiteral("Qt 没有返回任何拍照分辨率。") << "\n";
    } else {
        for (int index = 0; index < m_captureResolutions.size(); ++index) {
            stream << index + 1 << ". " << sizeToText(m_captureResolutions.at(index)) << "\n";
        }
    }

    if (!m_viewfinderSettings.isEmpty()) {
        stream << "\n" << QStringLiteral("Qt 最高预览档位：")
               << viewfinderSettingToText(m_viewfinderSettings.first()) << "\n";
    }

    if (!m_captureResolutions.isEmpty()) {
        stream << QStringLiteral("Qt 最高拍照分辨率：") << sizeToText(m_captureResolutions.first()) << "\n";
    }

    m_capabilityEdit->setPlainText(text);
}

void MainWindow::updateButtonStates()
{
    const bool hasCameraList = !m_cameras.isEmpty();
    const bool hasOpenedCamera = (m_camera != 0);
    const bool hasViewfinderChoices = !m_viewfinderSettings.isEmpty();
    const bool hasCaptureChoices = !m_captureResolutions.isEmpty();
    const bool canCapture = (m_imageCapture != 0) && m_imageCapture->isReadyForCapture();

    m_openButton->setEnabled(hasCameraList);
    m_stopButton->setEnabled(hasOpenedCamera);
    m_applyHighestButton->setEnabled(hasOpenedCamera && (hasViewfinderChoices || hasCaptureChoices));
    m_applySelectedButton->setEnabled(hasOpenedCamera && (hasViewfinderChoices || hasCaptureChoices));
    m_captureButton->setEnabled(canCapture);
}

void MainWindow::logMessage(const QString &message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                             .arg(message);
    m_logEdit->appendPlainText(line);
}
