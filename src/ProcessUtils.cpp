#include "ProcessUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpSocket>

namespace ProcessUtils {

static QString makeAbsolute(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QFileInfo info(path);
    if (info.isAbsolute()) {
        return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    }
    return QDir::current().absoluteFilePath(path);
}

QString expandPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    if (path.startsWith("~/")) {
        QString expanded = QDir::home().filePath(path.mid(2));
        return makeAbsolute(expanded);
    }
    return makeAbsolute(path);
}

QString expandOptionalPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    return expandPath(path);
}

QString resolveVmDir(const QString &overrideDir)
{
    QString candidate = expandOptionalPath(overrideDir);
    if (!candidate.isEmpty() && QDir(candidate).exists()) {
        return candidate;
    }

    const QString win11Default = QStringLiteral("/home/park/vm/win11");
    if (QDir(win11Default).exists()) {
        return win11Default;
    }

    QString fallback = expandPath(QStringLiteral("~/vm"));
    return fallback;
}

QString resolveDiskImage(const QString &vmDir, const QString &overridePath)
{
    QString candidate = expandOptionalPath(overridePath);
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate;
    }
    if (!vmDir.isEmpty()) {
        QString defaultDisk = QDir(vmDir).filePath(QStringLiteral("disk.qcow2"));
        if (QFileInfo::exists(defaultDisk)) {
            return defaultDisk;
        }
    }
    return {};
}

QString resolveOvmfCode(const QString &overridePath)
{
    QString candidate = expandOptionalPath(overridePath);
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QStringList defaults = {
        QStringLiteral("/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd"),
        QStringLiteral("/usr/share/edk2/ovmf/OVMF_CODE.fd"),
        QStringLiteral("/usr/share/OVMF/OVMF_CODE.fd")
    };
    for (const QString &path : defaults) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

QString resolveOvmfVars(const QString &vmDir, const QString &overridePath)
{
    QString candidate = expandOptionalPath(overridePath);
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate;
    }
    if (!vmDir.isEmpty()) {
        QString localVars = QDir(vmDir).filePath(QStringLiteral("OVMF_VARS.fd"));
        if (QFileInfo::exists(localVars)) {
            return localVars;
        }
    }
    const QStringList defaults = {
        QStringLiteral("/usr/share/edk2/ovmf/OVMF_VARS.fd"),
        QStringLiteral("/usr/share/OVMF/OVMF_VARS.fd")
    };
    for (const QString &path : defaults) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

QString resolveQemuBinary(const QString &overridePath)
{
    QString candidate = expandOptionalPath(overridePath);
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate;
    }
    const QStringList defaults = {
        QStringLiteral("~/ws/qemu/build/qemu-system-x86_64"),
        QStringLiteral("/home/park/ws/qemu/build/qemu-system-x86_64")
    };
    for (const QString &path : defaults) {
        QString expanded = expandOptionalPath(path);
        if (!expanded.isEmpty() && QFileInfo::exists(expanded)) {
            return expanded;
        }
    }
    QString found = QStandardPaths::findExecutable(QStringLiteral("qemu-system-x86_64"));
    return found;
}

QString resolveSwtpmBinary(const QString &overridePath)
{
    QString candidate = expandOptionalPath(overridePath);
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
        return candidate;
    }
    return QStandardPaths::findExecutable(QStringLiteral("swtpm"));
}

QString describeProcessError(QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("Failed to start");
    case QProcess::Crashed:
        return QStringLiteral("Process crashed");
    case QProcess::Timedout:
        return QStringLiteral("Operation timed out");
    case QProcess::ReadError:
        return QStringLiteral("Read error");
    case QProcess::WriteError:
        return QStringLiteral("Write error");
    case QProcess::UnknownError:
    default:
        return QStringLiteral("Unknown error");
    }
}

bool isPortInUse(int port)
{
    auto tryAddress = [port](const QHostAddress &addr) -> bool {
        if (addr.isNull()) {
            return false;
        }
        QTcpSocket socket;
        socket.connectToHost(addr, port);
        if (socket.waitForConnected(50)) {
            socket.disconnectFromHost();
            return true;
        }
        return false;
    };

    if (tryAddress(QHostAddress::LocalHost)) {
        return true;
    }
    if (tryAddress(QHostAddress::LocalHostIPv6)) {
        return true;
    }
    return false;
}

int pickVncDisplay(const QString &request, QStringList &logMessages, bool &autoSelected)
{
    autoSelected = false;
    QString trimmed = request.trimmed();
    bool requestAuto = trimmed.isEmpty() || trimmed.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0;
    if (!requestAuto) {
        bool ok = false;
        QString view = trimmed;
        if (view.startsWith(QLatin1Char(':'))) {
            view.remove(0, 1);
        }
        int display = view.toInt(&ok);
        if (!ok || display < 0 || display > 99) {
            logMessages << QObject::tr("Invalid VNC display \"%1\". Falling back to auto.").arg(request);
        } else {
            int port = 5900 + display;
            if (!isPortInUse(port)) {
                autoSelected = false;
                return display;
            }
            logMessages << QObject::tr("Requested VNC display :%1 is busy. Searching for a free port.")
                               .arg(display);
        }
        requestAuto = true;
    }

    if (requestAuto) {
        for (int display = 1; display < 100; ++display) {
            int port = 5900 + display;
            if (!isPortInUse(port)) {
                autoSelected = true;
                logMessages << QObject::tr("Using available VNC display :%1").arg(display);
                return display;
            }
        }
    }

    logMessages << QObject::tr("Unable to find a free VNC display.");
    return -1;
}

} // namespace ProcessUtils
