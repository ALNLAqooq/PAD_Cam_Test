#include "WindowsNativeCameraBackend.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPixmap>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <windows.h>
#include <dshow.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>

namespace
{
template <typename T>
void safeRelease(T *&pointer)
{
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

QString hrToString(HRESULT result)
{
    return QStringLiteral("0x%1").arg(static_cast<unsigned long>(result), 8, 16, QLatin1Char('0')).toUpper();
}

QString wideToQString(const WCHAR *text, UINT32 length)
{
    if (!text || length == 0) {
        return QString();
    }

    return QString::fromWCharArray(text, static_cast<int>(length));
}

QString sizeToText(int width, int height)
{
    return QStringLiteral("%1 x %2").arg(width).arg(height);
}

QString previewModeToText(quint32 width, quint32 height, quint32 frameRateNumerator, quint32 frameRateDenominator)
{
    const double fps = (frameRateDenominator == 0)
        ? 0.0
        : static_cast<double>(frameRateNumerator) / static_cast<double>(frameRateDenominator);
    return QStringLiteral("%1 | %2 fps | RGB32")
        .arg(sizeToText(static_cast<int>(width), static_cast<int>(height)))
        .arg(fps, 0, 'f', 2);
}

QString supportToText(bool supported)
{
    return supported ? QStringLiteral("支持") : QStringLiteral("不支持");
}

HRESULT enumerateVideoDevices(IMFActivate ***devices, UINT32 *deviceCount)
{
    if (!devices || !deviceCount) {
        return E_POINTER;
    }

    *devices = nullptr;
    *deviceCount = 0;

    IMFAttributes *attributes = nullptr;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }

    if (SUCCEEDED(result)) {
        result = MFEnumDeviceSources(attributes, devices, deviceCount);
    }

    safeRelease(attributes);
    return result;
}

void releaseDeviceArray(IMFActivate **devices, UINT32 deviceCount)
{
    if (!devices) {
        return;
    }

    for (UINT32 index = 0; index < deviceCount; ++index) {
        if (devices[index]) {
            devices[index]->Release();
        }
    }

    CoTaskMemFree(devices);
}
}

WindowsNativeCameraBackend::WindowsNativeCameraBackend(QObject *parent)
    : CameraBackend(parent)
    , m_placeholderWidget(nullptr)
    , m_stopRequested(false)
    , m_isOpened(false)
    , m_readyForCapture(false)
    , m_selectedPreviewIndex(0)
    , m_selectedCaptureIndex(0)
    , m_currentCameraIndex(-1)
    , m_sourceReader(nullptr)
    , m_cameraControl(nullptr)
    , m_ksControl(nullptr)
    , m_focusRequestPending(false)
    , m_focusRequestX(0.5)
    , m_focusRequestY(0.5)
    , m_focusSupported(false)
    , m_focusSupportsAuto(false)
    , m_roiSupported(false)
    , m_focusDefaultValue(0)
    , m_lastFrameTime(0)
    , m_currentFps(0.0)
    , m_fpsTimer(nullptr)
{
    updateCapabilityText();
}

WindowsNativeCameraBackend::~WindowsNativeCameraBackend()
{
    stopPreviewWorker();
}

CameraBackendType WindowsNativeCameraBackend::backendType() const
{
    return CameraBackendType::WindowsNative;
}

QString WindowsNativeCameraBackend::backendDisplayName() const
{
    return QStringLiteral("Windows Native (WIP)");
}

QWidget *WindowsNativeCameraBackend::createPreviewWidget(QWidget *parent)
{
    if (!m_placeholderWidget) {
        m_placeholderWidget = new QLabel(QStringLiteral("Windows 原生相机后端已接入设备枚举。\n点击“打开”后将在这里显示原生预览。"), parent);
        m_placeholderWidget->setAlignment(Qt::AlignCenter);
        m_placeholderWidget->setMinimumSize(720, 420);
        m_placeholderWidget->setStyleSheet(QStringLiteral("QLabel { background: #202020; color: #f0f0f0; border: 1px solid #5a5a5a; }"));
    } else if (m_placeholderWidget->parentWidget() != parent) {
        m_placeholderWidget->setParent(parent);
    }

    return m_placeholderWidget;
}

QWidget *WindowsNativeCameraBackend::previewWidget() const
{
    return m_placeholderWidget;
}

void WindowsNativeCameraBackend::refreshCameras()
{
    m_cameras.clear();
    m_previewOptions.clear();
    m_captureOptions.clear();
    m_nativePreviewModes.clear();

    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = (coInitResult == S_OK || coInitResult == S_FALSE);
    if (FAILED(coInitResult) && coInitResult != RPC_E_CHANGED_MODE) {
        m_capabilityText = QStringLiteral("Windows 原生设备枚举失败：COM 初始化失败（%1）").arg(hrToString(coInitResult));
        emit logMessage(QStringLiteral("Windows 原生设备枚举失败：COM 初始化失败（%1）").arg(hrToString(coInitResult)));
        emit camerasChanged();
        emit capabilitiesChanged();
        return;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startupResult)) {
        m_capabilityText = QStringLiteral("Windows 原生设备枚举失败：MFStartup 失败（%1）").arg(hrToString(startupResult));
        emit logMessage(QStringLiteral("Windows 原生设备枚举失败：MFStartup 失败（%1）").arg(hrToString(startupResult)));
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        emit camerasChanged();
        emit capabilitiesChanged();
        return;
    }

    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;
    const HRESULT result = enumerateVideoDevices(&devices, &deviceCount);

    if (SUCCEEDED(result)) {
        for (UINT32 index = 0; index < deviceCount; ++index) {
            WCHAR *friendlyName = nullptr;
            UINT32 friendlyNameLength = 0;
            WCHAR *symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;

            QString description = QStringLiteral("Unknown Camera");
            QString id = QStringLiteral("native://unknown");

            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                             &friendlyName,
                                                             &friendlyNameLength))) {
                description = wideToQString(friendlyName, friendlyNameLength);
            }

            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                             &symbolicLink,
                                                             &symbolicLinkLength))) {
                id = wideToQString(symbolicLink, symbolicLinkLength);
            }

            CameraDeviceInfo cameraInfo;
            cameraInfo.id = id;
            cameraInfo.description = description;
            cameraInfo.displayText = description;
            cameraInfo.preferredDefault = (index == 0);
            m_cameras.push_back(cameraInfo);

            CoTaskMemFree(friendlyName);
            CoTaskMemFree(symbolicLink);
        }
    } else {
        emit logMessage(QStringLiteral("Windows 原生设备枚举失败（%1）").arg(hrToString(result)));
    }

    releaseDeviceArray(devices, deviceCount);
    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }

    if (!m_cameras.isEmpty()) {
        m_currentCameraIndex = 0;
        loadModesForCamera(m_cameras.first().id);
    } else {
        m_currentCameraIndex = -1;
    }

    updateCapabilityText();
    emit logMessage(QStringLiteral("Windows 原生后端发现 %1 个摄像头设备。").arg(m_cameras.size()));
    emit camerasChanged();
    emit capabilitiesChanged();
}

