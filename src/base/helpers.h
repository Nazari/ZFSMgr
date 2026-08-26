#pragma once

#include <memory>
#include <string>
#include <vector>

#include <map>

#include "connectionprofile.h"

// Building commands, and predicates over ZFS values, WITHOUT Qt.
//
// The second piece of the base layer, ported by hand from `mwhelpers`.
// `src/mainwindow_helpers.cpp` remains as an adapter so the call sites need not be touched;
// see docs/diseno_tecnico_capa_base_sin_qt.md.
//
// What is NOT here, and why, is in that same document: the ones taking `ConnectionProfile`,
// and the ones using regular expressions, JSON, processes or the filesystem.
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
    bool sendEnabled{false};
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

// Collapses whitespace and trims to `maxLen` CHARACTERS —not bytes—, so a line can be
// written into the log without splitting a UTF-8 character in half.
std::string oneLine(const std::string& v, int maxLen = 220);

// The values ZFS uses to say yes in the `mounted` property.
bool isMountedValueTrue(const std::string& value);

// «pool/a/b» -> «pool/a». Empty when there is no parent, the pool's root included.
std::string parentDatasetName(const std::string& dataset);

bool isWindowsOsType(const std::string& osType);

// When the parent does not mount —no mountpoint, «none», or `canmount=off`—, requiring it to
// be mounted before mounting the child makes no sense.
bool parentMountCheckRequired(const std::string& parentMountpoint,
                              const std::string& parentCanmount);
bool parentAllowsChildMount(const std::string& parentMountpoint,
                            const std::string& parentCanmount,
                            const std::string& parentMounted);

// Mount commands. On Windows they come out as PowerShell and on Unix as POSIX shell, which
// is the difference that forces carrying `isWindows` all the way down here.
std::string buildRecursiveUmountCommand(bool isWindows, const std::string& datasetName);
std::string buildSingleUmountCommand(bool isWindows, const std::string& datasetName);
std::string buildSingleMountCommand(const std::string& datasetName);
std::string buildMountChildrenCommand(bool isWindows, const std::string& datasetName);
std::string buildWindowsMountPrecheckCommand(const std::string& datasetName,
                                             const std::string& effectiveMountpoint);

// Pipeline transfers. `pv` is optional: when it is missing, it goes through `cat`.
std::string streamProgressPipeFilter();
std::string buildPipedTransferCommand(const std::string& sendSegment,
                                      const std::string& recvSegment);
std::string streamCodecName(StreamCodec codec);
StreamCodec chooseStreamCodec(bool hasZstdBoth, bool hasGzipBoth);
std::string buildTarSourceCommand(bool isWindows, const std::string& mountPath, StreamCodec codec);
std::string buildTarDestinationCommand(bool isWindows, const std::string& mountPath, StreamCodec codec);

// Prepends the directories `zfs` usually lives in, for when a non-interactive session's PATH
// does not carry them.
std::string withUnixSearchPathCommand(const std::string& cmd);

std::string storedSecretMarkerPrefix();

// Discards whatever precedes the first '{': some commands print warnings before the JSON.
std::string stripToJson(const std::string& output);

// Encodes each UTF-8 BYTE as \0ddd for `printf '%b'`, so that the result is pure ASCII.
//
// It exists because of the sudo password on macOS: Qt decomposes the characters when handing
// the command to the shell, and sudo received bytes other than the ones typed.
std::string shPrintfOctalEscaped(const std::string& s);

// Path of the SSH multiplexing socket. It carries the %C marker, which ssh itself expands
// into a digest of user/host/port.
std::string sshControlPath();

// Which transfer buttons should end up enabled, given the selection.
TransferButtonState computeTransferButtonState(const TransferButtonInputs& in);

// Mountpoints repeated across datasets: it groups by mountpoint and returns only the ones
// with more than one.
std::map<std::string, std::vector<std::string>> duplicateMountpoints(
    const std::map<std::string, std::string>& datasetMountpoints);

// Mountpoints already occupied by a DIFFERENT dataset from the one being asked for.
std::vector<MountpointConflict> externalMountpointConflicts(
    const std::map<std::string, std::string>& targetDatasetMountpoints,
    const std::map<std::string, std::vector<std::string>>& mountedByMountpoint);

