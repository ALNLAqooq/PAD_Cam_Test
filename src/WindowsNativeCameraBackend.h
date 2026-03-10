#ifndef WINDOWS_NATIVE_CAMERA_BACKEND_H
#define WINDOWS_NATIVE_CAMERA_BACKEND_H

#include "CameraBackend.h"

#include <QImage>
#include <QtGlobal>
#include <atomic>
#include <mutex>
#include <thread>

class QLabel;
class QWidget;

class WindowsNativeCameraBackend : public CameraBackend
{
    Q_OBJECT

public:
    explicit WindowsNativeCameraBackend(QObject *parent = nullptr);
    ~WindowsNativeCameraBackend() override;

    CameraBackendType backendType() const override;
    QString backendDisplayName() const override;

    QWidget *createPreviewWidget(QWidget *parent) override;
    QWidget *previewWidget() const override;

    void refreshCameras() override;
    void openCamera(int cameraIndex) override;
    void stopCamera() override;
    void applySettings(int previewIndex, int captureIndex) override;
    void captureImage() override;

    int defaultCameraIndex() const override;
    bool hasOpenedCamera() const override;
    bool isReadyForCapture() const override;

    QVector<CameraDeviceInfo> cameras() const override;
    QVector<PreviewOption> previewOptions() const override;
    QVector<CaptureOption> captureOptions() const override;
    QString capabilityText() const override;

private:
    struct NativeMode
    {
        quint32 width;
        quint32 height;
        quint32 frameRateNumerator;
        quint32 frameRateDenominator;
    };

    bool loadModesForCamera(const QString &cameraId);
    void previewThreadMain(QString cameraId, QString cameraName);
    void updateCapabilityText();
    void stopPreviewWorker();
    void showPreviewMessage(const QString &message);

    QVector<CameraDeviceInfo> m_cameras;
    QVector<PreviewOption> m_previewOptions;
    QVector<CaptureOption> m_captureOptions;
    QVector<NativeMode> m_nativePreviewModes;
    QLabel *m_placeholderWidget;
    QString m_capabilityText;
    std::thread m_previewThread;
    std::atomic_bool m_stopRequested;
    std::atomic_bool m_isOpened;
    std::atomic_bool m_readyForCapture;
    QString m_openedCameraId;
    QString m_openedCameraName;
    int m_selectedPreviewIndex;
    int m_selectedCaptureIndex;
    int m_currentCameraIndex;
    std::mutex m_frameMutex;
    QImage m_lastFrame;
};

#endif // WINDOWS_NATIVE_CAMERA_BACKEND_H