void WindowsNativeCameraBackend::openCamera(int cameraIndex)
{
    if (cameraIndex < 0 || cameraIndex >= m_cameras.size()) {
        emit logMessage(QStringLiteral("请先选择一个原生摄像头设备。"));
        return;
    }

    if (m_currentCameraIndex != cameraIndex) {
        m_currentCameraIndex = cameraIndex;
        loadModesForCamera(m_cameras.at(cameraIndex).id);
        updateCapabilityText();
        emit capabilitiesChanged();
    }

    stopPreviewWorker();

    m_openedCameraId = m_cameras.at(cameraIndex).id;
    m_openedCameraName = m_cameras.at(cameraIndex).description;
    m_stopRequested = false;

    emit stateTextChanged(QStringLiteral("Loaded"));
    emit statusTextChanged(QStringLiteral("Loading"));
    emit logMessage(QStringLiteral("正在打开原生摄像头：%1").arg(m_openedCameraName));
    showPreviewMessage(QStringLiteral("正在启动原生预览…"));

    m_previewThread = std::thread(&WindowsNativeCameraBackend::previewThreadMain,
                                  this,
                                  m_openedCameraId,
                                  m_openedCameraName);
}

void WindowsNativeCameraBackend::stopCamera()
{
    stopPreviewWorker();
    emit logMessage(QStringLiteral("原生摄像头已停止。"));
}

void WindowsNativeCameraBackend::applySettings(int previewIndex, int captureIndex)
{
    if (m_cameras.isEmpty()) {
        emit logMessage(QStringLiteral("当前没有可用的原生摄像头设备。"));
        return;
    }

    if (!m_nativePreviewModes.isEmpty() && previewIndex >= 0 && previewIndex < m_nativePreviewModes.size()) {
        m_selectedPreviewIndex = previewIndex;
        const NativeMode &mode = m_nativePreviewModes.at(previewIndex);
        emit logMessage(QStringLiteral("已请求原生预览档位：%1")
                            .arg(previewModeToText(mode.width, mode.height, mode.frameRateNumerator, mode.frameRateDenominator)));
    }

    if (!m_captureOptions.isEmpty() && captureIndex >= 0 && captureIndex < m_captureOptions.size()) {
        m_selectedCaptureIndex = captureIndex;
        emit logMessage(QStringLiteral("已请求原生拍照分辨率：%1")
                            .arg(m_captureOptions.at(captureIndex).displayText));
    }

    if (m_isOpened.load()) {
        emit logMessage(QStringLiteral("原生预览将按新档位重启。"));
        openCamera(m_currentCameraIndex >= 0 ? m_currentCameraIndex : 0);
    } else {
        updateCapabilityText();
        emit capabilitiesChanged();
    }
}

void WindowsNativeCameraBackend::captureImage()
{
    QImage frameCopy;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frameCopy = m_lastFrame;
    }

    if (frameCopy.isNull()) {
        emit logMessage(QStringLiteral("原生拍照失败：当前没有可保存的预览帧。"));
        return;
    }

    if (m_selectedCaptureIndex >= 0 && m_selectedCaptureIndex < m_captureOptions.size()) {
        const QSize targetSize = m_captureOptions.at(m_selectedCaptureIndex).resolution;
        if (targetSize.isValid() && frameCopy.size() != targetSize) {
            frameCopy = frameCopy.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::currentPath();
    }

    QDir dir(baseDir);
    if (!dir.exists(QStringLiteral("QtCamProbeNative"))) {
        dir.mkpath(QStringLiteral("QtCamProbeNative"));
    }

    const QString fileName = dir.filePath(QStringLiteral("QtCamProbeNative/native_cam_%1.jpg")
                                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));

    if (frameCopy.save(fileName, "JPG")) {
        emit photoTextChanged(QStringLiteral("%1 | %2").arg(sizeToText(frameCopy.width(), frameCopy.height())).arg(fileName));
        emit logMessage(QStringLiteral("原生拍照已保存：%1").arg(fileName));
    } else {
        emit logMessage(QStringLiteral("原生拍照失败：保存文件失败。"));
    }
}

int WindowsNativeCameraBackend::defaultCameraIndex() const
{
    return m_cameras.isEmpty() ? -1 : 0;
}

bool WindowsNativeCameraBackend::hasOpenedCamera() const
{
    return m_isOpened.load();
}

bool WindowsNativeCameraBackend::isReadyForCapture() const
{
    return m_readyForCapture.load();
}

QVector<CameraDeviceInfo> WindowsNativeCameraBackend::cameras() const
{
    return m_cameras;
}

QVector<PreviewOption> WindowsNativeCameraBackend::previewOptions() const
{
    return m_previewOptions;
}

QVector<CaptureOption> WindowsNativeCameraBackend::captureOptions() const
{
    return m_captureOptions;
}

QString WindowsNativeCameraBackend::capabilityText() const
{
    return m_capabilityText;
}

void WindowsNativeCameraBackend::requestFocusAt(qreal normalizedX, qreal normalizedY)
{
    if (!m_isOpened.load()) {
        emit logMessage(QStringLiteral("点击对焦请求已忽略：请先打开原生相机预览。"));
        return;
    }

    const qreal clampedX = qBound<qreal>(0.0, normalizedX, 1.0);
    const qreal clampedY = qBound<qreal>(0.0, normalizedY, 1.0);

    {
        std::lock_guard<std::mutex> lock(m_focusRequestMutex);
        m_focusRequestX = clampedX;
        m_focusRequestY = clampedY;
    }

    m_focusRequestPending = true;
    emit logMessage(QStringLiteral("已请求点击对焦：x=%1, y=%2")
                        .arg(clampedX, 0, 'f', 3)
                        .arg(clampedY, 0, 'f', 3));
}

