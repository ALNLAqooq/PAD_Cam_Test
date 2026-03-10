#include "MainWindow.h"

#include "QtCameraBackend.h"
#include "WindowsNativeCameraBackend.h"

#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
CameraBackendType backendTypeFromIndex(int value)
{
    return (value == static_cast<int>(CameraBackendType::WindowsNative))
        ? CameraBackendType::WindowsNative
        : CameraBackendType::QtMultimedia;
}

QString readyToText(bool ready)
{
    return ready ? QStringLiteral("是") : QStringLiteral("否");
}
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , m_backend(nullptr)
    , m_backendCombo(nullptr)
    , m_cameraCombo(nullptr)
    , m_viewfinderCombo(nullptr)
    , m_captureCombo(nullptr)
    , m_refreshButton(nullptr)
    , m_openButton(nullptr)
    , m_stopButton(nullptr)
    , m_applyHighestButton(nullptr)
    , m_applySelectedButton(nullptr)
    , m_captureButton(nullptr)
    , m_previewContainer(nullptr)
    , m_previewLayout(nullptr)
    , m_stateValueLabel(nullptr)
    , m_statusValueLabel(nullptr)
    , m_frameValueLabel(nullptr)
    , m_photoValueLabel(nullptr)
    , m_readyValueLabel(nullptr)
    , m_capabilityEdit(nullptr)
    , m_logEdit(nullptr)
{
    setupUi();
    setupBackend(CameraBackendType::QtMultimedia);
    logMessage(QStringLiteral("程序已启动。"));
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("PAD 摄像头分辨率探测"));
    resize(1280, 820);

    m_refreshButton = new QPushButton(QStringLiteral("刷新摄像头"), this);
    m_backendCombo = new QComboBox(this);
    m_backendCombo->addItem(QStringLiteral("Qt Multimedia"), static_cast<int>(CameraBackendType::QtMultimedia));
    m_backendCombo->addItem(QStringLiteral("Windows Native (WIP)"), static_cast<int>(CameraBackendType::WindowsNative));
    m_cameraCombo = new QComboBox(this);
    m_openButton = new QPushButton(QStringLiteral("打开"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_applyHighestButton = new QPushButton(QStringLiteral("应用最高档"), this);
    m_applySelectedButton = new QPushButton(QStringLiteral("应用当前选中"), this);
    m_captureButton = new QPushButton(QStringLiteral("拍照"), this);

    QHBoxLayout *topLayout = new QHBoxLayout;
    topLayout->addWidget(m_refreshButton);
    topLayout->addWidget(m_backendCombo);
    topLayout->addWidget(m_cameraCombo, 1);
    topLayout->addWidget(m_openButton);
    topLayout->addWidget(m_stopButton);
    topLayout->addWidget(m_applyHighestButton);
    topLayout->addWidget(m_applySelectedButton);
    topLayout->addWidget(m_captureButton);

    m_previewContainer = new QWidget(this);
    m_previewLayout = new QVBoxLayout(m_previewContainer);
    m_previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewLayout->setSpacing(0);

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
    leftLayout->addWidget(m_previewContainer, 1);
    leftLayout->addLayout(infoLayout);
    leftPanel->setMinimumWidth(720);
    leftPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_viewfinderCombo = new QComboBox(this);
    m_captureCombo = new QComboBox(this);

    QFormLayout *selectionLayout = new QFormLayout;
    selectionLayout->addRow(QStringLiteral("预览档位"), m_viewfinderCombo);
    selectionLayout->addRow(QStringLiteral("拍照档位"), m_captureCombo);

    QGroupBox *selectionBox = new QGroupBox(QStringLiteral("请求设置"), this);
    selectionBox->setLayout(selectionLayout);

    m_capabilityEdit = new QPlainTextEdit(this);
    m_capabilityEdit->setReadOnly(true);

    QGroupBox *capabilityBox = new QGroupBox(QStringLiteral("能力检测"), this);
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
    rightPanel->setMinimumWidth(360);
    rightPanel->setMaximumWidth(420);
    rightPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);

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
    connect(m_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onBackendSelectionChanged);

    resetStatusLabels();
    updateButtonStates();
}

