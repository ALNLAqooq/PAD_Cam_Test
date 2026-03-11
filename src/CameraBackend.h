#ifndef CAMERA_BACKEND_H
#define CAMERA_BACKEND_H

#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

class QWidget;

struct CameraDeviceInfo
{
    QString id;
    QString displayText;
    QString description;
    bool preferredDefault;
};

struct PreviewOption
{
    QString displayText;
    QSize resolution;
    qreal minimumFrameRate;
    qreal maximumFrameRate;
    int pixelFormat;
};

struct CaptureOption
{
    QString displayText;
    QSize resolution;
};

enum class CameraBackendType
{
    QtMultimedia = 0,
    WindowsNative = 1
};

class CameraBackend : public QObject
{
    Q_OBJECT

public:
    explicit CameraBackend(QObject *parent = nullptr);
    ~CameraBackend() override;

    virtual CameraBackendType backendType() const = 0;
    virtual QString backendDisplayName() const = 0;

    virtual QWidget *createPreviewWidget(QWidget *parent) = 0;
    virtual QWidget *previewWidget() const = 0;

    virtual void refreshCameras() = 0;
    virtual void openCamera(int cameraIndex) = 0;
    virtual void stopCamera() = 0;
    virtual void applySettings(int previewIndex, int captureIndex) = 0;
    virtual void captureImage() = 0;

    virtual int defaultCameraIndex() const = 0;
    virtual bool hasOpenedCamera() const = 0;
    virtual bool isReadyForCapture() const = 0;

    virtual QVector<CameraDeviceInfo> cameras() const = 0;
    virtual QVector<PreviewOption> previewOptions() const = 0;
    virtual QVector<CaptureOption> captureOptions() const = 0;
    virtual QString capabilityText() const = 0;
    virtual void requestFocusAt(qreal normalizedX, qreal normalizedY) = 0;

signals:
    void camerasChanged();
    void capabilitiesChanged();
    void logMessage(const QString &message);
    void stateTextChanged(const QString &text);
    void statusTextChanged(const QString &text);
    void frameTextChanged(const QString &text);
    void photoTextChanged(const QString &text);
    void readyForCaptureChanged(bool ready);
    void fpsChanged(double fps);
};

#endif // CAMERA_BACKEND_H