bool WindowsNativeCameraBackend::loadModesForCamera(const QString &cameraId)
{
    m_previewOptions.clear();
    m_captureOptions.clear();
    m_nativePreviewModes.clear();
    m_selectedPreviewIndex = 0;
    m_selectedCaptureIndex = 0;
    QVector<NativeMode> discoveredModes;

    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = (coInitResult == S_OK || coInitResult == S_FALSE);
    if (FAILED(coInitResult) && coInitResult != RPC_E_CHANGED_MODE) {
        emit logMessage(QStringLiteral("原生模式枚举失败：COM 初始化失败（%1）").arg(hrToString(coInitResult)));
        return false;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startupResult)) {
        emit logMessage(QStringLiteral("原生模式枚举失败：MFStartup 失败（%1）").arg(hrToString(startupResult)));
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return false;
    }

    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;
    IMFMediaSource *mediaSource = nullptr;
    IMFSourceReader *sourceReader = nullptr;
    IMFAttributes *readerAttributes = nullptr;

    HRESULT result = enumerateVideoDevices(&devices, &deviceCount);
    if (SUCCEEDED(result)) {
        IMFActivate *selectedDevice = nullptr;
        for (UINT32 index = 0; index < deviceCount; ++index) {
            WCHAR *symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;
            QString id;
            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                             &symbolicLink,
                                                             &symbolicLinkLength))) {
                id = wideToQString(symbolicLink, symbolicLinkLength);
            }
            CoTaskMemFree(symbolicLink);

            if (id == cameraId) {
                selectedDevice = devices[index];
                break;
            }
        }

        if (!selectedDevice && deviceCount > 0) {
            selectedDevice = devices[0];
        }

        if (!selectedDevice) {
            result = E_FAIL;
        } else {
            result = selectedDevice->ActivateObject(IID_PPV_ARGS(&mediaSource));
        }
    } else {
        emit logMessage(QStringLiteral("Native mode enum failed: MFEnumDeviceSources => %1").arg(hrToString(result)));
    }

    if (FAILED(result)) {
        emit logMessage(QStringLiteral("Native mode enum failed: ActivateObject => %1").arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        result = MFCreateAttributes(&readerAttributes, 1);
    }
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromMediaSource(mediaSource, readerAttributes, &sourceReader);
    }
    if (FAILED(result)) {
        emit logMessage(QStringLiteral("Native mode enum note: SourceReader init not available (with attributes): %1")
                            .arg(hrToString(result)));
    }
    if (FAILED(result)) {
        safeRelease(readerAttributes);
        result = MFCreateSourceReaderFromMediaSource(mediaSource, nullptr, &sourceReader);
        emit logMessage(QStringLiteral("Native mode enum note: SourceReader init not available (no attributes): %1")
                            .arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        HRESULT selectionResult = sourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(selectionResult)) {
            emit logMessage(QStringLiteral("Native mode enum: SetStreamSelection(all=false) => %1").arg(hrToString(selectionResult)));
        }
        selectionResult = sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (FAILED(selectionResult)) {
            emit logMessage(QStringLiteral("Native mode enum: SetStreamSelection(video=true) => %1").arg(hrToString(selectionResult)));
        }

        DWORD typeIndex = 0;
        bool hasType = false;
        while (true) {
            IMFMediaType *nativeType = nullptr;
            const HRESULT typeResult = sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, typeIndex, &nativeType);
            if (typeResult == MF_E_NO_MORE_TYPES) {
                if (!hasType) {
                    emit logMessage(QStringLiteral("Native mode enum: no native media types returned."));
                }
                break;
            }

            if (FAILED(typeResult)) {
                emit logMessage(QStringLiteral("Native mode enum: GetNativeMediaType failed at index %1 => %2")
                                    .arg(typeIndex)
                                    .arg(hrToString(typeResult)));
                break;
            }

            if (SUCCEEDED(typeResult)) {
                hasType = true;
                GUID majorType = GUID_NULL;
                GUID subtype = GUID_NULL;
                UINT32 width = 0;
                UINT32 height = 0;
                UINT32 frameRateNumerator = 0;
                UINT32 frameRateDenominator = 0;

                HRESULT metaResult = nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
                if (SUCCEEDED(metaResult) && majorType == MFMediaType_Video) {
                    nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
                    MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
                    MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &frameRateNumerator, &frameRateDenominator);

                    if (width > 0 && height > 0) {
                        bool exists = false;
                        for (int existingIndex = 0; existingIndex < discoveredModes.size(); ++existingIndex) {
                            const NativeMode &existingMode = discoveredModes.at(existingIndex);
                            if (existingMode.width == width
                                && existingMode.height == height
                                && existingMode.frameRateNumerator == frameRateNumerator
                                && existingMode.frameRateDenominator == frameRateDenominator) {
                                exists = true;
                                break;
                            }
                        }

                        if (!exists) {
                            NativeMode mode;
                            mode.width = width;
                            mode.height = height;
                            mode.frameRateNumerator = frameRateNumerator;
                            mode.frameRateDenominator = frameRateDenominator == 0 ? 1 : frameRateDenominator;
                            discoveredModes.push_back(mode);
                        }
                    }
                }

                safeRelease(nativeType);
            }

            ++typeIndex;
        }
    }

    safeRelease(sourceReader);
    safeRelease(readerAttributes);
    safeRelease(mediaSource);
    releaseDeviceArray(devices, deviceCount);
    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }

    bool usedMtaFallback = false;
    bool usedDescriptorFallback = false;
    if (discoveredModes.isEmpty()) {
        emit logMessage(QStringLiteral("Native mode enum fallback: retry with MTA thread."));
        usedMtaFallback = true;
        QVector<NativeMode> mtaModes;
        std::thread worker([this, cameraId, &mtaModes]() {
            enumerateNativeModesMta(cameraId, mtaModes);
        });
        worker.join();
        if (!mtaModes.isEmpty()) {
            discoveredModes = mtaModes;
        }
    }
    if (discoveredModes.isEmpty()) {
        emit logMessage(QStringLiteral("Native mode enum fallback: enumerate via IMFMediaSource descriptor."));
        usedDescriptorFallback = true;
        QVector<NativeMode> descriptorModes;
        std::thread worker([this, cameraId, &descriptorModes]() {
            enumerateNativeModesByMediaSourceMta(cameraId, descriptorModes);
        });
        worker.join();
        if (!descriptorModes.isEmpty()) {
            discoveredModes = descriptorModes;
        }
    }
    if (!discoveredModes.isEmpty()) {
        if (usedDescriptorFallback) {
            emit logMessage(QStringLiteral("Native mode enum: using descriptor path; SourceReader warnings can be ignored."));
        } else if (usedMtaFallback) {
            emit logMessage(QStringLiteral("Native mode enum: using MTA SourceReader fallback."));
        } else {
            emit logMessage(QStringLiteral("Native mode enum: using STA SourceReader."));
        }
    }

    rebuildOptionsFromNativeModes(discoveredModes);
    return !m_nativePreviewModes.isEmpty();
}

void WindowsNativeCameraBackend::rebuildOptionsFromNativeModes(const QVector<NativeMode> &modes)
{
    // 去重：使用 QMap 来按 (width, height, fpsNum, fpsDen) 去重
    QVector<NativeMode> deduplicatedModes;
    QSet<QString> seen;
    for (const NativeMode &mode : modes) {
        QString key = QStringLiteral("%1x%2@%3/%4")
                          .arg(mode.width)
                          .arg(mode.height)
                          .arg(mode.frameRateNumerator)
                          .arg(mode.frameRateDenominator);
        if (!seen.contains(key)) {
            seen.insert(key);
            deduplicatedModes.append(mode);
        }
    }

    m_nativePreviewModes = deduplicatedModes;

    m_previewOptions.clear();
    for (int index = 0; index < m_nativePreviewModes.size(); ++index) {
        const NativeMode &mode = m_nativePreviewModes.at(index);
        PreviewOption option;
        option.displayText = previewModeToText(mode.width, mode.height, mode.frameRateNumerator, mode.frameRateDenominator);
        option.resolution = QSize(static_cast<int>(mode.width), static_cast<int>(mode.height));
        option.minimumFrameRate = mode.frameRateDenominator > 0
            ? static_cast<qreal>(mode.frameRateNumerator) / static_cast<qreal>(mode.frameRateDenominator)
            : 0;
        option.maximumFrameRate = option.minimumFrameRate;
        option.pixelFormat = 0;
        m_previewOptions.push_back(option);
    }

    m_captureOptions.clear();
    QVector<QSize> uniqueResolutions;
    for (const NativeMode &mode : m_nativePreviewModes) {
        QSize resolution(static_cast<int>(mode.width), static_cast<int>(mode.height));
        if (!uniqueResolutions.contains(resolution)) {
            uniqueResolutions.append(resolution);
        }
    }
    std::sort(uniqueResolutions.begin(), uniqueResolutions.end(),
              [](const QSize &left, const QSize &right) {
                  return static_cast<qint64>(left.width()) * static_cast<qint64>(left.height())
                       > static_cast<qint64>(right.width()) * static_cast<qint64>(right.height());
              });
    for (const QSize &resolution : uniqueResolutions) {
        CaptureOption option;
        option.resolution = resolution;
        option.displayText = QStringLiteral("%1 x %2").arg(resolution.width()).arg(resolution.height());
        m_captureOptions.push_back(option);
    }

    m_selectedPreviewIndex = 0;
    m_selectedCaptureIndex = 0;

    updateCapabilityText();
    emit capabilitiesChanged();
}

