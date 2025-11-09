#include "MainWindow.h"

#include "ProcessUtils.h"
#include "TerminalWidget.h"

#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QtGlobal>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QTime>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

namespace {
constexpr int kVncWaitTimeoutMs = 10000;
constexpr int kVncProbeIntervalMs = 200;
constexpr quint8 kVncButtonMaskLeft = 1;
constexpr quint8 kVncButtonMaskMiddle = 2;
constexpr quint8 kVncButtonMaskRight = 4;
constexpr quint8 kVncButtonMaskWheelUp = 8;
constexpr quint8 kVncButtonMaskWheelDown = 16;

constexpr quint32 kKeysymBackspace = 0xff08;
constexpr quint32 kKeysymTab = 0xff09;
constexpr quint32 kKeysymReturn = 0xff0d;
constexpr quint32 kKeysymEscape = 0xff1b;
constexpr quint32 kKeysymDelete = 0xffff;
constexpr quint32 kKeysymHome = 0xff50;
constexpr quint32 kKeysymLeft = 0xff51;
constexpr quint32 kKeysymUp = 0xff52;
constexpr quint32 kKeysymRight = 0xff53;
constexpr quint32 kKeysymDown = 0xff54;
constexpr quint32 kKeysymPageUp = 0xff55;
constexpr quint32 kKeysymPageDown = 0xff56;
constexpr quint32 kKeysymEnd = 0xff57;
constexpr quint32 kKeysymInsert = 0xff63;
constexpr quint32 kKeysymF1 = 0xffbe;

constexpr quint32 kKeysymShiftL = 0xffe1;
constexpr quint32 kKeysymShiftR = 0xffe2;
constexpr quint32 kKeysymControlL = 0xffe3;
constexpr quint32 kKeysymControlR = 0xffe4;
constexpr quint32 kKeysymAltL = 0xffe9;
constexpr quint32 kKeysymAltR = 0xffea;
constexpr quint32 kKeysymSuperL = 0xffeb;
constexpr quint32 kKeysymSuperR = 0xffec;
constexpr quint32 kKeysymCapsLock = 0xffe5;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_displayLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_logView(nullptr)
    , m_startButton(nullptr)
    , m_stopButton(nullptr)
    , m_gdbGroup(nullptr)
    , m_gdbTerminal(nullptr)
    , m_gdbStartButton(nullptr)
    , m_qemuProcess(nullptr)
    , m_swtpmProcess(nullptr)
    , m_vncClient(new VncClient(this))
    , m_vncProbeTimer(new QTimer(this))
    , m_currentVncDisplay(-1)
    , m_ignoreNextVncDisconnect(false)
    , m_abortOnVncTimeout(false)
    , m_remoteFramebufferSize()
    , m_pointerButtonMask(0)
    , m_vncHasFocus(false)
    , m_pendingGdbStart(false)
{
    buildUi();
    setWindowTitle(tr("QEMU VM Launcher"));

    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::handleStart);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::handleStop);
    connect(m_vncClient, &VncClient::framebufferUpdated,
            this, &MainWindow::handleFramebufferUpdated);
    connect(m_vncClient, &VncClient::disconnected,
            this, &MainWindow::handleVncDisconnected);

    m_vncProbeTimer->setInterval(kVncProbeIntervalMs);
    m_vncProbeTimer->setSingleShot(false);
    connect(m_vncProbeTimer, &QTimer::timeout, this, [this]() {
        if (m_currentVncDisplay < 0) {
            m_vncProbeTimer->stop();
            return;
        }
        int port = 5900 + m_currentVncDisplay;
        if (ProcessUtils::isPortInUse(port)) {
            m_vncProbeTimer->stop();
            appendLog(tr("VNC ready on %1:%2").arg(m_currentConfig.vncHost).arg(port));
            startVncClient(m_currentConfig.vncHost, port);
            return;
        }
        if (m_vncProbeElapsed.elapsed() > kVncWaitTimeoutMs) {
            if (m_abortOnVncTimeout) {
                m_vncProbeTimer->stop();
                appendLog(tr("Timed out waiting for VNC on %1:%2")
                              .arg(m_currentConfig.vncHost)
                              .arg(port));
                QMessageBox::warning(this, tr("VNC Timeout"),
                                     tr("QEMU did not open the VNC port in time. "
                                        "Check the QEMU output for details."));
                handleStop();
            } else {
                appendLog(tr("Still waiting for VNC on %1:%2; retrying...")
                              .arg(m_currentConfig.vncHost)
                              .arg(port));
                m_vncProbeElapsed.restart();
            }
        }
    });

    appendLog(tr("Ready. Click Start to launch the VM."));
    setUiRunning(false);
    m_currentConfig = VmConfig{};
}

