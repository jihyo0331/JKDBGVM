#pragma once

#include "VncClient.h"

#include <QMainWindow>
#include <QProcess>
#include <QElapsedTimer>

class QLabel;
class QPushButton;
class QTextEdit;
class QTimer;
class TerminalWidget;

struct VmConfig {
    QString qemuBinary;
    QString machineType;
    QString memorySize;
    QString vmDir;
    QString diskImage;
    QString ovmfCode;
    QString ovmfVars;
    QString accelerator;
    QString cpuModel;
    QString kernel;
    QString initrd;
    QString appendArgs;
    QString vncHost;
    QString vncDisplayRequest;
    bool enableTpm = false;
    QString swtpmBinary;
    QString swtpmStateDir;
    QString swtpmSocket;
    QString swtpmCtrlSocket;
    bool enableGdb = false;
    QString gdbBinary;
    QString gdbSymbols;
    int gdbPort = 1234;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void handleStart();
    void handleStop();
    void handleQemuStarted();
    void handleQemuError(QProcess::ProcessError error);
    void handleQemuFinished(int exitCode, QProcess::ExitStatus status);
    void handleQemuStdout();
    void handleQemuStderr();
    void handleFramebufferUpdated(const QImage &image);
    void handleVncDisconnected(const QString &reason);

private:
    void buildUi();
    void appendLog(const QString &text);
    bool resolveConfig(VmConfig &cfg);
    QStringList buildQemuArguments(const VmConfig &cfg, int vncDisplay);
    void startVncClient(const QString &host, int port);
    void sendPointerEvent(const QPoint &remotePos, quint8 buttons);
    void sendKeyEvent(quint32 keysym, bool pressed);
    bool mapToRemotePosition(const QPoint &widgetPos, QPoint &remotePos) const;
    quint8 pointerMaskFromButtons(Qt::MouseButtons buttons) const;
    quint32 keyEventToKeysym(QKeyEvent *event) const;
    void handleMouseEvent(QMouseEvent *event);
    void handleWheelEvent(QWheelEvent *event);
    void handleKeyPressEvent(QKeyEvent *event);
    void handleKeyReleaseEvent(QKeyEvent *event);
    bool startSwtpm(const VmConfig &cfg);
    void stopSwtpm();
    void startGdbConsole(bool userRequested = false);
    void stopGdbConsole();
    void removeDiskLock(const QString &diskImage);
    void setUiRunning(bool running);
    void cleanupRuntime();

    QLabel *m_displayLabel;
    QLabel *m_statusLabel;
    QTextEdit *m_logView;
    QPushButton *m_startButton;
    QPushButton *m_stopButton;
    QWidget *m_gdbGroup;
    TerminalWidget *m_gdbTerminal;
    QPushButton *m_gdbStartButton;

    QProcess *m_qemuProcess;
    QProcess *m_swtpmProcess;
    VncClient *m_vncClient;
    QTimer *m_vncProbeTimer;
    QElapsedTimer m_vncProbeElapsed;
    VmConfig m_currentConfig;
    int m_currentVncDisplay;
    bool m_ignoreNextVncDisconnect;
    bool m_abortOnVncTimeout;
    QSize m_remoteFramebufferSize;
    quint8 m_pointerButtonMask;
    bool m_vncHasFocus;
    bool m_pendingGdbStart;
};