bool WindowsNativeCameraBackend::enumerateNativeModesMta(const QString &cameraId, QVector<NativeMode> &outModes)
{
    outModes.clear();

    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = (coInitResult == S_OK || coInitResult == S_FALSE);
    if (FAILED(coInitResult) && coInitResult != RPC_E_CHANGED_MODE) {
        emit logMessage(QStringLiteral("Native mode enum MTA: CoInitializeEx failed: %1").arg(hrToString(coInitResult)));
        return false;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startupResult)) {
        emit logMessage(QStringLiteral("Native mode enum MTA: MFStartup failed: %1").arg(hrToString(startupResult)));
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return false;
    }

    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;
    IMFMediaSource *mediaSource = nullptr;
    IMFSourceReader *sourceReader = nullptr;
    IMFAttributes *readerAttributes = nullptr;

    HRESULT result = enumerateVideoDevices(&devices, &deviceCount);
    if (SUCCEEDED(result)) {
        IMFActivate *selectedDevice = nullptr;
        for (UINT32 index = 0; index < deviceCount; ++index) {
            WCHAR *symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;
            QString id;
            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                             &symbolicLink,
                                                             &symbolicLinkLength))) {
                id = wideToQString(symbolicLink, symbolicLinkLength);
            }
            CoTaskMemFree(symbolicLink);

            if (id == cameraId) {
                selectedDevice = devices[index];
                break;
            }
        }

        if (!selectedDevice && deviceCount > 0) {
            selectedDevice = devices[0];
        }

        if (!selectedDevice) {
            result = E_FAIL;
        } else {
            result = selectedDevice->ActivateObject(IID_PPV_ARGS(&mediaSource));
        }
    } else {
        emit logMessage(QStringLiteral("Native mode enum MTA: MFEnumDeviceSources => %1").arg(hrToString(result)));
    }

    if (FAILED(result)) {
        emit logMessage(QStringLiteral("Native mode enum MTA: ActivateObject => %1").arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        result = MFCreateAttributes(&readerAttributes, 1);
    }
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromMediaSource(mediaSource, readerAttributes, &sourceReader);
    }
    if (FAILED(result)) {
        safeRelease(readerAttributes);
        result = MFCreateSourceReaderFromMediaSource(mediaSource, nullptr, &sourceReader);
        emit logMessage(QStringLiteral("Native mode enum MTA note: SourceReader init not available (no attributes): %1")
                            .arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        sourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        DWORD typeIndex = 0;
        while (true) {
            IMFMediaType *nativeType = nullptr;
            const HRESULT typeResult = sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, typeIndex, &nativeType);
            if (typeResult == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(typeResult)) {
                emit logMessage(QStringLiteral("Native mode enum MTA: GetNativeMediaType(%1) => %2")
                                    .arg(typeIndex)
                                    .arg(hrToString(typeResult)));
                break;
            }

            GUID majorType = GUID_NULL;
            UINT32 width = 0;
            UINT32 height = 0;
            UINT32 frameRateNumerator = 0;
            UINT32 frameRateDenominator = 0;

            HRESULT metaResult = nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
            if (SUCCEEDED(metaResult) && majorType == MFMediaType_Video) {
                MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
                MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &frameRateNumerator, &frameRateDenominator);
                if (width > 0 && height > 0) {
                    NativeMode mode;
                    mode.width = width;
                    mode.height = height;
                    mode.frameRateNumerator = frameRateNumerator;
                    mode.frameRateDenominator = frameRateDenominator == 0 ? 1 : frameRateDenominator;
                    outModes.push_back(mode);
                }
            }

            safeRelease(nativeType);
            ++typeIndex;
        }
    }

    safeRelease(sourceReader);
    safeRelease(readerAttributes);
    safeRelease(mediaSource);
    releaseDeviceArray(devices, deviceCount);

    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }

    if (outModes.isEmpty()) {
        emit logMessage(QStringLiteral("Native mode enum MTA: no native modes found."));
    }

    return !outModes.isEmpty();
}

bool WindowsNativeCameraBackend::enumerateNativeModesByMediaSourceMta(const QString &cameraId, QVector<NativeMode> &outModes)
{
    outModes.clear();

    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = (coInitResult == S_OK || coInitResult == S_FALSE);
    if (FAILED(coInitResult) && coInitResult != RPC_E_CHANGED_MODE) {
        emit logMessage(QStringLiteral("Native mode enum descriptor: CoInitializeEx failed: %1").arg(hrToString(coInitResult)));
        return false;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startupResult)) {
        emit logMessage(QStringLiteral("Native mode enum descriptor: MFStartup failed: %1").arg(hrToString(startupResult)));
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return false;
    }

    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;
    IMFMediaSource *mediaSource = nullptr;
    IMFPresentationDescriptor *presentation = nullptr;

    HRESULT result = enumerateVideoDevices(&devices, &deviceCount);
    if (SUCCEEDED(result)) {
        IMFActivate *selectedDevice = nullptr;
        for (UINT32 index = 0; index < deviceCount; ++index) {
            WCHAR *symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;
            QString id;
            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                             &symbolicLink,
                                                             &symbolicLinkLength))) {
                id = wideToQString(symbolicLink, symbolicLinkLength);
            }
            CoTaskMemFree(symbolicLink);

            if (id == cameraId) {
                selectedDevice = devices[index];
                break;
            }
        }

        if (!selectedDevice && deviceCount > 0) {
            selectedDevice = devices[0];
        }

        if (!selectedDevice) {
            result = E_FAIL;
        } else {
            result = selectedDevice->ActivateObject(IID_PPV_ARGS(&mediaSource));
        }
    } else {
        emit logMessage(QStringLiteral("Native mode enum descriptor: MFEnumDeviceSources => %1").arg(hrToString(result)));
    }

    if (FAILED(result)) {
        emit logMessage(QStringLiteral("Native mode enum descriptor: ActivateObject => %1").arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        result = mediaSource->CreatePresentationDescriptor(&presentation);
    }
    if (FAILED(result)) {
        emit logMessage(QStringLiteral("Native mode enum descriptor: CreatePresentationDescriptor => %1").arg(hrToString(result)));
    }

    if (SUCCEEDED(result)) {
        DWORD streamCount = 0;
        result = presentation->GetStreamDescriptorCount(&streamCount);
        if (FAILED(result)) {
            emit logMessage(QStringLiteral("Native mode enum descriptor: GetStreamDescriptorCount => %1").arg(hrToString(result)));
        } else {
            for (DWORD streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
                BOOL selected = FALSE;
                IMFStreamDescriptor *streamDesc = nullptr;
                result = presentation->GetStreamDescriptorByIndex(streamIndex, &selected, &streamDesc);
                if (FAILED(result)) {
                    emit logMessage(QStringLiteral("Native mode enum descriptor: GetStreamDescriptorByIndex(%1) => %2")
                                        .arg(streamIndex)
                                        .arg(hrToString(result)));
                    continue;
                }

                IMFMediaTypeHandler *handler = nullptr;
                result = streamDesc->GetMediaTypeHandler(&handler);
                if (FAILED(result)) {
                    emit logMessage(QStringLiteral("Native mode enum descriptor: GetMediaTypeHandler(%1) => %2")
                                        .arg(streamIndex)
                                        .arg(hrToString(result)));
                    safeRelease(streamDesc);
                    continue;
                }

                GUID majorType = GUID_NULL;
                handler->GetMajorType(&majorType);
                if (majorType != MFMediaType_Video) {
                    safeRelease(handler);
                    safeRelease(streamDesc);
                    continue;
                }

                DWORD typeCount = 0;
                result = handler->GetMediaTypeCount(&typeCount);
                if (FAILED(result)) {
                    emit logMessage(QStringLiteral("Native mode enum descriptor: GetMediaTypeCount => %1").arg(hrToString(result)));
                    safeRelease(handler);
                    safeRelease(streamDesc);
                    continue;
                }

                for (DWORD typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
                    IMFMediaType *type = nullptr;
                    result = handler->GetMediaTypeByIndex(typeIndex, &type);
                    if (FAILED(result)) {
                        emit logMessage(QStringLiteral("Native mode enum descriptor: GetMediaTypeByIndex(%1) => %2")
                                            .arg(typeIndex)
                                            .arg(hrToString(result)));
                        continue;
                    }

                    UINT32 width = 0;
                    UINT32 height = 0;
                    UINT32 fpsNum = 0;
                    UINT32 fpsDen = 0;
                    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
                    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fpsNum, &fpsDen);

                    if (width > 0 && height > 0) {
                        NativeMode mode;
                        mode.width = width;
                        mode.height = height;
                        mode.frameRateNumerator = fpsNum;
                        mode.frameRateDenominator = fpsDen == 0 ? 1 : fpsDen;
                        outModes.push_back(mode);
                    }

                    safeRelease(type);
                }

                safeRelease(handler);
                safeRelease(streamDesc);
            }
        }
    }

    safeRelease(presentation);
    safeRelease(mediaSource);
    releaseDeviceArray(devices, deviceCount);

    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }

    if (outModes.isEmpty()) {
        emit logMessage(QStringLiteral("Native mode enum descriptor: no native modes found."));
    }

    return !outModes.isEmpty();
}