MainWindow::~MainWindow()
{
    handleStop();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    handleStop();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_displayLabel) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
            handleMouseEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::Wheel:
            handleWheelEvent(static_cast<QWheelEvent*>(event));
            return true;
        case QEvent::KeyPress:
            handleKeyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::KeyRelease:
            handleKeyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::FocusIn:
            m_vncHasFocus = true;
            break;
        case QEvent::FocusOut:
            m_vncHasFocus = false;
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    m_statusLabel = new QLabel(tr("Status: idle"), central);
    layout->addWidget(m_statusLabel);

    m_displayLabel = new QLabel(tr("VNC output will appear here after launch."), central);
    m_displayLabel->setAlignment(Qt::AlignCenter);
    m_displayLabel->setMinimumSize(QSize(640, 360));
    m_displayLabel->setStyleSheet("background-color: #202020; color: #aaaaaa;");
    m_displayLabel->setFocusPolicy(Qt::StrongFocus);
    m_displayLabel->setMouseTracking(true);
    m_displayLabel->installEventFilter(this);
    layout->addWidget(m_displayLabel, 1);

    auto *buttonLayout = new QHBoxLayout();
    m_startButton = new QPushButton(tr("Start"), central);
    m_stopButton = new QPushButton(tr("Stop"), central);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_stopButton);
    layout->addLayout(buttonLayout);

    m_logView = new QTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(160);
    layout->addWidget(m_logView);

    auto *gdbGroup = new QGroupBox(tr("GDB Console"), central);
    auto *gdbLayout = new QVBoxLayout(gdbGroup);
    m_gdbStartButton = new QPushButton(tr("Launch GDB"), gdbGroup);
    m_gdbStartButton->setEnabled(false);
    gdbLayout->addWidget(m_gdbStartButton, 0, Qt::AlignLeft);
    m_gdbTerminal = new TerminalWidget(gdbGroup);
    m_gdbTerminal->setMinimumHeight(160);
    gdbLayout->addWidget(m_gdbTerminal, 1);
    layout->addWidget(gdbGroup);
    m_gdbGroup = gdbGroup;
    m_gdbGroup->setVisible(false);

    connect(m_gdbStartButton, &QPushButton::clicked, this, [this]() {
        startGdbConsole(true);
    });

    setCentralWidget(central);
}

void MainWindow::appendLog(const QString &text)
{
    m_logView->append(QStringLiteral("[%1] %2")
                          .arg(QTime::currentTime().toString("HH:mm:ss"))
                          .arg(text));
}

