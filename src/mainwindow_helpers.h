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
QString parseOpenZfsVersionText(const QString& text);
QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text);
QString sshControlPath();
QString sshBaseCommand(const ConnectionProfile& p);

} // namespace mwhelpers
