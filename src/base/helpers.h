#pragma once

#include <string>

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

}  // namespace zfsmgr::base::helpers