bool MainWindow::resolveConfig(VmConfig &cfg)
{
    cfg.qemuBinary = ProcessUtils::resolveQemuBinary(qEnvironmentVariable("QQ_QEMU_BIN"));
    if (cfg.qemuBinary.isEmpty()) {
        QMessageBox::critical(this, tr("QEMU Not Found"),
                              tr("Unable to locate qemu-system-x86_64. "
                                 "Install QEMU or set QQ_QEMU_BIN."));
        return false;
    }

    cfg.machineType = qEnvironmentVariable("QQ_QEMU_MACHINE", "q35");
    cfg.memorySize = qEnvironmentVariable("QQ_QEMU_MEMORY", "4G");
    cfg.vmDir = ProcessUtils::resolveVmDir(qEnvironmentVariable("QQ_VM_DIR"));
    cfg.diskImage = ProcessUtils::resolveDiskImage(cfg.vmDir, qEnvironmentVariable("QQ_VM_DISK"));
    if (cfg.diskImage.isEmpty()) {
        QMessageBox::critical(this, tr("Disk Image Missing"),
                              tr("Could not locate a disk image. "
                                 "Set QQ_VM_DISK or place disk.qcow2 in %1.")
                                  .arg(cfg.vmDir));
        return false;
    }

    cfg.ovmfCode = ProcessUtils::resolveOvmfCode(qEnvironmentVariable("QQ_QEMU_OVMF_CODE"));
    cfg.ovmfVars = ProcessUtils::resolveOvmfVars(cfg.vmDir, qEnvironmentVariable("QQ_QEMU_OVMF_VARS"));
    cfg.accelerator = qEnvironmentVariable("QQ_QEMU_ACCEL", "tcg,thread=multi");
    cfg.cpuModel = qEnvironmentVariable("QQ_QEMU_CPU",
                                        QStringLiteral("Westmere,model-id=\"Intel(R) Xeon(R) CPU E5620 @ 2.40GHz\""));
    cfg.kernel = ProcessUtils::expandOptionalPath(qEnvironmentVariable("QQ_QEMU_KERNEL"));
    cfg.initrd = ProcessUtils::expandOptionalPath(qEnvironmentVariable("QQ_QEMU_INITRD"));
    cfg.appendArgs = qEnvironmentVariable("QQ_QEMU_APPEND");
    cfg.vncHost = qEnvironmentVariable("QQ_VNC_HOST", "127.0.0.1");
    cfg.vncDisplayRequest = qEnvironmentVariable("QQ_VNC_DISPLAY");

    const QString enableTpmEnv = qEnvironmentVariable("QQ_ENABLE_TPM");
    cfg.enableTpm = enableTpmEnv == QLatin1String("1") || enableTpmEnv.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    if (cfg.enableTpm) {
        cfg.swtpmBinary = ProcessUtils::resolveSwtpmBinary(qEnvironmentVariable("QQ_SWTPM_BIN"));
        if (cfg.swtpmBinary.isEmpty()) {
            QMessageBox::critical(this, tr("swtpm Not Found"),
                                  tr("TPM support is required but swtpm was not found.\n"
                                     "Install the swtpm package or set QQ_ENABLE_TPM=0 to skip it."));
            return false;
        }
        cfg.swtpmStateDir = QDir(cfg.vmDir).filePath(QStringLiteral("tpm"));
        cfg.swtpmSocket = QDir(cfg.vmDir).filePath(QStringLiteral("swtpm.sock"));
        cfg.swtpmCtrlSocket = QDir(cfg.vmDir).filePath(QStringLiteral("swtpm-ctrl.sock"));
    } else {
        cfg.swtpmBinary.clear();
        cfg.swtpmStateDir.clear();
        cfg.swtpmSocket.clear();
        cfg.swtpmCtrlSocket.clear();
    }

    const QString enableGdbEnv = qEnvironmentVariable("QQ_ENABLE_GDB");
    if (enableGdbEnv.isEmpty()) {
        cfg.enableGdb = true;
    } else {
        cfg.enableGdb = enableGdbEnv == QLatin1String("1")
            || enableGdbEnv.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    }
    cfg.gdbBinary = ProcessUtils::expandOptionalPath(qEnvironmentVariable("QQ_GDB_BIN"));
    if (cfg.gdbBinary.isEmpty()) {
        cfg.gdbBinary = QStringLiteral("gdb");
    }
    cfg.gdbSymbols = ProcessUtils::expandOptionalPath(qEnvironmentVariable("QQ_GDB_SYMBOLS"));
    bool okPort = false;
    cfg.gdbPort = qEnvironmentVariable("QQ_GDB_PORT", "1234").toInt(&okPort);
    if (!okPort || cfg.gdbPort <= 0) {
        cfg.gdbPort = 1234;
    }

    return true;
}

QStringList MainWindow::buildQemuArguments(const VmConfig &cfg, int vncDisplay)
{
    QStringList args;
    args << "-machine" << cfg.machineType;
    if (!cfg.cpuModel.isEmpty()) {
        args << "-cpu" << cfg.cpuModel;
    }
    args << "-m" << cfg.memorySize;
    args << "-display" << "none";
    args << "-vnc" << QString(":%1").arg(vncDisplay);

    if (!cfg.accelerator.isEmpty()) {
        if (cfg.accelerator.compare(QStringLiteral("kvm"), Qt::CaseInsensitive) == 0) {
            args << "-enable-kvm";
        } else {
            args << "-accel" << cfg.accelerator;
        }
    }
    if (cfg.enableGdb) {
        args << "-S";
        args << "-gdb"
             << QStringLiteral("tcp:localhost:%1,ipv4").arg(cfg.gdbPort);
    }

    if (!cfg.ovmfCode.isEmpty()) {
        args << "-drive"
             << QString("if=pflash,format=raw,readonly=on,file=%1").arg(cfg.ovmfCode);
    }
    if (!cfg.ovmfVars.isEmpty()) {
        args << "-drive"
             << QString("if=pflash,format=raw,file=%1").arg(cfg.ovmfVars);
    }
    if (!cfg.diskImage.isEmpty()) {
        args << "-drive"
             << QString("file=%1,if=virtio,format=qcow2").arg(cfg.diskImage);
        args << "-boot" << "order=c";
    }
    if (!cfg.kernel.isEmpty()) {
        args << "-kernel" << cfg.kernel;
    }
    if (!cfg.initrd.isEmpty()) {
        args << "-initrd" << cfg.initrd;
    }
    if (!cfg.appendArgs.isEmpty()) {
        args << "-append" << cfg.appendArgs;
    }
    args << "-smp" << qEnvironmentVariable("QQ_QEMU_SMP", "4");
    if (cfg.enableTpm && !cfg.swtpmSocket.isEmpty()) {
        args << "-chardev"
             << QString("socket,id=chrtpm,path=%1").arg(cfg.swtpmSocket);
        args << "-tpmdev" << "emulator,id=tpm0,chardev=chrtpm";
        args << "-device" << "tpm-crb,tpmdev=tpm0";
    }
    return args;
}

