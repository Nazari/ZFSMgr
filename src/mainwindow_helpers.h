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

struct TransferButtonInputs {
    bool srcDatasetSelected{false};
    bool srcSnapshotSelected{false};
    bool dstDatasetSelected{false};
    bool dstSnapshotSelected{false};
    QString srcSelectionKey;
    QString dstSelectionKey;
    bool srcSelectionConsistent{false};
    bool dstSelectionConsistent{false};
    bool srcDatasetMounted{false};
    bool dstDatasetMounted{false};
};

struct TransferButtonState {
    bool copyEnabled{false};
    bool levelEnabled{false};
    bool syncEnabled{false};
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
TransferButtonState computeTransferButtonState(const TransferButtonInputs& in);
QString sshControlPath();
QString sshBaseCommand(const ConnectionProfile& p);
QString withSudoCommand(const ConnectionProfile& p, const QString& cmd);
QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd);
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd);

} // namespace mwhelpers
