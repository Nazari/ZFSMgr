#pragma once

#include "connectiondialog.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mwhelpers {

struct ImportablePoolInfo {
    QString pool;
    QString guid;
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
QString sshHostKeyProblemHint(const QString& sshStderr);
QString shSingleQuote(const QString& s);
bool isMountedValueTrue(const QString& value);
QString parentDatasetName(const QString& dataset);
QString normalizeDriveLetterValue(const QString& raw);
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
QVector<QPair<QString, QString>> parseZfsMountJsonOutput(const QString& text);
QString buildHasMountedChildrenCommand(bool isWindows, const QString& datasetName);
QString buildRecursiveUmountCommand(bool isWindows, const QString& datasetName);
QString buildSingleUmountCommand(bool isWindows, const QString& datasetName);
QString buildSingleMountCommand(const QString& datasetName);
QString buildMountChildrenCommand(bool isWindows, const QString& datasetName);
QString buildWindowsMountPrecheckCommand(const QString& datasetName, const QString& effectiveMountpoint);
QString sshControlPath();
QString findLocalExecutable(const QString& name);
QString sshUserHost(const ConnectionProfile& p);
QString sshUserHostPort(const ConnectionProfile& p);
QString sshAddressFamilyOption(const ConnectionProfile& p);
QString sshBaseCommand(const ConnectionProfile& p);
QString scpUploadCommand(const ConnectionProfile& p, const QString& localPath, const QString& remotePath);
QString buildSshTargetPrefix(const ConnectionProfile& p);
QString buildSimpleSshInvocation(const ConnectionProfile& p, const QString& remoteCmd);
QString streamProgressPipeFilter();
QString buildPipedTransferCommand(const QString& sendSegment, const QString& recvSegment);
QString streamCodecName(StreamCodec codec);
StreamCodec chooseStreamCodec(bool hasZstdBoth, bool hasGzipBoth);
QString buildTarSourceCommand(bool isWindows, const QString& mountPath, StreamCodec codec);
QString buildTarDestinationCommand(bool isWindows, const QString& mountPath, StreamCodec codec);
QString withUnixSearchPathCommand(const QString& cmd);
QString withSudoCommand(const ConnectionProfile& p, const QString& cmd);
QString agentCommand(const ConnectionProfile& p, const QString& agentArgs);
// Renderiza una invocación del agente a partir de sus argumentos.
//
// Es la ÚNICA dirección de conversión que sobrevive. La inversa —parsear la cadena
// para recuperar los argumentos— es la que causaba que un directorio con ';', '&' o
// '|' en el nombre truncara la orden, y desaparece con la migración a argv.
QString agentShellCommand(const ConnectionProfile& p, const QStringList& agentArgs);
QString agentShellCommandStreamInput(const ConnectionProfile& p, const QStringList& agentArgs);
// Verbos que solo existen en la línea de comandos del agente, nunca por RPC. La lista
// la fija el marcador de esquema de resources/CMakeLists.txt con el prefijo "cli-only:".
bool isCliOnlyAgentCommand(const QString& verb);
// Solo sobrevive como oráculo de los tests del renderizado a cadena.
QStringList posixShellSplitArgs(const QString& s);
QString withSudoStreamInputCommand(const ConnectionProfile& p, const QString& cmd);
// Comprueba que una contraseña de sudo local sirve realmente, con la MISMA invocación
// que usa withSudoCommand: `sudo -k -S -p '' true`.
//
// Existe porque guardar una contraseña equivocada dejaba la conexión Local inservible
// y sin arreglo posible desde la aplicación: el arranque solo la pedía cuando el campo
// estaba VACÍO, y la conexión Local no se puede editar. Se aceptaba cualquier cosa y el
// error aparecía mucho después, al intentar usar sudo, con otro mensaje.
//
// El `-k` es imprescindible: sin él la comprobación puede aprovechar un ticket de sudo
// todavía válido y dar por buena una contraseña incorrecta.
// En Windows no hay sudo: devuelve true sin comprobar nada.
bool localSudoPasswordWorks(const QString& password, QString* errorOut = nullptr);
// ¿Este error de un comando es sudo rechazando la contraseña? Sirve para ofrecer el
// arreglo donde el usuario se entera del problema, en vez de dejarlo con un mensaje
// que no dice qué hacer. Se distingue de "el usuario no está en sudoers", que no se
// arregla reintroduciendo la contraseña.
bool looksLikeSudoAuthFailure(const QString& text);
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd);
// Strips any leading non-JSON text (e.g. MOTD banners) before the first '{'.
QString stripToJson(const QString& output);

} // namespace mwhelpers
