#ifndef QT_CAMERA_BACKEND_H
#define QT_CAMERA_BACKEND_H

#include "CameraBackend.h"

#include <QCamera>
#include <QCameraImageCapture>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QList>

class QCameraViewfinder;
class QVideoFrame;
class QVideoProbe;
class QWidget;

class QtCameraBackend : public CameraBackend
{
    Q_OBJECT

public:
    explicit QtCameraBackend(QObject *parent = nullptr);
    ~QtCameraBackend() override;

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

private slots:
    void onCameraStatusChanged(QCamera::Status status);
    void onCameraStateChanged(QCamera::State state);
    void onCameraError(QCamera::Error error);
    void onImageSaved(int id, const QString &fileName);
    void onImageCaptureError(int id, QCameraImageCapture::Error error, const QString &errorString);
    void onReadyForCaptureChanged(bool ready);
    void onVideoFrameProbed(const QVideoFrame &frame);

private:
    void releaseCamera();
    void refreshCameraCapabilities();
    void logFocusCapabilities();

    QVector<QCameraInfo> m_cameraInfos;
    QVector<CameraDeviceInfo> m_cameras;
    QVector<PreviewOption> m_previewOptions;
    QVector<CaptureOption> m_captureOptions;
    QList<QCameraViewfinderSettings> m_viewfinderSettings;
    QList<QSize> m_captureResolutions;

    QCamera *m_camera;
    QCameraImageCapture *m_imageCapture;
    QVideoProbe *m_videoProbe;
    QCameraViewfinder *m_viewfinderWidget;

    QString m_capabilityText;
    int m_currentCameraIndex;
    bool m_focusCapabilitiesLogged;
};

#endif // QT_CAMERA_BACKEND_H