void WindowsNativeCameraBackend::previewThreadMain(QString cameraId, QString cameraName)
{
    const HRESULT coInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = (coInitResult == S_OK || coInitResult == S_FALSE);
    if (FAILED(coInitResult) && coInitResult != RPC_E_CHANGED_MODE) {
        emit logMessage(QStringLiteral("原生预览启动失败：COM 初始化失败（%1）").arg(hrToString(coInitResult)));
        emit statusTextChanged(QStringLiteral("Error"));
        showPreviewMessage(QStringLiteral("原生预览启动失败"));
        return;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startupResult)) {
        emit logMessage(QStringLiteral("原生预览启动失败：MFStartup 失败（%1）").arg(hrToString(startupResult)));
        emit statusTextChanged(QStringLiteral("Error"));
        showPreviewMessage(QStringLiteral("原生预览启动失败"));
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return;
    }

    IMFActivate **devices = nullptr;
    UINT32 deviceCount = 0;
    IMFMediaSource *mediaSource = nullptr;
    IMFSourceReader *sourceReader = nullptr;
    IMFAttributes *readerAttributes = nullptr;
    IMFMediaType *requestedType = nullptr;
    IMFMediaType *currentType = nullptr;
    IMFPresentationDescriptor *presentationDesc = nullptr;
    IMFStreamDescriptor *streamDesc = nullptr;
    IMFMediaTypeHandler *mediaTypeHandler = nullptr;

    HRESULT result = enumerateVideoDevices(&devices, &deviceCount);
    if (SUCCEEDED(result)) {
        IMFActivate *selectedDevice = nullptr;
        for (UINT32 index = 0; index < deviceCount; ++index) {
            WCHAR *symbolicLink = nullptr;
            UINT32 symbolicLinkLength = 0;
            QString id;

            if (SUCCEEDED(devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                             &symbolicLink,
                                                             &symbolicLinkLength))) {
                id = wideToQString(symbolicLink, symbolicLinkLength);
            }
            CoTaskMemFree(symbolicLink);

            if (id == cameraId) {
                selectedDevice = devices[index];
                break;
            }
        }

        if (!selectedDevice && deviceCount > 0) {
            selectedDevice = devices[0];
        }

        if (!selectedDevice) {
            result = E_FAIL;
        } else {
            result = selectedDevice->ActivateObject(IID_PPV_ARGS(&mediaSource));
        }
    }

    if (SUCCEEDED(result)) {
        result = mediaSource->CreatePresentationDescriptor(&presentationDesc);
    }

    // 在打开流之前，先通过 StreamDescriptor 设置期望的格式
    UINT32 requestedWidth = 0;
    UINT32 requestedHeight = 0;
    UINT32 requestedFpsNum = 0;
    UINT32 requestedFpsDen = 0;
    if (!m_nativePreviewModes.isEmpty()) {
        const int previewIndex = (m_selectedPreviewIndex >= 0 && m_selectedPreviewIndex < m_nativePreviewModes.size())
            ? m_selectedPreviewIndex
            : 0;
        requestedWidth = m_nativePreviewModes.at(previewIndex).width;
        requestedHeight = m_nativePreviewModes.at(previewIndex).height;
        requestedFpsNum = m_nativePreviewModes.at(previewIndex).frameRateNumerator;
        requestedFpsDen = m_nativePreviewModes.at(previewIndex).frameRateDenominator;

        emit logMessage(QStringLiteral("调试：请求档位索引=%1, 分辨率=%2x%3, 帧率=%4/%5")
                            .arg(previewIndex)
                            .arg(requestedWidth)
                            .arg(requestedHeight)
                            .arg(requestedFpsNum)
                            .arg(requestedFpsDen));
    }

    // 尝试通过 StreamDescriptor 的 MediaTypeHandler 设置格式
    if (SUCCEEDED(result) && presentationDesc && requestedWidth > 0 && requestedHeight > 0) {
        DWORD streamCount = 0;
        if (SUCCEEDED(presentationDesc->GetStreamDescriptorCount(&streamCount)) && streamCount > 0) {
            BOOL selected = FALSE;
            if (SUCCEEDED(presentationDesc->GetStreamDescriptorByIndex(0, &selected, &streamDesc))) {
                if (SUCCEEDED(streamDesc->GetMediaTypeHandler(&mediaTypeHandler))) {
                    // 遍历可用的媒体类型，寻找匹配的格式
                    DWORD typeCount = 0;
                    if (SUCCEEDED(mediaTypeHandler->GetMediaTypeCount(&typeCount))) {
                        bool foundExactMatch = false;
                        for (DWORD i = 0; i < typeCount; ++i) {
                            IMFMediaType *type = nullptr;
                            if (SUCCEEDED(mediaTypeHandler->GetMediaTypeByIndex(i, &type))) {
                                UINT32 w = 0, h = 0;
                                UINT32 fpsNum = 0, fpsDen = 0;
                                if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))) {
                                    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
                                    if (w == requestedWidth && h == requestedHeight) {
                                        // 找到匹配的格式，设置为当前格式
                                        emit logMessage(QStringLiteral("找到匹配的枚举格式 [%1]: %2x%3 @ %4/%5 fps")
                                                            .arg(i).arg(w).arg(h).arg(fpsNum).arg(fpsDen));
                                        mediaTypeHandler->SetCurrentMediaType(type);
                                        foundExactMatch = true;
                                        safeRelease(type);
                                        break;
                                    }
                                }
                                safeRelease(type);
                            }
                        }
                        if (!foundExactMatch) {
                            emit logMessage(QStringLiteral("警告：未在枚举格式中找到 %1x%2 的匹配项").arg(requestedWidth).arg(requestedHeight));
                        }
                    }
                }
            }
        }
    }

    if (SUCCEEDED(result)) {
        result = MFCreateAttributes(&readerAttributes, 1);
    }
    if (SUCCEEDED(result)) {
        result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromMediaSource(mediaSource, readerAttributes, &sourceReader);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateMediaType(&requestedType);
    }
    if (SUCCEEDED(result)) {
        result = requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = requestedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }

    if (SUCCEEDED(result) && requestedWidth > 0 && requestedHeight > 0) {
        result = MFSetAttributeSize(requestedType, MF_MT_FRAME_SIZE, requestedWidth, requestedHeight);
    }
    if (SUCCEEDED(result) && requestedFpsNum > 0 && requestedFpsDen > 0) {
        result = MFSetAttributeRatio(requestedType, MF_MT_FRAME_RATE, requestedFpsNum, requestedFpsDen);
    }

    // 在设置新格式前先取消选择流
    if (SUCCEEDED(result)) {
        sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
    }

    if (SUCCEEDED(result)) {
        result = sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, requestedType);
        if (SUCCEEDED(result)) {
            sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        } else {
            emit logMessage(QStringLiteral("SetCurrentMediaType 失败：%1").arg(hrToString(result)));
        }
    }

    // 如果 SetCurrentMediaType 成功，验证实际设置的格式
    if (SUCCEEDED(result)) {
        IMFMediaType *actualType = nullptr;
        result = sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actualType);
        if (SUCCEEDED(result) && actualType) {
            UINT32 actualWidth = 0, actualHeight = 0;
            UINT32 actualFpsNum = 0, actualFpsDen = 0;
            MFGetAttributeSize(actualType, MF_MT_FRAME_SIZE, &actualWidth, &actualHeight);
            MFGetAttributeRatio(actualType, MF_MT_FRAME_RATE, &actualFpsNum, &actualFpsDen);
            emit logMessage(QStringLiteral("调试：实际设置后的分辨率=%1x%2, 帧率=%3/%4")
                                .arg(actualWidth)
                                .arg(actualHeight)
                                .arg(actualFpsNum)
                                .arg(actualFpsDen));

            // 如果设备拒绝了请求的格式，尝试使用枚举到的第一个匹配分辨率的格式
            if ((actualWidth != requestedWidth || actualHeight != requestedHeight) && !m_nativePreviewModes.isEmpty()) {
                emit logMessage(QStringLiteral("设备未接受请求的格式，尝试查找最接近的可用格式..."));

                // 寻找第一个匹配分辨率的模式
                bool foundExactMatch = false;
                for (int i = 0; i < m_nativePreviewModes.size(); ++i) {
                    const NativeMode &mode = m_nativePreviewModes.at(i);
                    if (mode.width == requestedWidth && mode.height == requestedHeight) {
                        // 找到匹配分辨率的模式，尝试用它的完整参数（包括帧率）
                        IMFMediaType *exactType = nullptr;
                        result = MFCreateMediaType(&exactType);
                        if (SUCCEEDED(result)) {
                            exactType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                            exactType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                            MFSetAttributeSize(exactType, MF_MT_FRAME_SIZE, mode.width, mode.height);
                            MFSetAttributeRatio(exactType, MF_MT_FRAME_RATE, mode.frameRateNumerator, mode.frameRateDenominator);

                            sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, FALSE);
                            result = sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, exactType);
                            if (SUCCEEDED(result)) {
                                sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
                                emit logMessage(QStringLiteral("使用枚举模式 [%1] 重新设置格式：%2x%3 @ %4/%5 fps")
                                                    .arg(i)
                                                    .arg(mode.width)
                                                    .arg(mode.height)
                                                    .arg(mode.frameRateNumerator)
                                                    .arg(mode.frameRateDenominator));
                            }
                            safeRelease(exactType);
                            if (SUCCEEDED(result)) {
                                foundExactMatch = true;
                                break;
                            }
                        }
                    }
                }

                if (!foundExactMatch) {
                    emit logMessage(QStringLiteral("警告：设备可能不支持请求的分辨率 %1x%2").arg(requestedWidth).arg(requestedHeight));
                }
            }

            safeRelease(actualType);
        }
        result = sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
    }

    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 currentFpsNum = 0;
    UINT32 currentFpsDen = 0;
    if (SUCCEEDED(result)) {
        result = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
    }
    if (SUCCEEDED(result)) {
        MFGetAttributeRatio(currentType, MF_MT_FRAME_RATE, &currentFpsNum, &currentFpsDen);
    }

    if (FAILED(result)) {
        emit logMessage(QStringLiteral("原生预览启动失败（%1）").arg(hrToString(result)));
        emit statusTextChanged(QStringLiteral("Error"));
        showPreviewMessage(QStringLiteral("原生预览启动失败"));
    } else {
        setSourceReader(sourceReader);
        m_currentPreviewSize = QSize(static_cast<int>(width), static_cast<int>(height));
        probeFocusCapabilities();
        m_isOpened = true;
        m_readyForCapture = true;
        emit readyForCaptureChanged(true);
        emit stateTextChanged(QStringLiteral("Active"));
        emit statusTextChanged(QStringLiteral("Active"));
        emit frameTextChanged(QStringLiteral("%1 x %2").arg(width).arg(height));
        const QString actualMode = previewModeToText(width, height, currentFpsNum, currentFpsDen == 0 ? 1 : currentFpsDen);
        emit logMessage(QStringLiteral("原生预览已启动：%1 | %2").arg(cameraName, actualMode));
        if (requestedWidth > 0 && requestedHeight > 0
            && (requestedWidth != width || requestedHeight != height)) {
            emit logMessage(QStringLiteral("注意：请求预览档位 %1 x %2，实际输出 %3 x %4。")
                                .arg(requestedWidth)
                                .arg(requestedHeight)
                                .arg(width)
                                .arg(height));
            emit logMessage(QStringLiteral("这是设备驱动的限制：它忽略了我们的格式请求。"));
        }

        // 重置帧率统计
        m_lastFrameTime.store(0);
        m_currentFps.store(0.0);

        // 在主线程启动 FPS 更新定时器（每秒一次）
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_isOpened.load()) {
                return;
            }

            if (m_fpsTimer) {
                m_fpsTimer->stop();
                m_fpsTimer->deleteLater();
                m_fpsTimer = nullptr;
            }

            m_fpsTimer = new QTimer(this);
            connect(m_fpsTimer, &QTimer::timeout, this, [this]() {
                double fps = m_currentFps.load();
                if (fps > 0) {
                    emit fpsChanged(fps);
                }
            });
            m_fpsTimer->start(1000);
        }, Qt::QueuedConnection);
    }

    while (SUCCEEDED(result) && !m_stopRequested.load()) {
        if (m_focusRequestPending.exchange(false)) {
            qreal requestX = 0.5;
            qreal requestY = 0.5;
            {
                std::lock_guard<std::mutex> lock(m_focusRequestMutex);
                requestX = m_focusRequestX;
                requestY = m_focusRequestY;
            }
            applyFocusRequest(requestX, requestY);
        }

        IMFSample *sample = nullptr;
        IMFMediaBuffer *buffer = nullptr;
        BYTE *data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        DWORD streamFlags = 0;

        result = sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          0,
                                          nullptr,
                                          &streamFlags,
                                          nullptr,
                                          &sample);

        if (FAILED(result) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            safeRelease(sample);
            break;
        }

        if (!sample) {
            continue;
        }

        result = sample->ConvertToContiguousBuffer(&buffer);
        if (SUCCEEDED(result)) {
            result = buffer->Lock(&data, &maxLength, &currentLength);
        }

        if (SUCCEEDED(result) && data && width > 0 && height > 0) {
            const int stride = static_cast<int>(currentLength / height);
            QImage frameImage(data, static_cast<int>(width), static_cast<int>(height), stride, QImage::Format_RGB32);
            const QImage frameCopy = frameImage.copy();

            // 计算帧率
            qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
            qint64 lastTime = m_lastFrameTime.load();
            if (lastTime > 0) {
                qint64 diff = currentTime - lastTime;
                if (diff > 0) {
                    double fps = 1000.0 / static_cast<double>(diff);
                    m_currentFps.store(fps);
                }
            }
            m_lastFrameTime.store(currentTime);

            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_lastFrame = frameCopy;
            }

            QMetaObject::invokeMethod(this, [this, frameCopy]() {
                if (!m_placeholderWidget) {
                    return;
                }

                m_placeholderWidget->setPixmap(QPixmap::fromImage(frameCopy).scaled(
                    m_placeholderWidget->size(),
                    Qt::KeepAspectRatio,
                    Qt::SmoothTransformation));
            }, Qt::QueuedConnection);
        }

        if (buffer && data) {
            buffer->Unlock();
        }

        safeRelease(buffer);
        safeRelease(sample);
    }

    m_isOpened = false;
    m_readyForCapture = false;

    clearControlInterfaces();

    safeRelease(currentType);
    safeRelease(requestedType);
    safeRelease(mediaTypeHandler);
    safeRelease(streamDesc);
    safeRelease(presentationDesc);
    safeRelease(sourceReader);
    safeRelease(readerAttributes);
    safeRelease(mediaSource);
    releaseDeviceArray(devices, deviceCount);

    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }

    emit readyForCaptureChanged(false);
    emit stateTextChanged(QStringLiteral("Loaded"));
    emit statusTextChanged(QStringLiteral("Loaded"));
}

