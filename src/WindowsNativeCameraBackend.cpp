#include "WindowsNativeCameraBackend.h"

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMetaObject>
#include <QPixmap>
#include <QStandardPaths>
#include <QTextStream>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfreadwrite.h>

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

bool WindowsNativeCameraBackend::loadModesForCamera(const QString &cameraId)
{
    m_previewOptions.clear();
    m_captureOptions.clear();
    m_nativePreviewModes.clear();
    m_selectedPreviewIndex = 0;
    m_selectedCaptureIndex = 0;

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
        DWORD typeIndex = 0;
        while (true) {
            IMFMediaType *nativeType = nullptr;
            const HRESULT typeResult = sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, typeIndex, &nativeType);
            if (typeResult == MF_E_NO_MORE_TYPES) {
                break;
            }

            if (SUCCEEDED(typeResult)) {
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
                        for (int existingIndex = 0; existingIndex < m_nativePreviewModes.size(); ++existingIndex) {
                            const NativeMode &existingMode = m_nativePreviewModes.at(existingIndex);
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
                            m_nativePreviewModes.push_back(mode);
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

    std::sort(m_nativePreviewModes.begin(), m_nativePreviewModes.end(),
              [](const NativeMode &left, const NativeMode &right) {
                  const qint64 leftArea = static_cast<qint64>(left.width) * static_cast<qint64>(left.height);
                  const qint64 rightArea = static_cast<qint64>(right.width) * static_cast<qint64>(right.height);
                  if (leftArea != rightArea) {
                      return leftArea > rightArea;
                  }
                  const double leftFps = static_cast<double>(left.frameRateNumerator) / static_cast<double>(left.frameRateDenominator == 0 ? 1 : left.frameRateDenominator);
                  const double rightFps = static_cast<double>(right.frameRateNumerator) / static_cast<double>(right.frameRateDenominator == 0 ? 1 : right.frameRateDenominator);
                  return leftFps > rightFps;
              });

    for (int index = 0; index < m_nativePreviewModes.size(); ++index) {
        const NativeMode &mode = m_nativePreviewModes.at(index);
        PreviewOption preview;
        preview.displayText = previewModeToText(mode.width, mode.height, mode.frameRateNumerator, mode.frameRateDenominator);
        preview.resolution = QSize(static_cast<int>(mode.width), static_cast<int>(mode.height));
        preview.minimumFrameRate = (mode.frameRateDenominator == 0)
            ? 0.0
            : static_cast<qreal>(mode.frameRateNumerator) / static_cast<qreal>(mode.frameRateDenominator);
        preview.maximumFrameRate = preview.minimumFrameRate;
        preview.pixelFormat = 0;
        m_previewOptions.push_back(preview);

        bool hasCaptureResolution = false;
        for (int captureIndex = 0; captureIndex < m_captureOptions.size(); ++captureIndex) {
            if (m_captureOptions.at(captureIndex).resolution == preview.resolution) {
                hasCaptureResolution = true;
                break;
            }
        }

        if (!hasCaptureResolution) {
            CaptureOption capture;
            capture.resolution = preview.resolution;
            capture.displayText = sizeToText(capture.resolution.width(), capture.resolution.height());
            m_captureOptions.push_back(capture);
        }
    }

    return !m_nativePreviewModes.isEmpty();
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

    UINT32 requestedWidth = 0;
    UINT32 requestedHeight = 0;
    if (!m_nativePreviewModes.isEmpty()) {
        const int previewIndex = (m_selectedPreviewIndex >= 0 && m_selectedPreviewIndex < m_nativePreviewModes.size())
            ? m_selectedPreviewIndex
            : 0;
        requestedWidth = m_nativePreviewModes.at(previewIndex).width;
        requestedHeight = m_nativePreviewModes.at(previewIndex).height;
    }

    if (SUCCEEDED(result) && requestedWidth > 0 && requestedHeight > 0) {
        result = MFSetAttributeSize(requestedType, MF_MT_FRAME_SIZE, requestedWidth, requestedHeight);
    }
    if (SUCCEEDED(result)) {
        result = sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, requestedType);
    }
    if (SUCCEEDED(result)) {
        result = sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType);
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(result)) {
        result = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
    }

    if (FAILED(result)) {
        emit logMessage(QStringLiteral("原生预览启动失败（%1）").arg(hrToString(result)));
        emit statusTextChanged(QStringLiteral("Error"));
        showPreviewMessage(QStringLiteral("原生预览启动失败"));
    } else {
        m_isOpened = true;
        m_readyForCapture = true;
        emit readyForCaptureChanged(true);
        emit stateTextChanged(QStringLiteral("Active"));
        emit statusTextChanged(QStringLiteral("Active"));
        emit frameTextChanged(QStringLiteral("%1 x %2").arg(width).arg(height));
        emit logMessage(QStringLiteral("原生预览已启动：%1").arg(cameraName));
    }

    while (SUCCEEDED(result) && !m_stopRequested.load()) {
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

    safeRelease(currentType);
    safeRelease(requestedType);
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

void WindowsNativeCameraBackend::updateCapabilityText()
{
    QString text;
    QTextStream stream(&text);

    stream << QStringLiteral("=== Windows 原生相机后端 ===") << "\n";
    stream << QStringLiteral("当前状态：已接入原生设备枚举、基础预览、基础拍照。点击对焦尚未接入。") << "\n\n";
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

    stream << "\n" << QStringLiteral("规划中的下一步：") << "\n";
    stream << QStringLiteral("1. 接入点击对焦 / ROI 对焦") << "\n";

    m_capabilityText = text;
}

void WindowsNativeCameraBackend::stopPreviewWorker()
{
    m_stopRequested = true;
    if (m_previewThread.joinable()) {
        m_previewThread.join();
    }

    m_isOpened = false;
    m_readyForCapture = false;
    emit readyForCaptureChanged(false);
    emit stateTextChanged(QStringLiteral("-"));
    emit statusTextChanged(QStringLiteral("-"));
    emit frameTextChanged(QStringLiteral("-"));
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