void MainWindow::handleStart()
{
    if (m_qemuProcess) {
        QMessageBox::information(this, tr("Already running"),
                                 tr("QEMU is already running."));
        return;
    }

    VmConfig cfg;
    if (!resolveConfig(cfg)) {
        return;
    }

    m_gdbGroup->setVisible(cfg.enableGdb);
    m_gdbStartButton->setEnabled(cfg.enableGdb);
    if (!cfg.enableGdb) {
        stopGdbConsole();
    }

    QStringList logMessages;
    bool autoSelected = false;
    m_currentVncDisplay = ProcessUtils::pickVncDisplay(cfg.vncDisplayRequest, logMessages, autoSelected);
    if (m_currentVncDisplay < 0) {
        QMessageBox::critical(this, tr("VNC Port Error"),
                              tr("Could not determine a VNC display to use."));
        return;
    }
    for (const QString &msg : logMessages) {
        appendLog(msg);
    }

    removeDiskLock(cfg.diskImage);

    if (!startSwtpm(cfg)) {
        return;
    }

    m_currentConfig = cfg;
    m_pendingGdbStart = cfg.enableGdb;
    QStringList args = buildQemuArguments(cfg, m_currentVncDisplay);

    appendLog(tr("Launching QEMU: %1 %2").arg(cfg.qemuBinary, args.join(' ')));

    m_qemuProcess = new QProcess(this);
    connect(m_qemuProcess, &QProcess::started, this, &MainWindow::handleQemuStarted);
    connect(m_qemuProcess, &QProcess::readyReadStandardOutput, this, &MainWindow::handleQemuStdout);
    connect(m_qemuProcess, &QProcess::readyReadStandardError, this, &MainWindow::handleQemuStderr);
    connect(m_qemuProcess, &QProcess::finished, this, &MainWindow::handleQemuFinished);
    connect(m_qemuProcess, &QProcess::errorOccurred, this, &MainWindow::handleQemuError);

    m_qemuProcess->setProgram(cfg.qemuBinary);
    m_qemuProcess->setArguments(args);
    m_qemuProcess->setWorkingDirectory(cfg.vmDir);
    m_qemuProcess->setProcessChannelMode(QProcess::SeparateChannels);
    m_qemuProcess->start();

    setUiRunning(true);
    m_statusLabel->setText(tr("Status: launching QEMU..."));
    m_abortOnVncTimeout = true;
    m_vncProbeElapsed.restart();
    m_vncProbeTimer->start();
}

void MainWindow::handleStop()
{
    if (m_vncProbeTimer->isActive()) {
        m_vncProbeTimer->stop();
    }
    if (m_vncClient->isRunning()) {
        m_ignoreNextVncDisconnect = true;
        m_vncClient->requestInterruption();
        m_vncClient->wait(1000);
        m_vncClient->stopClient();
    }
    if (m_qemuProcess) {
        if (m_qemuProcess->state() != QProcess::NotRunning) {
            appendLog(tr("Stopping QEMU..."));
            m_qemuProcess->terminate();
            if (!m_qemuProcess->waitForFinished(2000)) {
                appendLog(tr("QEMU did not exit cleanly; killing process."));
                m_qemuProcess->kill();
                m_qemuProcess->waitForFinished(2000);
            }
        }
        m_qemuProcess->deleteLater();
        m_qemuProcess = nullptr;
    }
    stopGdbConsole();
    m_gdbStartButton->setEnabled(false);
    m_pendingGdbStart = false;
    cleanupRuntime();
}

