#pragma once

#include <QProcess>
#include <QString>
#include <QStringList>

namespace ProcessUtils {

QString expandPath(const QString &path);
QString expandOptionalPath(const QString &path);
QString resolveVmDir(const QString &overrideDir);
QString resolveDiskImage(const QString &vmDir, const QString &overridePath);
QString resolveOvmfCode(const QString &overridePath);
QString resolveOvmfVars(const QString &vmDir, const QString &overridePath);
QString resolveQemuBinary(const QString &overridePath);
QString resolveSwtpmBinary(const QString &overridePath);
QString describeProcessError(QProcess::ProcessError error);

bool isPortInUse(int port);
int pickVncDisplay(const QString &request, QStringList &logMessages, bool &autoSelected);

} // namespace ProcessUtils
