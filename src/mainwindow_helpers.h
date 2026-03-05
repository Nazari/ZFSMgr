#pragma once

#include "connectiondialog.h"

#include <QMap>
#include <QString>
#include <QStringList>
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

struct MountpointConflict {
    QString mountpoint;
    QString mountedDataset;
    QString requestedDataset;
};

enum class StreamCodec {
    Zstd,
    Gzip,
    None,
};

QString oneLine(const QString& v, int maxLen = 220);
QString shSingleQuote(const QString& s);
bool isMountedValueTrue(const QString& value);
QString parentDatasetName(const QString& dataset);
QString normalizeDriveLetterValue(const QString& raw);
bool looksLikePowerShellScript(const QString& cmd);
bool isWindowsOsType(const QString& osType);
QString windowsGptTypeName(const QString& guid);
QString formatWindowsFsTypeDetail(const QString& rawFsType);
bool windowsPartitionTypeIsProtected(const QString& rawFsType);
QString parseOpenZfsVersionText(const QString& text);
QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text);
TransferButtonState computeTransferButtonState(const TransferButtonInputs& in);
bool parentMountCheckRequired(const QString& parentMountpoint, const QString& parentCanmount);
bool parentAllowsChildMount(const QString& parentMountpoint, const QString& parentCanmount, const QString& parentMounted);
QMap<QString, QStringList> duplicateMountpoints(const QMap<QString, QString>& datasetMountpoints);
QVector<MountpointConflict> externalMountpointConflicts(const QMap<QString, QString>& targetDatasetMountpoints,
                                                        const QMap<QString, QStringList>& mountedByMountpoint);
QVector<QPair<QString, QString>> parseZfsMountOutput(const QString& text);
QString buildHasMountedChildrenCommand(bool isWindows, const QString& datasetName);
QString buildRecursiveUmountCommand(bool isWindows, const QString& datasetName);
QString buildSingleUmountCommand(bool isWindows, const QString& datasetName);
QString buildSingleMountCommand(const QString& datasetName);
QString buildMountChildrenCommand(bool isWindows, const QString& datasetName);
QString buildWindowsMountPrecheckCommand(const QString& datasetName, const QString& effectiveMountpoint);
QString sshControlPath();
QString sshUserHost(const ConnectionProfile& p);
QString sshUserHostPort(const ConnectionProfile& p);
QString sshBaseCommand(const ConnectionProfile& p);
QString buildSshTargetPrefix(const ConnectionProfile& p);
QString buildSimpleSshInvocation(const ConnectionProfile& p, const QString& remoteCmd);
QString streamProgressPipeFilter();
QString buildPipedTransferCommand(const QString& sendSegment, const QString& recvSegment);
QString streamCodecName(StreamCodec codec);
StreamCodec chooseStreamCodec(bool hasZstdBoth, bool hasGzipBoth);
QString buildTarSourceCommand(bool isWindows, const QString& mountPath, StreamCodec codec);
QString buildTarDestinationCommand(bool isWindows, const QString& mountPath, StreamCodec codec);
QString withSudoCommand(const ConnectionProfile& p, const QString& cmd);
QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd);
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd);

} // namespace mwhelpers