// Masks the secret of the verbs that carry one, so the invocation can be written into the
// log.
std::string maskedAgentArgvForLog(const std::vector<std::string>& argv);

// Keeps the drive LETTER only, upper-cased. Empty when there is none.
std::string normalizeDriveLetterValue(const std::string& raw);

// Explains the two host-key failures that have a known remedy. Empty when the error is
// anything else.
std::string sshHostKeyProblemHint(const std::string& sshStderr);

// Readable name of a GPT partition-type GUID. Empty when it is not known.
std::string windowsGptTypeName(const std::string& guid);
std::string formatWindowsFsTypeDetail(const std::string& rawFsType);
// Partitions and disks that must NOT be offered to ZFS: system, recovery, reserved, and the
// boot disk.
bool windowsPartitionTypeIsProtected(const std::string& rawFsType);

// El disco al que pertenece una partición de Windows, o vacío si no lo es.
//
// En Windows ZFS nombra sus vdev `\\?\Harddisk<N>Partition<M>` —es lo que sale en
// `zpool status` y lo único que `zpool create` acepta— mientras que el disco entero se
// llama `\\.\PhysicalDrive<N>`. Los dos números son el mismo disco, pero los nombres no
// se parecen, así que la traducción hay que hacerla a mano.
std::string windowsPartitionDiskPath(const std::string& partitionPath);

// La primera partición de un disco entero de Windows, o vacío si no es un disco entero.
// `\\.\PhysicalDrive1` -> `\\?\Harddisk1Partition1`. Es la partición que `zpool create`
// acaba de escribir cuando se le da el disco entero.
std::string windowsWholeDiskFirstPartition(const std::string& diskPath);

// ¿Este fallo de `zpool create` es el de «etiquetó el disco y no llegó a crear el pool»?
//
// En Windows, dar el disco entero hace que zpool bloquee el volumen, escriba la GPT y
// después vuelva a abrir la partición recién creada. Ese último paso falla a menudo con
// EINVAL, y el disco se queda etiquetado y sin pool. Se reconoce por las dos cosas juntas:
// la etiqueta escrita y el EINVAL.
bool windowsPoolCreateLabeledButFailed(const std::string& output);

// El nombre mDNS que merece la pena probar cuando el anfitrión no resuelve, o vacío.
//
// Un perfil con `host = mbp` deja la conexión en ERROR con la línea cruda de ssh, y eso se
// lee igual que «la máquina está apagada» o «el daemon no responde». Peor: el nombre corto
// puede haber resuelto durante meses —el que anuncia el DHCP— y dejar de hacerlo sin que
// nadie toque nada, de modo que el fallo aparece muy lejos de su causa.
//
// Solo se ofrece cuando tiene sentido: el error es de resolución, y el nombre es corto —sin
// puntos y sin pinta de dirección IP—, que es el único caso en el que añadir `.local` puede
// arreglar algo. NO comprueba si ese nombre resuelve: eso es una consulta de red y esto se
// llama desde el hilo de la interfaz. Sugiere, no afirma.
std::string mdnsFallbackHost(const std::string& sshError, const std::string& host);

// Verbs that exist only on the agent's command line, never over RPC.
bool isCliOnlyAgentCommand(const std::string& verb);

// Splits the way a POSIX shell would. It survives only as the oracle for the tests of the
// string rendering.
std::vector<std::string> posixShellSplitArgs(const std::string& s);

// Replaces each password with a marker, so the command can be written to disk without
// writing the secret. It covers BOTH forms: the octal one from shPrintfOctalEscaped and the
// literal one. When after substituting the secret STILL appears, it returns empty and sets
// `okOut` to false: losing the command beats writing a password.
std::string redactSecretsForStorage(const std::string& command,
                                    const std::vector<StorableSecret>& secrets,
                                    bool* okOut);
std::string restoreSecretsFromStorage(const std::string& stored,
                                      const std::vector<StorableSecret>& secrets);