void MainWindow::handleQemuStarted()
{
    appendLog(tr("QEMU started (PID %1).").arg(m_qemuProcess->processId()));
    m_statusLabel->setText(tr("Status: waiting for VNC..."));
    if (m_pendingGdbStart && m_currentConfig.enableGdb) {
        startGdbConsole(false);
        m_pendingGdbStart = false;
    }
}

void MainWindow::handleQemuError(QProcess::ProcessError error)
{
    const QString message = ProcessUtils::describeProcessError(error);
    appendLog(tr("QEMU error: %1").arg(message));
    QMessageBox::critical(this, tr("QEMU Error"),
                          tr("Failed to start QEMU: %1").arg(message));
    handleStop();
}

void MainWindow::handleQemuFinished(int exitCode, QProcess::ExitStatus status)
{
    appendLog(tr("QEMU exited (code %1, %2).")
                  .arg(exitCode)
                  .arg(status == QProcess::NormalExit ? tr("normal") : tr("crashed")));
    handleStop();
}

void MainWindow::handleQemuStdout()
{
    if (!m_qemuProcess) {
        return;
    }
    const QByteArray data = m_qemuProcess->readAllStandardOutput();
    if (!data.isEmpty()) {
        appendLog(QString::fromLocal8Bit(data).trimmed());
    }
}

void MainWindow::handleQemuStderr()
{
    if (!m_qemuProcess) {
        return;
    }
    const QByteArray data = m_qemuProcess->readAllStandardError();
    if (!data.isEmpty()) {
        appendLog(QString::fromLocal8Bit(data).trimmed());
    }
}

