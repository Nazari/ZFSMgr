#pragma once

#include "connectiondialog.h"

#include <QString>

namespace mwhelpers {

QString oneLine(const QString& v, int maxLen = 220);
QString shSingleQuote(const QString& s);
bool isMountedValueTrue(const QString& value);
QString parentDatasetName(const QString& dataset);
QString normalizeDriveLetterValue(const QString& raw);
bool looksLikePowerShellScript(const QString& cmd);
QString sshControlPath();
QString sshBaseCommand(const ConnectionProfile& p);

} // namespace mwhelpers