// Looks for an executable on the PATH and, when it does not turn up, in the usual
// directories.
//
// It returns the absolute path, or empty when it is not there. The directory fallback is not
// an ornament: on macOS a process launched from the Finder inherits a minimal PATH that does
// NOT include /opt/homebrew/bin, so `sshpass` was installed and still could not be found.
//
// On Windows the PATHEXT extensions are tried as well, because plain «ssh» is not the name of
// any file.
std::string findLocalExecutable(const std::string& name);

// When the command has anything outside ASCII, it rewrites it as
// `eval "$(printf '%b' '...')"`. See shPrintfOctalEscaped: on macOS Qt decomposed the
// characters as they were passed along.
std::string asciiSafeShellCommand(const std::string& cmd);

// Is this a PASSWORD rejection? Different from an authorisation failure, where typing it
// again fixes nothing and therefore no retry is offered.
bool looksLikeSudoAuthFailure(const std::string& text);

struct ImportablePoolInfo {
    std::string pool;
    std::string guid;
    std::string state;
    std::string reason;
};

// Masks the secrets of a command so it can be written into the log. It covers the specific
// shapes this application builds —not a generic «password»— and that is why there are seven
// patterns.
std::string maskCommandSecrets(const std::string& input);

// The same, but for the OUTPUT of a command before writing it into the log.
//
// It is needed because the daemon's TLS material is read by running a command, and its
// output —the client's private key, in full— was being dumped into the log line by line:
// with `zfsmgr-cli -v` it came out on standard error, which is where people copy and paste
// from. It is the key used to talk to the daemon as root.
//
// What is trimmed is the inside, not the whole line: the path and the markers stay, because
// they are exactly what serves to diagnose that the material was read, and from where.
std::string maskSecretOutput(const std::string& input);

// Pulls the OpenZFS version out of free-form output. Empty when it is not found, or when the
// major number is above 10, which gives away a false match.
std::string parseOpenZfsVersionText(const std::string& text);

// Splits the output of `zpool import`.
std::vector<ImportablePoolInfo> parseZpoolImportOutput(const std::string& text);

// Qué significa que la sonda de importables no encuentre nada.
//
// «No hay ninguno» y «no puedo mirar» se leen IGUAL en la salida de `zpool import`: las dos
// dicen «no pools available to import». El agente distingue una de otra —intenta abrir un
// dispositivo y mira el errno— y lo cuenta con marcas en su salida; esto las lee.
//
// El tercer estado existe y no se puede afirmar con certeza desde el agente: para saber si
// el permiso ESTÁ concedido habría que leer la base de TCC, y leerla exige justo el permiso
// que falta. Lo único honesto que hay es la fecha en que esa base cambió por última vez: si
// es POSTERIOR al arranque del agente, lo más probable es que se acabe de conceder y falte
// reiniciarlo, porque macOS decide el permiso al arrancar el proceso. Por eso el estado se
// llama «probablemente».
enum class ImportProbeDiagnosis {
    PoolsFound,             // hay importables: no hay nada que explicar
    NothingToImport,        // se puede mirar y no hay ninguno: NO se avisa de nada
    DisksUnreadable,        // no se pueden leer los discos: falta el permiso
    ProbablyNeedsRestart,   // no se pueden leer, pero el permiso cambió tras arrancar
};

struct ImportProbeReading {
    ImportProbeDiagnosis diagnosis{ImportProbeDiagnosis::NothingToImport};
    std::string device;   // el dispositivo concreto que no se pudo abrir, si se sabe
};

ImportProbeReading readImportProbe(const std::string& probeOutput, bool anyPoolParsed);

// En qué ORDEN se aplican las propiedades que se han tocado en un borrador.
//
// El orden es el de CREACIÓN: el mismo en que el usuario las fue tocando, de la más antigua
// a la más nueva. Ni alfabético, ni por prioridades que decida el programa — lo único
// predecible es reproducir lo que hizo quien lo hizo.
//
// No es un detalle estético. `canmount=on` DISPARA el montaje usando el `mountpoint` que
// haya puesto en ese instante, así que el orden decide DÓNDE se monta. El 2026-08-26 se
// aplicó `canmount` antes que `mountpoint` y ZFS intentó montar en el punto anterior, que
// era `/Users/linarese`: el directorio de trabajo del usuario. Solo se salvó porque no
// estaba vacío. Y ni siquiera era alfabético: salía de un conjunto sin orden definido, o
// sea arbitrario y capaz de cambiar entre ejecuciones.
//
// `recorded` es la secuencia tal y como se fue anotando. Lo que no aparezca en ella va
// detrás y por orden alfabético: no debería ocurrir, pero si ocurre más vale que sea
// repetible que aleatorio.
std::vector<std::string> datasetPropertyApplyOrder(const std::vector<std::string>& props,
                                                   const std::vector<std::string>& recorded);

