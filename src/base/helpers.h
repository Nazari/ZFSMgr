#pragma once

#include <string>
#include <vector>

#include <map>

#include "connectionprofile.h"

// Construcción de órdenes y predicados sobre valores de ZFS, SIN Qt.
//
// Segunda pieza de la capa base, portada a mano desde `mwhelpers`. `src/mainwindow_helpers.cpp`
// se queda como adaptador para no tocar los puntos de llamada; ver
// docs/diseno_tecnico_capa_base_sin_qt.md.
//
// Lo que NO está aquí, y por qué, está en ese mismo documento: las que toman
// `ConnectionProfile`, las que usan expresiones regulares, JSON, procesos o el sistema
// de ficheros.
namespace zfsmgr::base::helpers {

struct TransferButtonInputs {
    bool srcDatasetSelected{false};
    bool srcSnapshotSelected{false};
    bool dstDatasetSelected{false};
    bool dstSnapshotSelected{false};
    std::string srcSelectionKey;
    std::string dstSelectionKey;
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
    std::string mountpoint;
    std::string mountedDataset;
    std::string requestedDataset;
};

struct StorableSecret {
    std::string key;
    std::string secret;
};

enum class StreamCodec {
    Zstd,
    Gzip,
    None,
};

// Colapsa espacios y recorta a `maxLen` CARACTERES —no bytes—, para escribir una línea
// en el registro sin partir un carácter UTF-8 por la mitad.
std::string oneLine(const std::string& v, int maxLen = 220);

// Valores con que ZFS dice que sí en la propiedad `mounted`.
bool isMountedValueTrue(const std::string& value);

// «pool/a/b» -> «pool/a». Vacío si no hay padre, incluido el caso de la raíz del pool.
std::string parentDatasetName(const std::string& dataset);

bool isWindowsOsType(const std::string& osType);

// Si el padre no se monta —sin punto de montaje, «none», o `canmount=off`—, no tiene
// sentido exigir que esté montado para montar al hijo.
bool parentMountCheckRequired(const std::string& parentMountpoint,
                              const std::string& parentCanmount);
bool parentAllowsChildMount(const std::string& parentMountpoint,
                            const std::string& parentCanmount,
                            const std::string& parentMounted);

// Órdenes de montaje. En Windows salen en PowerShell y en Unix en shell POSIX, que es
// la diferencia que obliga a llevar el `isWindows` hasta aquí.
std::string buildHasMountedChildrenCommand(bool isWindows, const std::string& datasetName);
std::string buildRecursiveUmountCommand(bool isWindows, const std::string& datasetName);
std::string buildSingleUmountCommand(bool isWindows, const std::string& datasetName);
std::string buildSingleMountCommand(const std::string& datasetName);
std::string buildMountChildrenCommand(bool isWindows, const std::string& datasetName);
std::string buildWindowsMountPrecheckCommand(const std::string& datasetName,
                                             const std::string& effectiveMountpoint);

// Transferencias por tubería. `pv` es opcional: si no está, se pasa por `cat`.
std::string streamProgressPipeFilter();
std::string buildPipedTransferCommand(const std::string& sendSegment,
                                      const std::string& recvSegment);
std::string streamCodecName(StreamCodec codec);
StreamCodec chooseStreamCodec(bool hasZstdBoth, bool hasGzipBoth);
std::string buildTarSourceCommand(bool isWindows, const std::string& mountPath, StreamCodec codec);
std::string buildTarDestinationCommand(bool isWindows, const std::string& mountPath, StreamCodec codec);

// Antepone las rutas donde suele vivir `zfs` cuando el PATH de una sesión no
// interactiva no las trae.
std::string withUnixSearchPathCommand(const std::string& cmd);

std::string rpcTunnelBusyReason();
std::string storedSecretMarkerPrefix();

// Descarta lo que preceda al primer '{': algunas órdenes escriben avisos antes del JSON.
std::string stripToJson(const std::string& output);

// Codifica cada BYTE UTF-8 como \0ddd para `printf '%b'`, de modo que el resultado es
// ASCII puro.
//
// Existe por la contraseña de sudo en macOS: Qt descompone los caracteres al pasar la
// orden al intérprete y sudo recibía otros bytes de los tecleados.
std::string shPrintfOctalEscaped(const std::string& s);

// Ruta del socket de multiplexado de SSH. Lleva el marcador %C, que expande el propio
// ssh con un resumen de usuario/host/puerto.
std::string sshControlPath();

// Qué botones de transferencia deben quedar activos, dada la selección.
TransferButtonState computeTransferButtonState(const TransferButtonInputs& in);

// Puntos de montaje repetidos entre datasets: los agrupa por punto y devuelve solo los
// que tienen más de uno.
std::map<std::string, std::vector<std::string>> duplicateMountpoints(
    const std::map<std::string, std::string>& datasetMountpoints);

// Puntos de montaje que ya ocupa OTRO dataset distinto del que se pide.
std::vector<MountpointConflict> externalMountpointConflicts(
    const std::map<std::string, std::string>& targetDatasetMountpoints,
    const std::map<std::string, std::vector<std::string>>& mountedByMountpoint);

// Tapa el secreto de los verbos que lo llevan, para poder escribir la invocación en el
// registro.
std::string maskedAgentArgvForLog(const std::vector<std::string>& argv);

// Deja solo la LETRA de unidad, en mayúscula. Vacío si no hay ninguna.
std::string normalizeDriveLetterValue(const std::string& raw);

// Explica los dos fallos de clave de host que tienen remedio conocido. Vacío si el
// error es otro.
std::string sshHostKeyProblemHint(const std::string& sshStderr);

// Nombre legible de un GUID de tipo de partición GPT. Vacío si no se conoce.
std::string windowsGptTypeName(const std::string& guid);
std::string formatWindowsFsTypeDetail(const std::string& rawFsType);
// Particiones y discos que NO deben ofrecerse a ZFS: sistema, recuperación, reservada,
// y el disco de arranque.
bool windowsPartitionTypeIsProtected(const std::string& rawFsType);

// Verbos que solo existen en la línea de comandos del agente, nunca por RPC.
bool isCliOnlyAgentCommand(const std::string& verb);

// Trocea como lo haría un shell POSIX. Solo sobrevive como oráculo de los tests del
// renderizado a cadena.
std::vector<std::string> posixShellSplitArgs(const std::string& s);

// Sustituye cada contraseña por un marcador, para escribir la orden en disco sin
// escribir el secreto. Contempla las DOS formas: la octal de shPrintfOctalEscaped y la
// literal. Si tras sustituir el secreto SIGUE apareciendo, devuelve vacío y pone
// `okOut` a false: es preferible perder la orden que escribir una contraseña.
std::string redactSecretsForStorage(const std::string& command,
                                    const std::vector<StorableSecret>& secrets,
                                    bool* okOut);
std::string restoreSecretsFromStorage(const std::string& stored,
                                      const std::vector<StorableSecret>& secrets);

// Busca un ejecutable en el PATH y, si no aparece, en los directorios de siempre.
//
// Devuelve la ruta absoluta, o vacío si no está. El respaldo por directorios no es un
// adorno: en macOS un proceso lanzado desde el Finder hereda un PATH mínimo que NO incluye
// /opt/homebrew/bin, así que `sshpass` estaba instalado y aun así no se encontraba.
//
// En Windows se prueban además las extensiones de PATHEXT, porque «ssh» a secas no es el
// nombre de ningún fichero.
std::string findLocalExecutable(const std::string& name);

// Si la orden tiene algo fuera de ASCII, la reescribe como `eval "$(printf '%b' '...')"`.
// Ver shPrintfOctalEscaped: en macOS Qt descomponía los caracteres al pasarlos.
std::string asciiSafeShellCommand(const std::string& cmd);

// ¿Es un rechazo de CONTRASEÑA? Distinto de un fallo de autorización, donde volver a
// teclearla no arregla nada y por tanto no se ofrece reintentar.
bool looksLikeSudoAuthFailure(const std::string& text);

struct ImportablePoolInfo {
    std::string pool;
    std::string guid;
    std::string state;
    std::string reason;
};

// Tapa los secretos de una orden para poder escribirla en el registro. Contempla las
// formas concretas que construye esta aplicación —no un «password» genérico— y de ahí
// que sean siete patrones.
std::string maskCommandSecrets(const std::string& input);

// Saca la versión de OpenZFS de una salida en texto libre. Vacío si no la encuentra o
// si el número mayor pasa de 10, que delata una coincidencia falsa.
std::string parseOpenZfsVersionText(const std::string& text);

// Trocea la salida de `zpool import`.
std::vector<ImportablePoolInfo> parseZpoolImportOutput(const std::string& text);

// --- Invocación por SSH y del agente.
//
// Se apoyan en ConnectionProfile, que es lo que las mantenía atadas a Qt.
std::string sshUserHost(const ConnectionProfile& p);
std::string sshUserHostPort(const ConnectionProfile& p);
std::string sshAddressFamilyOption(const ConnectionProfile& p);
std::string sshBaseCommand(const ConnectionProfile& p);
std::string buildSshTargetPrefix(const ConnectionProfile& p);
std::string buildSimpleSshInvocation(const ConnectionProfile& p, const std::string& remoteCmd);
std::string buildSshPreviewCommandText(const ConnectionProfile& p, const std::string& remoteCmd);

// Los mismos argumentos, pero como lista para lanzar `scp` DIRECTAMENTE, sin intérprete.
// `multiplex` a false omite ControlMaster/ControlPersist/ControlPath: el OpenSSH de
// Windows no admite multiplexado.
std::vector<std::string> scpUploadArgs(const ConnectionProfile& p,
                                       const std::string& localPath,
                                       const std::string& remotePath,
                                       bool multiplex);

std::string withSudoCommand(const ConnectionProfile& p, const std::string& cmd);
std::string withSudoStreamInputCommand(const ConnectionProfile& p, const std::string& cmd);
std::string agentCommand(const ConnectionProfile& p, const std::string& agentArgs);
std::string agentShellCommand(const ConnectionProfile& p,
                              const std::vector<std::string>& agentArgs);
std::string agentShellCommandStreamInput(const ConnectionProfile& p,
                                         const std::vector<std::string>& agentArgs);

}  // namespace zfsmgr::base::helpers