void MainWindow::handleFramebufferUpdated(const QImage &image)
{
    QPixmap pix = QPixmap::fromImage(image);
    m_displayLabel->setText(QString());
    m_displayLabel->setPixmap(pix.scaled(m_displayLabel->size(),
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
    m_remoteFramebufferSize = image.size();
}

void MainWindow::handleVncDisconnected(const QString &reason)
{
    if (m_ignoreNextVncDisconnect) {
        m_ignoreNextVncDisconnect = false;
        return;
    }

    appendLog(tr("VNC disconnected: %1").arg(reason));

    const bool qemuRunning = m_qemuProcess && m_qemuProcess->state() != QProcess::NotRunning;
    if (qemuRunning && m_currentVncDisplay >= 0) {
        appendLog(tr("Attempting to reconnect to VNC..."));
        m_statusLabel->setText(tr("Status: reconnecting to VNC..."));
        m_displayLabel->setPixmap(QPixmap());
        m_displayLabel->setText(tr("VNC disconnected. Reconnecting..."));
        m_vncProbeElapsed.restart();
        if (!m_vncProbeTimer->isActive()) {
            m_vncProbeTimer->start();
        }
        return;
    }

    m_displayLabel->setPixmap(QPixmap());
    m_displayLabel->setText(tr("VNC output will appear here after launch."));
    m_remoteFramebufferSize = QSize();
    m_pointerButtonMask = 0;
    m_vncHasFocus = false;
}

void MainWindow::startVncClient(const QString &host, int port)
{
    if (m_vncClient->isRunning()) {
        m_ignoreNextVncDisconnect = true;
        m_vncClient->requestInterruption();
        m_vncClient->wait(1000);
        m_vncClient->stopClient();
    }

    appendLog(tr("Connecting VNC client to %1:%2").arg(host).arg(port));
    m_displayLabel->setPixmap(QPixmap());
    m_displayLabel->setText(tr("Connecting to VNC..."));
    if (!m_vncClient->startClient(host, port)) {
        appendLog(tr("Failed to start VNC client."));
        QMessageBox::critical(this, tr("VNC Error"),
                              tr("Unable to connect to the VNC server."));
        handleStop();
        return;
    }

    m_abortOnVncTimeout = false;
    m_statusLabel->setText(tr("Status: VM running"));
}

void MainWindow::startGdbConsole(bool userRequested)
{
    if (!m_currentConfig.enableGdb) {
        if (userRequested) {
            QMessageBox::information(this, tr("GDB Disabled"),
                                     tr("Enable QQ_ENABLE_GDB=1 to use the GDB console."));
        }
        return;
    }
    if (!m_gdbTerminal) {
        return;
    }
    if (m_gdbTerminal->isRunning()) {
        m_gdbTerminal->stopProcess();
    }

    QStringList args;
    args << QStringLiteral("--nx") << QStringLiteral("--quiet");
    if (!m_currentConfig.gdbSymbols.isEmpty()) {
        args << m_currentConfig.gdbSymbols;
    }
    args << QStringLiteral("-ex")
         << QStringLiteral("target remote localhost:%1").arg(m_currentConfig.gdbPort);

    if (!m_gdbTerminal->startProcess(m_currentConfig.gdbBinary, args, m_currentConfig.vmDir)) {
        appendLog(tr("Failed to start GDB console."));
        QMessageBox::warning(this, tr("GDB Error"),
                             tr("Could not launch %1. Check that it is installed.")
                                 .arg(m_currentConfig.gdbBinary));
        return;
    }

    appendLog(tr("GDB launched on localhost:%1").arg(m_currentConfig.gdbPort));
}

void MainWindow::stopGdbConsole()
{
    if (m_gdbTerminal && m_gdbTerminal->isRunning()) {
        m_gdbTerminal->stopProcess();
    }
}

void MainWindow::removeDiskLock(const QString &diskImage)
{
    if (diskImage.isEmpty()) {
        return;
    }
    const QString lockPath = diskImage + QStringLiteral(".lck");
    QFileInfo info(lockPath);
    if (!info.exists()) {
        return;
    }
    bool removed = false;
    if (info.isDir()) {
        QDir dir(lockPath);
        removed = dir.removeRecursively();
    } else {
        removed = QFile::remove(lockPath);
    }
    if (removed) {
        appendLog(tr("Released disk lock %1").arg(lockPath));
    } else {
        appendLog(tr("Failed to remove disk lock %1").arg(lockPath));
    }
}

void MainWindow::sendPointerEvent(const QPoint &remotePos, quint8 buttons)
{
    if (!m_vncClient || !m_vncClient->isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(m_vncClient,
                              "postPointerEvent",
                              Qt::QueuedConnection,
                              Q_ARG(int, remotePos.x()),
                              Q_ARG(int, remotePos.y()),
                              Q_ARG(int, static_cast<int>(buttons)));
}

void MainWindow::sendKeyEvent(quint32 keysym, bool pressed)
{
    if (!m_vncClient || !m_vncClient->isRunning() || keysym == 0) {
        return;
    }
    QMetaObject::invokeMethod(m_vncClient,
                              "postKeyEvent",
                              Qt::QueuedConnection,
                              Q_ARG(quint32, keysym),
                              Q_ARG(bool, pressed));
}

bool MainWindow::mapToRemotePosition(const QPoint &widgetPos, QPoint &remotePos) const
{
    if (m_remoteFramebufferSize.isEmpty()) {
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPixmap pixmap = m_displayLabel->pixmap(Qt::ReturnByValue);
#else
    const QPixmap *pixmapPtr = m_displayLabel->pixmap();
    QPixmap pixmap = pixmapPtr ? *pixmapPtr : QPixmap();
#endif
    if (pixmap.isNull()) {
        return false;
    }

    QSize scaledSize = pixmap.size() / pixmap.devicePixelRatio();
    QSize labelSize = m_displayLabel->size();
    if (scaledSize.width() == 0 || scaledSize.height() == 0) {
        return false;
    }

    const int offsetX = (labelSize.width() - scaledSize.width()) / 2;
    const int offsetY = (labelSize.height() - scaledSize.height()) / 2;
    const int localX = widgetPos.x() - offsetX;
    const int localY = widgetPos.y() - offsetY;
    if (localX < 0 || localY < 0 || localX >= scaledSize.width() || localY >= scaledSize.height()) {
        return false;
    }

    const double normX = static_cast<double>(localX) / static_cast<double>(scaledSize.width());
    const double normY = static_cast<double>(localY) / static_cast<double>(scaledSize.height());
    const int remoteX = qBound(0, static_cast<int>(normX * m_remoteFramebufferSize.width()), m_remoteFramebufferSize.width() - 1);
    const int remoteY = qBound(0, static_cast<int>(normY * m_remoteFramebufferSize.height()), m_remoteFramebufferSize.height() - 1);
    remotePos = QPoint(remoteX, remoteY);
    return true;
}

quint8 MainWindow::pointerMaskFromButtons(Qt::MouseButtons buttons) const
{
    quint8 mask = 0;
    if (buttons & Qt::LeftButton) {
        mask |= kVncButtonMaskLeft;
    }
    if (buttons & Qt::MiddleButton) {
        mask |= kVncButtonMaskMiddle;
    }
    if (buttons & Qt::RightButton) {
        mask |= kVncButtonMaskRight;
    }
    return mask;
}

quint32 MainWindow::keyEventToKeysym(QKeyEvent *event) const
{
    if (!event) {
        return 0;
    }
    if (!event->text().isEmpty()) {
        const QChar ch = event->text().at(0);
        if (!ch.isNull() && ch.unicode() >= 0x20) {
            return ch.unicode();
        }
    }
    switch (event->key()) {
    case Qt::Key_Backspace: return kKeysymBackspace;
    case Qt::Key_Tab: return kKeysymTab;
    case Qt::Key_Return:
    case Qt::Key_Enter: return kKeysymReturn;
    case Qt::Key_Escape: return kKeysymEscape;
    case Qt::Key_Delete: return kKeysymDelete;
    case Qt::Key_Home: return kKeysymHome;
    case Qt::Key_End: return kKeysymEnd;
    case Qt::Key_PageUp: return kKeysymPageUp;
    case Qt::Key_PageDown: return kKeysymPageDown;
    case Qt::Key_Left: return kKeysymLeft;
    case Qt::Key_Right: return kKeysymRight;
    case Qt::Key_Up: return kKeysymUp;
    case Qt::Key_Down: return kKeysymDown;
    case Qt::Key_Insert: return kKeysymInsert;
    case Qt::Key_Shift: return kKeysymShiftL;
    case Qt::Key_Control: return kKeysymControlL;
    case Qt::Key_Alt: return kKeysymAltL;
    case Qt::Key_AltGr: return kKeysymAltR;
    case Qt::Key_Meta: return kKeysymSuperL;
    case Qt::Key_CapsLock: return kKeysymCapsLock;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35) {
            return kKeysymF1 + (event->key() - Qt::Key_F1);
        }
        break;
    }
    return 0;
}

void MainWindow::handleMouseEvent(QMouseEvent *event)
{
    if (!event || !m_vncClient || !m_vncClient->isRunning()) {
        return;
    }
    Qt::MouseButtons buttons = event->buttons();
    if (event->type() == QEvent::MouseButtonPress) {
        buttons |= event->button();
        if (!m_vncHasFocus) {
            m_displayLabel->setFocus(Qt::MouseFocusReason);
            m_vncHasFocus = true;
        }
    }
    m_pointerButtonMask = pointerMaskFromButtons(buttons);

    QPoint remotePos;
    if (!mapToRemotePosition(event->pos(), remotePos)) {
        return;
    }
    sendPointerEvent(remotePos, m_pointerButtonMask);
    event->accept();
}

void MainWindow::handleWheelEvent(QWheelEvent *event)
{
    if (!event || !m_vncClient || !m_vncClient->isRunning()) {
        return;
    }
    QPoint remotePos;
    if (!mapToRemotePosition(event->position().toPoint(), remotePos)) {
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        return;
    }
    const quint8 wheelMask = (delta > 0)
                                 ? static_cast<quint8>(m_pointerButtonMask | kVncButtonMaskWheelUp)
                                 : static_cast<quint8>(m_pointerButtonMask | kVncButtonMaskWheelDown);
    sendPointerEvent(remotePos, wheelMask);
    sendPointerEvent(remotePos, m_pointerButtonMask);
    event->accept();
}

void MainWindow::handleKeyPressEvent(QKeyEvent *event)
{
    if (!m_vncHasFocus || !event) {
        return;
    }
    const quint32 keysym = keyEventToKeysym(event);
    if (keysym == 0) {
        return;
    }
    sendKeyEvent(keysym, true);
    event->accept();
}

void MainWindow::handleKeyReleaseEvent(QKeyEvent *event)
{
    if (!m_vncHasFocus || !event) {
        return;
    }
    const quint32 keysym = keyEventToKeysym(event);
    if (keysym == 0) {
        return;
    }
    sendKeyEvent(keysym, false);
    event->accept();
}

bool MainWindow::startSwtpm(const VmConfig &cfg)
{
    if (!cfg.enableTpm) {
        return true;
    }
    if (cfg.swtpmBinary.isEmpty() || cfg.swtpmSocket.isEmpty()) {
        QMessageBox::critical(this, tr("swtpm Configuration"),
                              tr("TPM support is enabled but the configuration is incomplete."));
        return false;
    }

    QDir stateDir(cfg.swtpmStateDir);
    if (!cfg.swtpmStateDir.isEmpty() && !stateDir.exists()) {
        if (!QDir().mkpath(cfg.swtpmStateDir)) {
            QMessageBox::critical(this, tr("TPM Directory Error"),
                                  tr("Failed to create TPM state directory:\n%1").arg(cfg.swtpmStateDir));
            return false;
        }
    }
    QFile::remove(cfg.swtpmSocket);
    if (!cfg.swtpmCtrlSocket.isEmpty()) {
        QFile::remove(cfg.swtpmCtrlSocket);
    }

    stopSwtpm();

    appendLog(tr("Starting swtpm..."));
    m_swtpmProcess = new QProcess(this);
    QStringList args;
    args << QStringLiteral("socket")
         << QStringLiteral("--tpm2");
    if (!cfg.swtpmStateDir.isEmpty()) {
        args << QStringLiteral("--tpmstate")
             << QStringLiteral("dir=%1").arg(cfg.swtpmStateDir);
    }
    if (!cfg.swtpmCtrlSocket.isEmpty()) {
        args << QStringLiteral("--ctrl")
             << QStringLiteral("type=unixio,path=%1").arg(cfg.swtpmCtrlSocket);
    }
    args << QStringLiteral("--server")
         << QStringLiteral("type=unixio,path=%1").arg(cfg.swtpmSocket);
    args << QStringLiteral("--log") << QStringLiteral("level=20");

    m_swtpmProcess->setProgram(cfg.swtpmBinary);
    m_swtpmProcess->setArguments(args);
    m_swtpmProcess->setProcessChannelMode(QProcess::SeparateChannels);

    QProcess *proc = m_swtpmProcess;
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QByteArray data = proc->readAllStandardOutput();
        if (!data.isEmpty()) {
            appendLog(tr("swtpm: %1").arg(QString::fromLocal8Bit(data).trimmed()));
        }
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QByteArray data = proc->readAllStandardError();
        if (!data.isEmpty()) {
            appendLog(tr("swtpm: %1").arg(QString::fromLocal8Bit(data).trimmed()));
        }
    });

    m_swtpmProcess->start();
    if (!m_swtpmProcess->waitForStarted(2000)) {
        appendLog(tr("Failed to start swtpm process."));
        QMessageBox::critical(this, tr("swtpm Error"),
                              tr("Unable to start swtpm. Check that the swtpm binary is executable."));
        stopSwtpm();
        return false;
    }

    QElapsedTimer waitTimer;
    waitTimer.start();
    while (waitTimer.elapsed() < 3000) {
        if (QFileInfo::exists(cfg.swtpmSocket)) {
            appendLog(tr("swtpm ready at %1").arg(cfg.swtpmSocket));
            return true;
        }
        QThread::msleep(50);
    }

    appendLog(tr("Timed out waiting for swtpm socket at %1").arg(cfg.swtpmSocket));
    QMessageBox::critical(this, tr("swtpm Timeout"),
                          tr("swtpm did not create its socket in time."));
    stopSwtpm();
    return false;
}

void MainWindow::stopSwtpm()
{
    if (!m_swtpmProcess) {
        return;
    }
    if (m_swtpmProcess->state() != QProcess::NotRunning) {
        m_swtpmProcess->terminate();
        if (!m_swtpmProcess->waitForFinished(1000)) {
            m_swtpmProcess->kill();
            m_swtpmProcess->waitForFinished(1000);
        }
    }
    m_swtpmProcess->deleteLater();
    m_swtpmProcess = nullptr;
}

void MainWindow::setUiRunning(bool running)
{
    m_startButton->setEnabled(!running);
    m_stopButton->setEnabled(running);
    if (m_gdbStartButton) {
        m_gdbStartButton->setEnabled(running && m_currentConfig.enableGdb);
    }
}

void MainWindow::cleanupRuntime()
{
    stopSwtpm();
    m_vncProbeTimer->stop();
    m_currentVncDisplay = -1;
    m_ignoreNextVncDisconnect = false;
    m_abortOnVncTimeout = false;
    if (m_currentConfig.enableTpm) {
        if (!m_currentConfig.swtpmSocket.isEmpty()) {
            QFile::remove(m_currentConfig.swtpmSocket);
        }
        if (!m_currentConfig.swtpmCtrlSocket.isEmpty()) {
            QFile::remove(m_currentConfig.swtpmCtrlSocket);
        }
    }
    m_statusLabel->setText(tr("Status: idle"));
    setUiRunning(false);
    m_displayLabel->setPixmap(QPixmap());
    m_displayLabel->setText(tr("VNC output will appear here after launch."));
    QString lastDisk = m_currentConfig.diskImage;
    m_currentConfig = VmConfig{};
    m_remoteFramebufferSize = QSize();
    m_pointerButtonMask = 0;
    m_vncHasFocus = false;
    removeDiskLock(lastDisk);
}
