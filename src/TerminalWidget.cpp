#include "TerminalWidget.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTextCursor>
#include <QTextOption>

namespace {
constexpr int kFlushIntervalMs = 50;
}

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
    , m_output(new QPlainTextEdit(this))
    , m_input(new QLineEdit(this))
    , m_sendButton(new QPushButton(tr("Send"), this))
    , m_process(new QProcess(this))
    , m_flushTimer(new QTimer(this))
{
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setWordWrapMode(QTextOption::NoWrap);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_input->setPlaceholderText(tr("Enter command and press Enter"));

    auto *inputLayout = new QHBoxLayout();
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->addWidget(m_input, 1);
    inputLayout->addWidget(m_sendButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_output, 1);
    layout->addLayout(inputLayout);

    connect(m_sendButton, &QPushButton::clicked, this, &TerminalWidget::handleSendCommand);
    connect(m_input, &QLineEdit::returnPressed, this, &TerminalWidget::handleSendCommand);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &TerminalWidget::handleReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &TerminalWidget::handleReadyReadStderr);
    connect(m_process, &QProcess::finished, this, &TerminalWidget::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &TerminalWidget::handleProcessError);

    m_flushTimer->setInterval(kFlushIntervalMs);
    m_flushTimer->setSingleShot(false);
    connect(m_flushTimer, &QTimer::timeout, this, &TerminalWidget::flushPendingOutput);
}

bool TerminalWidget::startProcess(const QString &program,
                                  const QStringList &arguments,
                                  const QString &workingDir)
{
    stopProcess();

    m_output->clear();
    m_pendingChunks.clear();

    if (!workingDir.isEmpty()) {
        m_process->setWorkingDirectory(workingDir);
    }
    m_process->setProgram(program);
    m_process->setArguments(arguments);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->start();
    bool started = m_process->waitForStarted(1000);
    if (!started) {
        appendText(tr("[terminal] Failed to start %1\n").arg(program));
        return false;
    }

    appendText(tr("[terminal] Started %1 %2\n").arg(program, arguments.join(' ')));
    setControlsEnabled(true);
    m_flushTimer->start();
    return true;
}

void TerminalWidget::stopProcess()
{
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_process->terminate();
    if (!m_process->waitForFinished(500)) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
    setControlsEnabled(false);
}

bool TerminalWidget::isRunning() const
{
    return m_process->state() != QProcess::NotRunning;
}

void TerminalWidget::handleReadyReadStdout()
{
    const QByteArray data = m_process->readAllStandardOutput();
    if (!data.isEmpty()) {
        m_pendingChunks << QString::fromLocal8Bit(data);
    }
}

void TerminalWidget::handleReadyReadStderr()
{
    const QByteArray data = m_process->readAllStandardError();
    if (!data.isEmpty()) {
        m_pendingChunks << QString::fromLocal8Bit(data);
    }
}

void TerminalWidget::flushPendingOutput()
{
    if (m_pendingChunks.isEmpty()) {
        return;
    }
    appendText(m_pendingChunks.join(QString()));
    m_pendingChunks.clear();
}

void TerminalWidget::handleProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    flushPendingOutput();
    appendText(tr("[terminal] Process exited (%1, %2)\n")
                   .arg(exitCode)
                   .arg(status == QProcess::NormalExit ? tr("normal") : tr("crashed")));
    setControlsEnabled(false);
    emit processFinished(exitCode, status);
}

void TerminalWidget::handleProcessError(QProcess::ProcessError error)
{
    appendText(tr("[terminal] Process error: %1\n").arg(m_process->errorString()));
    if (error == QProcess::FailedToStart || error == QProcess::Crashed) {
        setControlsEnabled(false);
    }
}

void TerminalWidget::handleSendCommand()
{
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    const QString text = m_input->text();
    if (text.isEmpty()) {
        return;
    }
    m_process->write(text.toLocal8Bit());
    m_process->write("\n");
    m_input->clear();
}

void TerminalWidget::appendText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    m_output->moveCursor(QTextCursor::End);
    m_output->insertPlainText(text);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void TerminalWidget::setControlsEnabled(bool enabled)
{
    m_input->setEnabled(enabled);
    m_sendButton->setEnabled(enabled);
    if (!enabled) {
        m_flushTimer->stop();
    }
}