// --- SSH and agent invocation.
//
// They lean on ConnectionProfile, which is what used to keep them tied to Qt.
std::string sshUserHost(const ConnectionProfile& p);
std::string sshUserHostPort(const ConnectionProfile& p);
std::string sshAddressFamilyOption(const ConnectionProfile& p);
std::string sshBaseCommand(const ConnectionProfile& p);
std::string buildSshTargetPrefix(const ConnectionProfile& p);
std::string buildSimpleSshInvocation(const ConnectionProfile& p, const std::string& remoteCmd);
std::string buildSshPreviewCommandText(const ConnectionProfile& p, const std::string& remoteCmd);

// How a command's arguments travel in the daemon's generic verbs: a JSON list of strings,
// base64-encoded.
//
// **It is a contract of the DAEMON, not of the caller.** It used to be written out fourteen
// times —eleven in the main window, two in the shell and one in the web server—, each one
// assembling the same JSON by hand. Fourteen places to get it wrong separately the day that
// format changes, and none of the clients has any business knowing how it is serialised.
std::string agentArgv(const std::vector<std::string>& argv);

// Hands a secret to a child over a DESCRIPTOR, never on the command line.
//
// `sshpass -p <password>` leaves the password in the argv, and any process's argv is readable
// by everyone with `ps`. sshpass wipes it as soon as it starts —which is why `ps` shows
// spaces where it was— but between the `exec` and that wipe there is a real window, and
// looking at the right moment is enough. The house rule is that secrets travel by descriptor
// or by terminal, never by argument and never by environment variable (sshpass's `-e` is no
// good either: the environment is readable at /proc/<pid>/environ).
//
// **A pipe is read ONCE.** Whoever retries a launch has to build another object; that is why
// this is short-lived and created just before each `exec`, not once per connection. With the
// second attempt reading from an already-drained pipe, authentication would fail without
// saying why.
//
// The descriptor is deliberately left WITHOUT CLOEXEC: here inheritance is exactly what is
// wanted, the opposite of what sockets need.
class SecretFromDescriptor {
public:
    explicit SecretFromDescriptor(const std::string& secret);
    ~SecretFromDescriptor();
    SecretFromDescriptor(const SecretFromDescriptor&) = delete;
    SecretFromDescriptor& operator=(const SecretFromDescriptor&) = delete;

    // False when the pipe could not be built, or on Windows, where there is no sshpass.
    bool ok() const { return m_fd >= 0; }
    int descriptor() const { return m_fd; }
    // The option exactly as sshpass expects it, run together: «-d7».
    std::string sshpassOption() const;

private:
    int m_fd{-1};
};

// Uploading a file over scp: THE PROGRAM AND THE ARGUMENTS together.
//
// Together because they cannot be decided separately: when the connection uses a password,
// `sshpass` has to be launched instead of `scp`, and `BatchMode=no` has to be set as well.
// Returning only the arguments forced the caller to remember both, and it did not: deploying
// the daemon to a password-authenticated machine failed with «Connection closed».
//
// `multiplex` set to false omits ControlMaster/ControlPersist/ControlPath: the OpenSSH on
// Windows does not support multiplexing.
struct ScpInvocation {
    std::string program;
    std::vector<std::string> args;
    // The pipe the password travels down, when there is one. It goes INSIDE the invocation
    // because it has to stay open until the caller launches the process: were it closed on
    // returning from scpUpload, sshpass would read from a dead descriptor.
    std::shared_ptr<SecretFromDescriptor> secret;
};
ScpInvocation scpUpload(const ConnectionProfile& p,
                        const std::string& localPath,
                        const std::string& remotePath,
                        bool multiplex);

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
