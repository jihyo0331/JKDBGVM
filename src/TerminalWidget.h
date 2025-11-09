#pragma once

#include <QWidget>
#include <QProcess>

class QPlainTextEdit;
class QLineEdit;
class QPushButton;

class QTimer;

class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);

    bool startProcess(const QString &program,
                      const QStringList &arguments,
                      const QString &workingDir = QString());
    void stopProcess();
    bool isRunning() const;

signals:
    void processFinished(int exitCode, QProcess::ExitStatus status);

private slots:
    void handleReadyReadStdout();
    void handleReadyReadStderr();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus status);
    void handleProcessError(QProcess::ProcessError error);
    void handleSendCommand();
    void flushPendingOutput();

private:
    void appendText(const QString &text);
    void setControlsEnabled(bool enabled);

    QPlainTextEdit *m_output;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QProcess *m_process;
    QStringList m_pendingChunks;
    QTimer *m_flushTimer;
};
