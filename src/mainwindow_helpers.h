#pragma once

#include "connectiondialog.h"

#include <QString>
#include <QVector>

namespace mwhelpers {

struct ImportablePoolInfo {
    QString pool;
    QString state;
    QString reason;
};

QString oneLine(const QString& v, int maxLen = 220);
QString shSingleQuote(const QString& s);
bool isMountedValueTrue(const QString& value);
QString parentDatasetName(const QString& dataset);
QString normalizeDriveLetterValue(const QString& raw);
bool looksLikePowerShellScript(const QString& cmd);
bool isWindowsOsType(const QString& osType);
QString parseOpenZfsVersionText(const QString& text);
QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text);
QString sshControlPath();
QString sshBaseCommand(const ConnectionProfile& p);
QString withSudoCommand(const ConnectionProfile& p, const QString& cmd);
QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd);
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd);

} // namespace mwhelpers
