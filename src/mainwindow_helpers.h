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

QString maskCommandSecrets(const QString& input);
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
// Los mismos argumentos, pero como lista para lanzar `scp` DIRECTAMENTE, sin pasar por
// un intérprete.
//
// Existe porque la subida del daemon se lanzaba con `sh -c`, y cuando la aplicación
// corre en Windows no hay ningún intérprete POSIX: el proceso no llegaba a arrancar,
// la salida de error quedaba vacía y lo único que se veía era «scp falló».
//
// `multiplex` a false omite ControlMaster/ControlPersist/ControlPath: el OpenSSH de
// Windows no admite multiplexado, y además da un segundo intento cuando el socket de
// control heredado está en mal estado.
QStringList scpUploadArgs(const ConnectionProfile& p,
                          const QString& localPath,
                          const QString& remotePath,
                          bool multiplex);
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
// Codifica un texto como escapes octales para `printf '%b'`: cada byte UTF-8 pasa a
// ser \0ddd, así que el resultado es ASCII puro.
//
// Existe por la contraseña de sudo en macOS. Qt convierte los argumentos que pasa a
// un proceso a la forma DESCOMPUESTA de Unicode, que es la que macOS usa para el
// sistema de ficheros: una `ñ` precompuesta (U+00F1, bytes c3 b1) llega al intérprete
// como `n` + tilde combinante (6e cc 83). Son bytes distintos, así que sudo recibía
// una contraseña que no era la del usuario y la rechazaba, mientras que escribirla
// directamente en su entrada estándar —lo que hace la comprobación— sí funcionaba.
// Verificado en un Mac: mismo QString, c3 b1 por un lado y 6e cc 83 por el otro.
//
// En ASCII no hay descomposición posible, de ahí la codificación. `\0ddd` con `%b` es
// POSIX, así que vale igual en macOS, Linux y FreeBSD.
QString shPrintfOctalEscaped(const QString& s);

// Una contraseña de perfil y la clave con la que se la vuelve a encontrar al restaurar.
struct StorableSecret {
    QString key;
    QString secret;
};

// Sustituye en `command` cada contraseña por un marcador, para poder escribir la orden
// en disco sin escribir el secreto. Contempla las DOS formas en que puede aparecer: la
// octal que produce shPrintfOctalEscaped —la de withSudoCommand— y la literal.
//
// Si al terminar alguna contraseña sigue presente, devuelve cadena vacía y pone *okOut a
// false. Quien llama DEBE respetarlo y no guardar nada: es la última comprobación antes
// de que un secreto acabe en un fichero de texto.
QString redactSecretsForStorage(const QString& command,
                                const QVector<StorableSecret>& secrets,
                                bool* okOut);

// Inversa de la anterior. Un marcador cuya clave no esté en `secrets` se deja intacto:
// así quien restaura puede detectar que la orden apuntaba a un perfil que ya no existe,
// en vez de ejecutarla con un hueco.
QString restoreSecretsFromStorage(const QString& stored,
                                  const QVector<StorableSecret>& secrets);

// Prefijo del marcador, expuesto para poder comprobar si quedó alguno sin resolver.
QString storedSecretMarkerPrefix();
// Devuelve una orden de shell equivalente cuyo texto es ASCII puro, para que sobreviva
// al paso por los argumentos de un proceso.
//
// Mismo motivo que shPrintfOctalEscaped, pero general: en macOS, Qt convierte los
// argumentos que entrega a QProcess a la forma descompuesta de Unicode. Para un nombre
// de fichero da igual —el sistema de ficheros de Apple normaliza igual—, pero un nombre
// de dataset, un punto de montaje o una contraseña se comparan byte a byte contra lo
// que hay al otro lado, y dejan de coincidir. Afecta tanto a `sh -c` local como a la
// orden que se le pasa a `ssh`, porque en los dos casos es un argumento.
//
// Si la orden ya es ASCII —la inmensa mayoría— se devuelve TAL CUAL y no cambia nada:
// ni el rendimiento, ni lo que se ve al depurar, ni el comportamiento en Linux o
// Windows. El envoltorio solo aparece cuando hay algo que proteger.
QString asciiSafeShellCommand(const QString& cmd);
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
// En Windows no hay sudo: devuelve Ok sin comprobar nada.
//
// Distingue "la contraseña es incorrecta" de "no se pudo comprobar". Confundirlas es
// grave en las dos direcciones: decir "contraseña incorrecta" cuando lo que pasa es
// que no se encuentra el binario manda al usuario a buscar donde no hay nada, y
// bloquear el arranque porque no se pudo comprobar reproduce el encierro que esta
// comprobación venía a evitar. Ante la duda hay que dejar pasar.
enum class SudoCheck {
    Ok,
    WrongPassword,
    CouldNotCheck,
};
SudoCheck checkLocalSudoPassword(const QString& password, QString* detailOut = nullptr);
// ¿Este error de un comando es sudo rechazando la contraseña? Sirve para ofrecer el
// arreglo donde el usuario se entera del problema, en vez de dejarlo con un mensaje
// que no dice qué hacer. Se distingue de "el usuario no está en sudoers", que no se
// arregla reintroduciendo la contraseña.
bool looksLikeSudoAuthFailure(const QString& text);
QString buildSshPreviewCommandText(const ConnectionProfile& p, const QString& remoteCmd);
// Strips any leading non-JSON text (e.g. MOTD banners) before the first '{'.
QString stripToJson(const QString& output);

} // namespace mwhelpers