void MainWindow::refreshCameras()
{
    if (!m_backend) {
        return;
    }

    m_backend->refreshCameras();
    updateButtonStates();
}

void MainWindow::openSelectedCamera()
{
    if (!m_backend) {
        return;
    }

    m_backend->openCamera(m_cameraCombo->currentIndex());
    updateButtonStates();
}

void MainWindow::stopCamera()
{
    if (!m_backend) {
        return;
    }

    m_backend->stopCamera();
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
    if (!m_backend) {
        return;
    }

    m_backend->applySettings(m_viewfinderCombo->currentIndex(), m_captureCombo->currentIndex());
    updateButtonStates();
}

void MainWindow::captureImage()
{
    if (!m_backend) {
        return;
    }

    m_backend->captureImage();
    updateButtonStates();
}

void MainWindow::onBackendSelectionChanged(int index)
{
    setupBackend(backendTypeFromIndex(m_backendCombo->itemData(index).toInt()));
}

void MainWindow::onBackendCamerasChanged()
{
    populateCameraCombo();
    updateButtonStates();
}

void MainWindow::onBackendCapabilitiesChanged()
{
    populateSettingCombos();
    m_capabilityEdit->setPlainText(m_backend ? m_backend->capabilityText() : QString());
    updateButtonStates();
}

void MainWindow::onBackendStateTextChanged(const QString &text)
{
    m_stateValueLabel->setText(text);
    updateButtonStates();
}

void MainWindow::onBackendStatusTextChanged(const QString &text)
{
    m_statusValueLabel->setText(text);
    updateButtonStates();
}

void MainWindow::onBackendFrameTextChanged(const QString &text)
{
    m_frameValueLabel->setText(text);
}

void MainWindow::onBackendPhotoTextChanged(const QString &text)
{
    m_photoValueLabel->setText(text);
}

void MainWindow::onBackendReadyForCaptureChanged(bool ready)
{
    m_readyValueLabel->setText(readyToText(ready));
    updateButtonStates();
}

void MainWindow::setupBackend(CameraBackendType backendType)
{
    if (m_backend && m_backend->backendType() == backendType) {
        return;
    }

    const int comboIndex = (backendType == CameraBackendType::WindowsNative) ? 1 : 0;
    if (m_backendCombo->currentIndex() != comboIndex) {
        m_backendCombo->blockSignals(true);
        m_backendCombo->setCurrentIndex(comboIndex);
        m_backendCombo->blockSignals(false);
    }

    if (m_backend) {
        CameraBackend *oldBackend = m_backend;
        replacePreviewWidget(nullptr);
        disconnect(oldBackend, nullptr, this, nullptr);
        m_backend = nullptr;
        delete oldBackend;
    }

    if (backendType == CameraBackendType::WindowsNative) {
        m_backend = new WindowsNativeCameraBackend(this);
    } else {
        m_backend = new QtCameraBackend(this);
    }

    connect(m_backend, &CameraBackend::camerasChanged, this, &MainWindow::onBackendCamerasChanged);
    connect(m_backend, &CameraBackend::capabilitiesChanged, this, &MainWindow::onBackendCapabilitiesChanged);
    connect(m_backend, &CameraBackend::stateTextChanged, this, &MainWindow::onBackendStateTextChanged);
    connect(m_backend, &CameraBackend::statusTextChanged, this, &MainWindow::onBackendStatusTextChanged);
    connect(m_backend, &CameraBackend::frameTextChanged, this, &MainWindow::onBackendFrameTextChanged);
    connect(m_backend, &CameraBackend::photoTextChanged, this, &MainWindow::onBackendPhotoTextChanged);
    connect(m_backend, &CameraBackend::readyForCaptureChanged, this, &MainWindow::onBackendReadyForCaptureChanged);
    connect(m_backend, &CameraBackend::logMessage, this, &MainWindow::logMessage);

    replacePreviewWidget(m_backend->createPreviewWidget(m_previewContainer));
    resetStatusLabels();
    m_capabilityEdit->setPlainText(QString());
    m_backend->refreshCameras();
}