void WindowsNativeCameraBackend::setSourceReader(IMFSourceReader *reader)
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_sourceReader = reader;
}

void WindowsNativeCameraBackend::clearControlInterfaces()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    safeRelease(m_cameraControl);
    safeRelease(m_ksControl);
    m_sourceReader = nullptr;
    m_focusSupported = false;
    m_focusSupportsAuto = false;
    m_roiSupported = false;
    m_focusDefaultValue = 0;
    m_currentPreviewSize = QSize();
}

void WindowsNativeCameraBackend::probeFocusCapabilities()
{
    IMFSourceReader *reader = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        reader = m_sourceReader;
    }

    if (!reader) {
        return;
    }

    IAMCameraControl *cameraControl = nullptr;
    IKsControl *ksControl = nullptr;

    HRESULT controlResult = reader->GetServiceForStream(MF_SOURCE_READER_MEDIASOURCE,
                                                        GUID_NULL,
                                                        IID_PPV_ARGS(&cameraControl));
    if (FAILED(controlResult)) {
        controlResult = reader->GetServiceForStream(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                    GUID_NULL,
                                                    IID_PPV_ARGS(&cameraControl));
    }

    if (FAILED(controlResult)) {
        emit logMessage(QStringLiteral("IAMCameraControl 获取失败：%1").arg(hrToString(controlResult)));
    }

    HRESULT ksResult = reader->GetServiceForStream(MF_SOURCE_READER_MEDIASOURCE,
                                                   GUID_NULL,
                                                   IID_PPV_ARGS(&ksControl));
    if (FAILED(ksResult)) {
        ksResult = reader->GetServiceForStream(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               GUID_NULL,
                                               IID_PPV_ARGS(&ksControl));
    }

    if (FAILED(ksResult)) {
        emit logMessage(QStringLiteral("IKsControl 获取失败：%1").arg(hrToString(ksResult)));
    }

    bool focusSupported = false;
    bool focusSupportsAuto = false;
    long focusDefault = 0;

    if (cameraControl) {
        long minValue = 0;
        long maxValue = 0;
        long step = 0;
        long defaultValue = 0;
        long caps = 0;
        HRESULT focusRangeResult = cameraControl->GetRange(CameraControl_Focus,
                                                           &minValue,
                                                           &maxValue,
                                                           &step,
                                                           &defaultValue,
                                                           &caps);
        if (SUCCEEDED(focusRangeResult)) {
            focusSupported = true;
            focusSupportsAuto = (caps & CameraControl_Flags_Auto) != 0;
            focusDefault = defaultValue;
            emit logMessage(QStringLiteral("原生对焦范围：min=%1 max=%2 step=%3 default=%4 flags=0x%5")
                                .arg(minValue)
                                .arg(maxValue)
                                .arg(step)
                                .arg(defaultValue)
                                .arg(QString::number(static_cast<unsigned long>(caps), 16).toUpper()));
        } else {
            emit logMessage(QStringLiteral("对焦范围查询失败：%1").arg(hrToString(focusRangeResult)));
        }
    }

    bool roiSupported = false;
    if (ksControl) {
        KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_S roiInfo = {};
        KSPROPERTY property = {};
        property.Set = PROPSETID_VIDCAP_CAMERACONTROL_REGION_OF_INTEREST;
        property.Id = KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_PROPERTY_ID;
        property.Flags = KSPROPERTY_TYPE_GET;

        ULONG bytesReturned = 0;
        HRESULT roiResult = ksControl->KsProperty(&property,
                                                  sizeof(property),
                                                  &roiInfo,
                                                  sizeof(roiInfo),
                                                  &bytesReturned);
        if (SUCCEEDED(roiResult)) {
            roiSupported = (roiInfo.Capabilities & KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_CONFIG_FOCUS) != 0;
            emit logMessage(QStringLiteral("ROI 能力：cap=0x%1 focus=%2")
                                .arg(QString::number(static_cast<unsigned long>(roiInfo.Capabilities), 16).toUpper())
                                .arg(supportToText(roiSupported)));
        } else {
            emit logMessage(QStringLiteral("ROI 能力查询失败：%1").arg(hrToString(roiResult)));
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        safeRelease(m_cameraControl);
        safeRelease(m_ksControl);
        m_cameraControl = cameraControl;
        m_ksControl = ksControl;
        m_focusSupported = focusSupported;
        m_focusSupportsAuto = focusSupportsAuto;
        m_roiSupported = roiSupported;
        m_focusDefaultValue = focusDefault;
    }

    QMetaObject::invokeMethod(this, [this]() {
        updateCapabilityText();
        emit capabilitiesChanged();
    }, Qt::QueuedConnection);
}

