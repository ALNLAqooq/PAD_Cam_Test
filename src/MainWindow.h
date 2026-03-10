#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCamera>
#include <QCameraImageCapture>
#include <QCameraInfo>
#include <QCameraViewfinderSettings>
#include <QSize>
#include <QVector>
#include <QWidget>

class QCameraViewfinder;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVideoFrame;
class QVideoProbe;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

private slots:
    void refreshCameras();
    void openSelectedCamera();
    void stopCamera();
    void applyHighestSettings();
    void applySelectedSettings();
    void captureImage();

    void onCameraStatusChanged(QCamera::Status status);
    void onCameraStateChanged(QCamera::State state);
    void onCameraError(QCamera::Error error);
    void onImageSaved(int id, const QString &fileName);
    void onImageCaptureError(int id, QCameraImageCapture::Error error, const QString &errorString);
    void onReadyForCaptureChanged(bool ready);
    void onVideoFrameProbed(const QVideoFrame &frame);

private:
    void setupUi();
    void releaseCamera();
    void refreshCameraCapabilities();
    void logFocusCapabilities();
    void logFlashCapabilities();
    void updateCapabilityText();
    void updateButtonStates();
    void logMessage(const QString &message);

    QVector<QCameraInfo> m_cameras;
    QList<QCameraViewfinderSettings> m_viewfinderSettings;
    QList<QSize> m_captureResolutions;

    QCamera *m_camera;
    QCameraImageCapture *m_imageCapture;
    QVideoProbe *m_videoProbe;

    QComboBox *m_cameraCombo;
    QComboBox *m_viewfinderCombo;
    QComboBox *m_captureCombo;
    QPushButton *m_refreshButton;
    QPushButton *m_openButton;
    QPushButton *m_stopButton;
    QPushButton *m_applyHighestButton;
    QPushButton *m_applySelectedButton;
    QPushButton *m_captureButton;
    QCameraViewfinder *m_viewfinderWidget;
    QLabel *m_stateValueLabel;
    QLabel *m_statusValueLabel;
    QLabel *m_frameValueLabel;
    QLabel *m_photoValueLabel;
    QLabel *m_readyValueLabel;
    QPlainTextEdit *m_capabilityEdit;
    QPlainTextEdit *m_logEdit;
    bool m_focusCapabilitiesLogged;
};

#endif // MAINWINDOW_H