void MainWindow::replacePreviewWidget(QWidget *widget)
{
    while (QLayoutItem *item = m_previewLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (widget) {
        widget->setParent(m_previewContainer);
        m_previewLayout->addWidget(widget);
        widget->show();
    }
}

void MainWindow::populateCameraCombo()
{
    const QString selectedText = m_cameraCombo->currentText();
    const QVector<CameraDeviceInfo> devices = m_backend ? m_backend->cameras() : QVector<CameraDeviceInfo>();

    m_cameraCombo->blockSignals(true);
    m_cameraCombo->clear();

    for (int index = 0; index < devices.size(); ++index) {
        m_cameraCombo->addItem(devices.at(index).displayText);
    }

    if (!devices.isEmpty()) {
        int targetIndex = selectedText.isEmpty() ? -1 : m_cameraCombo->findText(selectedText);
        if (targetIndex < 0 && m_backend) {
            targetIndex = m_backend->defaultCameraIndex();
        }
        if (targetIndex < 0) {
            targetIndex = 0;
        }
        m_cameraCombo->setCurrentIndex(targetIndex);
    }

    m_cameraCombo->blockSignals(false);
}

void MainWindow::populateSettingCombos()
{
    const QString selectedPreviewText = m_viewfinderCombo->currentText();
    const QString selectedCaptureText = m_captureCombo->currentText();
    const QVector<PreviewOption> previewItems = m_backend ? m_backend->previewOptions() : QVector<PreviewOption>();
    const QVector<CaptureOption> captureItems = m_backend ? m_backend->captureOptions() : QVector<CaptureOption>();

    m_viewfinderCombo->blockSignals(true);
    m_captureCombo->blockSignals(true);

    m_viewfinderCombo->clear();
    for (int index = 0; index < previewItems.size(); ++index) {
        m_viewfinderCombo->addItem(previewItems.at(index).displayText);
    }

    if (!previewItems.isEmpty()) {
        int previewIndex = selectedPreviewText.isEmpty() ? 0 : m_viewfinderCombo->findText(selectedPreviewText);
        if (previewIndex < 0) {
            previewIndex = 0;
        }
        m_viewfinderCombo->setCurrentIndex(previewIndex);
    }

    m_captureCombo->clear();
    for (int index = 0; index < captureItems.size(); ++index) {
        m_captureCombo->addItem(captureItems.at(index).displayText);
    }

    if (!captureItems.isEmpty()) {
        int captureIndex = selectedCaptureText.isEmpty() ? 0 : m_captureCombo->findText(selectedCaptureText);
        if (captureIndex < 0) {
            captureIndex = 0;
        }
        m_captureCombo->setCurrentIndex(captureIndex);
    }

    m_viewfinderCombo->blockSignals(false);
    m_captureCombo->blockSignals(false);
}

void MainWindow::resetStatusLabels()
{
    m_stateValueLabel->setText(QStringLiteral("-"));
    m_statusValueLabel->setText(QStringLiteral("-"));
    m_frameValueLabel->setText(QStringLiteral("-"));
    m_photoValueLabel->setText(QStringLiteral("-"));
    m_readyValueLabel->setText(QStringLiteral("-"));
}

void MainWindow::updateButtonStates()
{
    const bool hasBackend = (m_backend != nullptr);
    const bool hasCameraList = hasBackend && !m_backend->cameras().isEmpty();
    const bool hasOpenedCamera = hasBackend && m_backend->hasOpenedCamera();
    const bool hasViewfinderChoices = hasBackend && !m_backend->previewOptions().isEmpty();
    const bool hasCaptureChoices = hasBackend && !m_backend->captureOptions().isEmpty();
    const bool canCapture = hasBackend && m_backend->isReadyForCapture();

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