bool WindowsNativeCameraBackend::applyFocusRequest(qreal normalizedX, qreal normalizedY)
{
    bool hasRoi = false;
    bool hasFocus = false;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        hasRoi = m_ksControl && m_roiSupported;
        hasFocus = m_cameraControl && m_focusSupported;
    }

    if (hasRoi && applyRoiFocus(normalizedX, normalizedY)) {
        return true;
    }

    if (hasFocus && applyAutoFocus()) {
        return true;
    }

    emit logMessage(QStringLiteral("点击对焦请求失败：未发现可用的原生对焦接口。"));
    return false;
}

bool WindowsNativeCameraBackend::applyAutoFocus()
{
    IAMCameraControl *cameraControl = nullptr;
    bool supportsAuto = false;
    long defaultValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        cameraControl = m_cameraControl;
        supportsAuto = m_focusSupportsAuto;
        defaultValue = m_focusDefaultValue;
    }

    if (!cameraControl) {
        return false;
    }

    const long flags = supportsAuto ? CameraControl_Flags_Auto : CameraControl_Flags_Manual;
    const HRESULT result = cameraControl->Set(CameraControl_Focus, defaultValue, flags);
    if (FAILED(result)) {
        emit logMessage(QStringLiteral("自动对焦请求失败：%1").arg(hrToString(result)));
        return false;
    }

    emit logMessage(QStringLiteral("已发送原生自动对焦请求。"));
    return true;
}

