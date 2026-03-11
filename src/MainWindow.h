#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "CameraBackend.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void refreshCameras();
    void openSelectedCamera();
    void stopCamera();
    void applyHighestSettings();
    void applySelectedSettings();
    void captureImage();
    void onBackendSelectionChanged(int index);
    void onBackendCamerasChanged();
    void onBackendCapabilitiesChanged();
    void onBackendStateTextChanged(const QString &text);
    void onBackendStatusTextChanged(const QString &text);
    void onBackendFrameTextChanged(const QString &text);
    void onBackendPhotoTextChanged(const QString &text);
    void onBackendReadyForCaptureChanged(bool ready);
    void onBackendFpsChanged(double fps);

private:
    void setupUi();
    void setupBackend(CameraBackendType backendType);
    void replacePreviewWidget(QWidget *widget);
    void populateCameraCombo();
    void populateSettingCombos();
    void resetStatusLabels();
    void updateButtonStates();
    void logMessage(const QString &message);
    bool handlePreviewClick(QObject *watched, const QPoint &pos);

    CameraBackend *m_backend;

    QComboBox *m_backendCombo;
    QComboBox *m_cameraCombo;
    QComboBox *m_viewfinderCombo;
    QComboBox *m_captureCombo;
    QPushButton *m_refreshButton;
    QPushButton *m_openButton;
    QPushButton *m_stopButton;
    QPushButton *m_applyHighestButton;
    QPushButton *m_applySelectedButton;
    QPushButton *m_captureButton;
    QWidget *m_previewContainer;
    QVBoxLayout *m_previewLayout;
    QWidget *m_previewWidget;
    QLabel *m_stateValueLabel;
    QLabel *m_statusValueLabel;
    QLabel *m_frameValueLabel;
    QLabel *m_photoValueLabel;
    QLabel *m_readyValueLabel;
    QLabel *m_fpsValueLabel;
    QPlainTextEdit *m_capabilityEdit;
    QPlainTextEdit *m_logEdit;
};

#endif // MAINWINDOW_H