bool WindowsNativeCameraBackend::applyRoiFocus(qreal normalizedX, qreal normalizedY)
{
    IKsControl *ksControl = nullptr;
    QSize previewSize;
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        ksControl = m_ksControl;
        previewSize = m_currentPreviewSize;
    }

    if (!ksControl || !previewSize.isValid()) {
        emit logMessage(QStringLiteral("ROI 对焦不可用：预览尺寸未知或接口缺失。"));
        return false;
    }

    const int width = previewSize.width();
    const int height = previewSize.height();
    const int roiWidth = qMax(32, width / 5);
    const int roiHeight = qMax(32, height / 5);
    const int centerX = static_cast<int>(normalizedX * width);
    const int centerY = static_cast<int>(normalizedY * height);
    const int left = qBound(0, centerX - roiWidth / 2, width - roiWidth);
    const int top = qBound(0, centerY - roiHeight / 2, height - roiHeight);

    RECT focusRect = {};
    focusRect.left = left;
    focusRect.top = top;
    focusRect.right = left + roiWidth;
    focusRect.bottom = top + roiHeight;

    KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_S roi = {};
    roi.FocusRect = focusRect;
    roi.AutoFocusLock = FALSE;
    roi.AutoExposureLock = FALSE;
    roi.AutoWhitebalanceLock = FALSE;
    roi.Configuration = KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_CONFIG_FOCUS
        | KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_FLAGS_AUTO;

    KSPROPERTY property = {};
    property.Set = PROPSETID_VIDCAP_CAMERACONTROL_REGION_OF_INTEREST;
    property.Id = KSPROPERTY_CAMERACONTROL_REGION_OF_INTEREST_PROPERTY_ID;
    property.Flags = KSPROPERTY_TYPE_SET;

    ULONG bytesReturned = 0;
    const HRESULT result = ksControl->KsProperty(&property,
                                                 sizeof(property),
                                                 &roi,
                                                 sizeof(roi),
                                                 &bytesReturned);
    if (FAILED(result)) {
        emit logMessage(QStringLiteral("ROI 对焦请求失败：%1").arg(hrToString(result)));
        return false;
    }

    emit logMessage(QStringLiteral("已发送 ROI 对焦请求。"));
    return true;
}

void WindowsNativeCameraBackend::updateCapabilityText()
{
    QString text;
    QTextStream stream(&text);

    stream << QStringLiteral("=== Windows 原生相机后端 ===") << "\n";
    stream << QStringLiteral("当前状态：已接入原生设备枚举、基础预览、基础拍照。点击对焦入口已接入。") << "\n\n";
    stream << QStringLiteral("=== 原生枚举到的设备 ===") << "\n";

    if (m_cameras.isEmpty()) {
        stream << QStringLiteral("未枚举到任何摄像头设备。") << "\n";
    } else {
        for (int index = 0; index < m_cameras.size(); ++index) {
            const CameraDeviceInfo &cameraInfo = m_cameras.at(index);
            stream << index + 1 << ". " << cameraInfo.description << "\n";
            stream << QStringLiteral("   id: ") << cameraInfo.id << "\n";
        }
    }

    stream << "\n" << QStringLiteral("=== 原生预览档位 ===") << "\n";
    if (m_previewOptions.isEmpty()) {
        stream << QStringLiteral("尚未枚举到原生预览档位。") << "\n";
    } else {
        for (int index = 0; index < m_previewOptions.size(); ++index) {
            stream << index + 1 << ". " << m_previewOptions.at(index).displayText << "\n";
        }
    }

    stream << "\n" << QStringLiteral("=== 原生拍照分辨率 ===") << "\n";
    if (m_captureOptions.isEmpty()) {
        stream << QStringLiteral("尚未枚举到原生拍照分辨率。") << "\n";
    } else {
        for (int index = 0; index < m_captureOptions.size(); ++index) {
            stream << index + 1 << ". " << m_captureOptions.at(index).displayText << "\n";
        }
    }

    stream << "\n" << QStringLiteral("=== 对焦能力 ===") << "\n";
    stream << QStringLiteral("IAMCameraControl：") << supportToText(m_focusSupported) << "\n";
    stream << QStringLiteral("自动对焦：") << supportToText(m_focusSupportsAuto) << "\n";
    stream << QStringLiteral("ROI 对焦：") << supportToText(m_roiSupported) << "\n";

    stream << "\n" << QStringLiteral("=== 点击对焦状态 ===") << "\n";
    if (m_focusSupported || m_roiSupported) {
        stream << QStringLiteral("已接入点击对焦入口（当前设备可用）。") << "\n";
    } else {
        stream << QStringLiteral("已接入点击对焦入口，但当前设备未暴露对焦控制能力。") << "\n";
    }

    m_capabilityText = text;
}

void WindowsNativeCameraBackend::stopPreviewWorker()
{
    m_stopRequested = true;
    if (m_previewThread.joinable()) {
        m_previewThread.join();
    }

    m_focusRequestPending = false;

    // 停止 FPS 定时器
    if (m_fpsTimer) {
        m_fpsTimer->stop();
        m_fpsTimer->deleteLater();
        m_fpsTimer = nullptr;
    }

    m_isOpened = false;
    m_readyForCapture = false;
    emit readyForCaptureChanged(false);
    emit stateTextChanged(QStringLiteral("-"));
    emit statusTextChanged(QStringLiteral("-"));
    emit frameTextChanged(QStringLiteral("-"));
    emit fpsChanged(0.0);
    showPreviewMessage(QStringLiteral("Windows 原生相机后端已接入设备枚举。\n点击“打开”后将在这里显示原生预览。"));
}

void WindowsNativeCameraBackend::showPreviewMessage(const QString &message)
{
    QMetaObject::invokeMethod(this, [this, message]() {
        if (!m_placeholderWidget) {
            return;
        }

        m_placeholderWidget->setText(message);
        m_placeholderWidget->setPixmap(QPixmap());
    }, Qt::QueuedConnection);
}
