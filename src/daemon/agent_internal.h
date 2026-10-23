#pragma once

// What the agent's translation units (src/daemon/*.cpp) share: types, constants, global state
// (extern) and function declarations. Internal to the agent: nothing outside src/daemon
// includes it. It was produced by splitting the former single-file daemon_main.cpp, and keeps
// that file's order.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <pwd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// RegGetValueA: la versión y el identificador de máquina se leen del registro, porque
// en Windows no hay uname ni /etc/machine-id que consultar.
#include <windows.h>
// CoCreateGuid, para nombrar directorios temporales sin colisiones entre conexiones.
#include <objbase.h>
#ifndef _PID_T_
#define _PID_T_
typedef int pid_t;
#endif
#endif
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include "requests.h"
#include "remotetree.h"
#include "compresion.h"
#include "copytree.h"
#include "json.h"
#include "peers.h"
#include "base/processes.h"
#include "base/osinfo.h"
#include "commands/advanced.h"
#include "commands/gsa.h"
#include "base/agentmsg.h"
#include "base/i18n.h"
#include "base/strutil.h"
#include "base/tlsserver.h"
#include "base/tlsclient.h"
#include "base/transportcmd.h"
#include "commands/zfsprops.h"
#include "commands/transfer.h"
#include <openssl/x509v3.h>
#include <openssl/ssl.h>

#ifdef HAVE_LIBZFS_CORE
// libzfs_core userspace headers (e.g. from libzfslinux-dev on Ubuntu) rely on
// Solaris/illumos compat types that are not automatically pulled in on Linux.
// Define the minimum set needed before including the ZFS headers.
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#ifndef B_FALSE
typedef enum { B_FALSE = 0, B_TRUE = 1 } boolean_t;
#endif
#ifndef _UINT_T
#define _UINT_T
typedef unsigned int uint_t;
#endif
#ifndef _UCHAR_T
#define _UCHAR_T
typedef unsigned char uchar_t;
#endif
#ifndef _HRTIME_T
#define _HRTIME_T
typedef int64_t hrtime_t;
#endif
#include <libzfs_core.h>
#include <libnvpair.h>
#ifndef ZFS_MAX_DATASET_NAME_LEN
#define ZFS_MAX_DATASET_NAME_LEN 256
#endif
#endif

#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <limits.h>
#else
#include <io.h>
#endif

namespace zfsmgr::agent {

#ifdef _WIN32
using NativeSock = SOCKET;

constexpr NativeSock kInvalidSock = INVALID_SOCKET;

inline void closeSock(NativeSock s) {
    closesocket(s);
}
#else
using NativeSock = int;

constexpr NativeSock kInvalidSock = -1;

inline void closeSock(NativeSock s) {
    close(s);
}
#endif
#ifdef _WIN32
// En Windows no hay /tmp: el ofstream fallaba y writeHeartbeat retornaba en silencio,
// así que el latido nunca se registraba.
constexpr const char* kHeartbeatPath = "C:\\ProgramData\\ZFSMgr\\agent\\heartbeat.log";

#else
constexpr const char* kHeartbeatPath = "/tmp/zfsmgr-agent-heartbeat.log";

#endif
#ifdef _WIN32
constexpr const char* kDaemonLogFile = "C:\\ProgramData\\ZFSMgr\\agent\\daemon.log";

#else
constexpr const char* kDaemonLogFile = "/var/lib/zfsmgr/daemon.log";

#endif
constexpr long long kDaemonLogMaxBytes = 2 * 1048576LL;

#ifdef _WIN32
constexpr const char* kJobsFilePath = "C:\\ProgramData\\ZFSMgr\\agent\\jobs.json";

#else
constexpr const char* kJobsFilePath = "/var/lib/zfsmgr/jobs.json";

#endif
#ifdef _WIN32
constexpr const char* kDefaultAgentConfigPath = "C:\\ProgramData\\ZFSMgr\\agent\\agent.conf";

constexpr const char* kDefaultTlsDir = "C:\\ProgramData\\ZFSMgr\\agent\\tls";

#else
constexpr const char* kDefaultAgentConfigPath = "/etc/zfsmgr/agent.conf";

constexpr const char* kDefaultTlsDir = "/etc/zfsmgr/tls";

#endif
#ifdef _WIN32
constexpr const char kPathSep = '\\';

#else
constexpr const char kPathSep = '/';

#endif
constexpr const char* kDefaultBind = "127.0.0.1";

constexpr int kDefaultPort = 47653;

const std::string kDefaultTlsCertPath_s = std::string(kDefaultTlsDir) + kPathSep + "server.crt";

const char* const kDefaultTlsCertPath = kDefaultTlsCertPath_s.c_str();

const std::string kDefaultTlsKeyPath_s = std::string(kDefaultTlsDir) + kPathSep + "server.key";

const char* const kDefaultTlsKeyPath = kDefaultTlsKeyPath_s.c_str();

const std::string kDefaultTlsClientCertPath_s =
    std::string(kDefaultTlsDir) + kPathSep + "client.crt";

const char* const kDefaultTlsClientCertPath = kDefaultTlsClientCertPath_s.c_str();

#ifdef _WIN32
constexpr const char* kDefaultCommandPath =
    "C:\\Program Files\\OpenZFS On Windows;C:\\Windows\\System32;C:\\Windows";

#else
constexpr const char* kDefaultCommandPath =
    "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/"
    "sbin:/sbin:/usr/bin:/bin";

#endif
// ¿Nos deja el sistema leer los discos?
//
// En macOS, «root» no basta: leer un dispositivo en crudo exige que el binario tenga
// concedido «Acceso total al disco». Sin él, `zpool import` no falla — dice «no pools
// available to import», exactamente igual que cuando de verdad no hay ninguno—. El
// usuario ve una lista vacía y no tiene forma de distinguir «no hay» de «no puedo mirar»:
// pasó con un pool en un disco externo que la propia máquina enseñaba en `diskutil list`.
//
// Se comprueba abriendo para lectura la primera rodaja de disco que haya: si eso da
// EPERM/EACCES, lo que falta es el permiso. En el resto de plataformas no aplica y la
// función dice que no hay problema.
const char* const kMarcaDiscosIlegibles = "__ZFSMGR_DISCOS_ILEGIBLES__\n";

const char* const kMarcaDiscoIlegible = "__ZFSMGR_DISCO_ILEGIBLE__ ";

const char* const kMarcaTccTrasArranque = "__ZFSMGR_TCC_TRAS_ARRANQUE__\n";

extern const std::chrono::system_clock::time_point g_arranqueDelProceso;
bool permisosCambiaronTrasArranque();
std::string discoIlegible();
constexpr const char* kApiVersion = "3";

#include "zfsmgr/agentversion_generated.h"
#ifndef ZFSMGR_AGENT_VERSION_STRING
#define ZFSMGR_AGENT_VERSION_STRING ZFSMGR_APP_VERSION
#endif
extern std::atomic<bool> g_stop;
extern std::mutex g_daemonLogMutex;
void daemonLog(const std::string& level, const std::string& msg);  // forward decl
enum class JobState { Queued, Running, Done, Failed, Cancelled };

struct DaemonJob {
    std::string id;
    std::string type;  // "send-to-peer" | "file-to-peer" | "pipe-local" | "mutation"
    std::string snap;
    std::string peerHost;
    int peerPort{0};
    std::string token;
    std::string baseSnap;
    std::string sendFlags;
    // Testigo de reanudación. Cuando viene, `zfs send -t` continúa la transfer
    // cortada en vez de mandarlo todo otra vez, y snap/baseSnap/sendFlags se ignoran.
    std::string resumeToken;
    std::string pipeCmd;       // base64-encoded shell command (pipe-local only)
    std::string dstDataset;    // destination dataset (pipe-local only)
    std::string submittedCmd;  // agent command being run (type "mutation")
    std::string resultOut;     // captured stdout of a "mutation" job
    int resultRc{0};           // exit code of a "mutation" job

    JobState state{JobState::Queued};
    pid_t sendPid{-1};
    uint64_t bytesTransferred{0};
    double rateMiBs{0.0};
    long elapsedSecs{0};
    std::string startedAtUtc;
    std::string finishedAtUtc;
    std::string errorText;
    // El error como código (agentmsg), para que el CLI lo diga en su idioma.
    std::string errorCode;
    std::vector<std::string> errorArgs;
    std::vector<std::string> progressLines;  // ring buffer, max 5
    // Qué hace, para quien mira `jobs`: «cp /origen -> /destino (+2)», «send pool/ds@s ->
    // host:puerto». Se compone al recibirlo, que es cuando se tienen los argumentos; el
    // verbo solo (`--mutate-copy-local`) no dice qué se está copiando ni adónde.
    std::string description;
};

void ponErrorDeTrabajo(DaemonJob& j, const std::string& codigo, std::vector<std::string> args);
extern std::mutex g_jobsMutex;
extern std::unordered_map<std::string, DaemonJob> g_jobs;
extern thread_local std::string t_currentJobId;
bool jobCancelRequested();
void reportJobProgress(const std::string& line);
void reportJobCounters(uint64_t bytes, double rateMiBs, long elapsedSecs);
void persistJobsLocked();
void loadPersistedJobsAtStartup();
void runZfsSendToPeerAsync(const std::string& jobId);
// ExecResult y la ejecución de procesos vienen de la capa base: son los mismos que usa
// el cliente, así que una corrección beneficia a los dos.
using zfsmgr::base::ExecResult;

void ponCodigo(ExecResult& r, const std::string& codigo, std::vector<std::string> args);
void ponFallo(ExecResult& r, int rc, const std::string& codigo, std::vector<std::string> args);
ExecResult fallo(int rc, const std::string& codigo, std::vector<std::string> args);
using zfsmgr::base::runExecCapture;

using zfsmgr::base::runExecCaptureWithStdin;

using zfsmgr::base::runExecStreaming;

#ifdef _WIN32
// Solo existe en Windows, igual que sus llamantes.
using zfsmgr::base::winBuildCommandLine;

#endif
struct AgentRuntimeConfig {
    std::string bind{kDefaultBind};
    int port{kDefaultPort};
    std::string tlsCert{kDefaultTlsCertPath};
    std::string tlsKey{kDefaultTlsKeyPath};
    std::string tlsClientCert{kDefaultTlsClientCertPath};
    std::string commandPath{kDefaultCommandPath};
    int cacheTtlFastMs{2000};
    int cacheMaxEntries{512};
    std::string transferBindAddr{"0.0.0.0"};
};

std::string trim(const std::string& s);
std::string toLower(std::string s);
std::vector<std::string> splitLines(const std::string& s);
std::string firstLineTrimmed(const std::string& raw);
bool isTruthyValue(const std::string& raw);
ExecResult getZfsPropertyCapture(const std::string& dataset, const std::string& prop);
ExecResult getDatasetMountpointCapture(const std::string& dataset);
std::string agentCapabilityList();
std::string makeTempDirIn(const std::string& baseDir, const char* patternPrefix);
std::string makeTempDir(const char* patternPrefix);
long long datasetUsedBytes(const std::string& ds);
std::string humanBytes(long long n);
// Informa del avance MIENTRAS dura una copia, muestreando cuánto ha crecido un dataset.
//
// Existe porque avisar solo al empezar y al terminar cada elemento no basta: si un
// elemento se lleva cuatro minutos —medido con 27 GB—, un contador que dice "1 de 13"
// y no se mueve es indistinguible de un cuelgue.
//
// Se muestrea el dataset en vez de leer la salida de rsync: ZFS ya lleva la cuenta, no
// hay que transmitir ni parsear nada, y sirve igual para el desglose (crece el dataset
// temporal) que para el ensamblado (crece el padre, donde está la escala).
class CopyProgressSampler {
public:
    CopyProgressSampler(std::string dataset, std::string prefix)
        : dataset_(std::move(dataset)), prefix_(std::move(prefix)) {
        baseline_ = datasetUsedBytes(dataset_);
        started_ = std::chrono::steady_clock::now();
        // El identificador del trabajo se lee AQUÍ, en el hilo que construye, y se
        // reinstala dentro del hilo muestreador: t_currentJobId es thread_local, así
        // que en un hilo nuevo nace vacío y reportJobProgress descartaba todo en
        // silencio. Es el mismo mecanismo que yo mismo introduje, y lanzar un hilo que
        // depende de él sin propagarlo lo dejaba mudo.
        const std::string jobId = t_currentJobId;
        worker_ = std::thread([this, jobId]() {
            t_currentJobId = jobId;
            run();
        });
    }
    ~CopyProgressSampler() {
        stop_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }
        // UNA muestra más, ya con la copia terminada.
        //
        // El hilo muestrea cada dos segundos, así que el último valor que dejaba era el de
        // la penúltima vuelta: una copia de 2,5 GiB quedaba anotada como 1,9 GiB, y una que
        // durase menos de dos segundos como CERO. El número se lee después, cuando el
        // trabajo ya está en «done» y nadie va a volver a mirarlo: tiene que ser el total,
        // no el de hace dos segundos. Aquí ya no hay nada copiando, así que lo que diga el
        // dataset es el resultado final.
        if (baseline_ >= 0) {
            const long long fin = datasetUsedBytes(dataset_);
            if (fin > baseline_) {
                const long long copied = fin - baseline_;
                const double secs =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started_)
                        .count();
                reportJobCounters(
                    static_cast<uint64_t>(copied),
                    secs > 0.0 ? (static_cast<double>(copied) / secs) / (1024.0 * 1024.0) : 0.0,
                    static_cast<long>(secs));
            }
        }
    }
    CopyProgressSampler(const CopyProgressSampler&) = delete;
    CopyProgressSampler& operator=(const CopyProgressSampler&) = delete;

private:
    void run() {
        while (!stop_) {
            // Troceado para que el destructor no espere dos segundos a que salga.
            for (int i = 0; i < 20 && !stop_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stop_ || baseline_ < 0) {
                continue;
            }
            const long long now = datasetUsedBytes(dataset_);
            if (now < 0 || now <= baseline_) {
                continue;
            }
            const long long copied = now - baseline_;
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            std::string line = prefix_ + " — " + humanBytes(copied) + " copiados";
            if (secs > 1.0) {
                line += " a " + humanBytes(static_cast<long long>(copied / secs)) + "/s";
            }
            reportJobProgress(line);
            // Y las MISMAS cifras a los contadores. El ritmo en MiB/s, que es lo que dice el
            // nombre del campo y lo que rotula la columna; el de la frase va en unidades
            // legibles y no es el mismo número.
            reportJobCounters(
                static_cast<uint64_t>(copied),
                secs > 0.0 ? (static_cast<double>(copied) / secs) / (1024.0 * 1024.0) : 0.0,
                static_cast<long>(secs));
        }
    }
    std::string dataset_;
    std::string prefix_;
    long long baseline_{-1};
    std::chrono::steady_clock::time_point started_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

bool datasetIsMounted(const std::string& name);
std::vector<std::string> conExclusionesDelSistema(std::vector<std::string> excludes);
ExecResult runCopyTreeCapture(const std::string& srcDir, const std::string& dstDir,
                              const std::vector<std::string>& excludes = {},
                              bool oneFileSystem = false);
ExecResult runDumpAdvancedBreakdownListCapture(const std::string& dataset,
                                               const std::string& rootGiven = {}, int depth = 0);
int countPendingCopyTransfers(const std::string& srcDir, const std::string& dstDir,
                              const std::vector<std::string>& excludes = {},
                              bool oneFileSystem = false);
#ifndef _WIN32
bool removeTreeWithoutCrossingMounts(const std::filesystem::path& node, dev_t dev,
                                     std::string& err);
#endif
#ifndef _WIN32
// ── Punto de montaje TEMPORAL para escribir en un dataset sin montar ─────────
//
// Desglosar, Ensamblar, Hacia Dir y la sincronización por tar necesitan el dataset montado y
// con escritura. Si no lo está —un entorno de arranque con `mountpoint=/` y `canmount=noauto`
// no puede montarse «en su sitio» sin taparle la raíz a la máquina—, se le pone un punto de
// montaje temporal en /tmp mientras dura la operación y después se le devuelve el suyo.
//
// El original se anota en dos propiedades de usuario ANTES de tocar nada: el valor y de
// dónde venía. Si la operación muere a medias —SIGKILL, un corte de luz, el daemon
// sustituido—, son la miga de pan con la que `repair-mounts` lo deja como estaba.
//
// De dónde venía importa: `kali` lo tenía RECIBIDO (`mountpoint=/ received`), y devolvérselo
// con `zfs set` lo dejaba como `local`, que ya no sigue a lo que llegue en el siguiente envío.
constexpr const char* kSavedMountpointProp = "org.zfsmgr:savedmountpoint";

constexpr const char* kSavedMountpointSourceProp = "org.zfsmgr:savedmountpointsource";

std::string origenDeMountpoint(const std::string& ds);
bool restauraMountpoint(const std::string& ds, const std::string& valor, const std::string& origen,
                        std::string& err, const std::set<std::string>& yaMontados = {});
void desmontaBajo(const std::string& ds, const std::string& dir);
std::set<std::string> montadosDelArbol(const std::string& ds);
struct AltMountGuard {
    std::string dataset;
    std::string tmpDir;
    bool active{false};
    // Lo que del árbol estaba montado ANTES de empezar. Al terminar, cualquier otro que haya
    // quedado montado se desmonta: es la red bajo restauraMountpoint, por si ZFS vuelve a
    // montar algo por su cuenta.
    std::set<std::string> montadosAntes;
    // Puntos de montaje que solo se pueden fijar con todo YA desmontado: uno explícito
    // puesto ahora, con el padre en /tmp, montaría el dataset en esa ruta de la máquina.
    std::vector<std::pair<std::string, std::string>> mountpointsAlFinal;

    // Moves the dataset onto a scratch mountpoint and mounts it there.
    bool engage(const std::string& ds, std::string& mpOut, std::string& errMsg) {
        char tmpl[] = "/tmp/zfsmgr-sync-XXXXXX";
        const char* dir = mkdtemp(tmpl);
        if (!dir) {
            errMsg = "cannot create temporary mountpoint\n";
            return false;
        }
        dataset = ds;
        tmpDir = dir;
        montadosAntes = montadosDelArbol(ds);
        const ExecResult cur = getZfsPropertyCapture(ds, "mountpoint");
        const std::string savedMp = (cur.rc == 0) ? trim(cur.out) : std::string();
        const std::string savedSrc = origenDeMountpoint(ds);
        // Record the original mountpoint before touching anything.
        (void)runExecCapture("zfs", {"set", std::string(kSavedMountpointProp) + "=" + savedMp, ds});
        if (!savedSrc.empty()) {
            (void)runExecCapture(
                "zfs", {"set", std::string(kSavedMountpointSourceProp) + "=" + savedSrc, ds});
        }
        // Con `-u`, y montando después SOLO el dataset. Sin él, ZFS montaba en el acto los
        // hijos con `canmount=on` dentro del directorio vacío de /tmp, y el padre —montado
        // después— quedaba ENCIMA tapándolos: el hijo seguía montado, pero su ruta caía dentro
        // del padre y ni se veía ni se podía desmontar por ella (medido al Ensamblar un entorno
        // de arranque). Los hijos los monta, en orden, quien los necesite.
        // Montado, `-u` le cambiaría la propiedad dejándolo donde estaba: se desmonta antes.
        if (datasetIsMounted(ds)) {
            (void)runExecCapture("zfs", {"unmount", ds});
        }
        const ExecResult setRes = runExecCapture("zfs", {"set", "-u", "mountpoint=" + tmpDir, ds});
        if (setRes.rc != 0) {
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, ds});
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointSourceProp, ds});
            rmdir(tmpDir.c_str());
            tmpDir.clear();
            errMsg = setRes.err.empty() ? "cannot set temporary mountpoint\n" : setRes.err;
            return false;
        }
        active = true;  // from here on the dataset must be restored
        // `zfs set mountpoint=` YA remonta el dataset si estaba montado, así que montarlo
        // otra vez falla con "filesystem already mounted" —y eso abortaba la operación
        // entera por algo que en realidad había salido bien—. Solo se monta si hace falta.
        if (!datasetIsMounted(ds)) {
            const ExecResult mountRes = runExecCapture("zfs", {"mount", ds});
            if (mountRes.rc != 0) {
                errMsg =
                    mountRes.err.empty() ? "cannot mount on temporary mountpoint\n" : mountRes.err;
                return false;
            }
        }
        // Estar montado no basta: tiene que estarlo DONDE toca. Si por lo que sea siguiera
        // en su sitio anterior, copiar de ahí sería copiar de donde no es.
        const ExecResult mpNow = getDatasetMountpointCapture(ds);
        const std::string effective = (mpNow.rc == 0) ? trim(mpNow.out) : std::string();
        if (effective != tmpDir) {
            errMsg = "the dataset " + ds + " was mounted at " + effective + " and not at " + tmpDir
                     + "\n";
            return false;
        }
        mpOut = tmpDir;
        return true;
    }

    void release() {
        if (!active) {
            if (!tmpDir.empty()) {
                rmdir(tmpDir.c_str());
                tmpDir.clear();
            }
            return;
        }
        active = false;
        desmontaBajo(dataset, tmpDir);
        (void)runExecCapture("zfs", {"unmount", dataset});
        const ExecResult saved = getZfsPropertyCapture(dataset, kSavedMountpointProp);
        const std::string savedMp = (saved.rc == 0) ? trim(saved.out) : std::string();
        const ExecResult savedSrc = getZfsPropertyCapture(dataset, kSavedMountpointSourceProp);
        std::string origen = (savedSrc.rc == 0) ? trim(savedSrc.out) : std::string();
        if (origen == "-") {
            origen.clear();
        }
        std::string err;
        const bool vuelto = restauraMountpoint(dataset, savedMp, origen, err, montadosAntes);
        if (!vuelto) {
            daemonLog("ERROR", "cannot restore the mountpoint of " + dataset + ": " + err
                                   + " (repair-mounts will find it)");
        }
        // Con `-u`: fijado normal, montaría el dataset en esa ruta de la máquina.
        for (const auto& mp : mountpointsAlFinal) {
            (void)runExecCapture("zfs", {"set", "-u", "mountpoint=" + mp.second, mp.first});
        }
        for (const std::string& n : montadosDelArbol(dataset)) {
            if (!montadosAntes.count(n)) {
                (void)runExecCapture("zfs", {"unmount", n});
                daemonLog("WARN", n + " was mounted while restoring " + dataset
                                      + "'s mountpoint; unmounted again");
            }
        }
        // Y lo que estaba montado antes vuelve a estarlo, de menos profundo a más. Si quien
        // usó el temporal lo dejó a propósito sin montar —Hacia Dir pone canmount=off, o lo
        // destruye—, `zfs mount` falla y se queda así, que es lo que se quería.
        std::vector<std::string> volver(montadosAntes.begin(), montadosAntes.end());
        std::sort(volver.begin(), volver.end(), [](const std::string& a, const std::string& b) {
            return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
        });
        for (const std::string& n : volver) {
            if (!datasetIsMounted(n)) {
                (void)runExecCapture("zfs", {"mount", n});
            }
        }
        // La miga de pan solo se retira si el punto de montaje volvió: si no, se queda para
        // que `repair-mounts` lo encuentre.
        if (vuelto) {
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, dataset});
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointSourceProp, dataset});
        }
        if (!tmpDir.empty()) {
            rmdir(tmpDir.c_str());
            tmpDir.clear();
        }
    }

    ~AltMountGuard() { release(); }
};

#endif
ExecResult runMutateAdvancedBreakdownCapture(const std::vector<std::string>& params);
ExecResult runMutateAdvancedAssembleCapture(const std::vector<std::string>& params);
std::string stripQuotes(const std::string& s);
AgentRuntimeConfig loadRuntimeConfig();
void applyRuntimeEnvironment(const AgentRuntimeConfig& cfg);
std::string readFirstLineFile(const char* path);
std::string readDaemonLogRange(long long offset, long long maxBytes);
std::string utcNowIsoString();
std::string compactSpaces(std::string s);
int decodeWaitStatus(int status);
bool startsWith(const std::string& s, const std::string& pref);
void onSignal(int);
void writeHeartbeat();
void rotateDaemonLog();
void daemonLog(const std::string& level, const std::string& msg);
std::string detectOsLine();
std::string detectMachineUuid();
std::string detectZfsVersionRaw();
std::string detectZfsUnavailableReason();
bool isAllowedMutationOp(const std::string& tool, const std::string& opRaw);
int fromBase64(unsigned char c);
bool decodeBase64(const std::string& in, std::string& out);
bool parseJsonStringArray(const std::string& json, std::vector<std::string>& out);
std::string motivoParaNoCambiarClave(const std::string& dataset, const std::string& flagsStr);
ExecResult runGenericMutationCapture(const std::string& tool, const std::string& payloadB64);
ExecResult runPrepareDirCapture(const std::vector<std::string>& params);
ExecResult runRemoveContentCapture(const std::vector<std::string>& params);
ExecResult runRemoveTreeCapture(const std::vector<std::string>& params);
ExecResult runMutateAdvancedFromDirPrepareCapture(const std::vector<std::string>& params);
#ifndef _WIN32
extern std::atomic<pid_t> g_streamChildPid;
void onStreamSignal(int);
// ── Hacia Dir: dataset -> directorio plano ───────────────────────────────────
// Reimplementación fiel del shell antiguo, cuyas garantías se habían perdido al
// pasar a nativo. La versión intermedia hacía rsync DIRECTAMENTE sobre el destino y
// luego destruía el origen, de modo que un fallo a mitad dejaba el destino fusionado
// a medias y el dataset ya destruido, sin vuelta atrás.
//
// Se recuperan las tres garantías:
//   1. Se rechaza un destino que ya sea mountpoint ZFS. Escribir ahí metería los
//      datos dentro de otro dataset, y al desmontarlo parecerían haberse esfumado.
//   2. Se copia a un directorio temporal y se INTERCAMBIA. El destino queda con lo
//      viejo o con lo nuevo, nunca mezclado.
//   3. El destino anterior se aparta como respaldo y vuelve a su sitio si algo falla.
//
// El temporal se crea junto al destino, no en /tmp, para que el intercambio final
// sea un rename dentro del mismo sistema de ficheros y por tanto atómico. El shell
// usaba /tmp y dependía de que mv copiara entre sistemas de ficheros.
struct DestBackupGuard {
    std::filesystem::path target;
    std::filesystem::path backup;
    bool active{false};

    void commit() { active = false; }

    ~DestBackupGuard() {
        if (!active || backup.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(target, ec);
        std::filesystem::rename(backup, target, ec);
    }
};

std::string datasetMountedExactlyAt(const std::string& path);
ExecResult runMutateAdvancedToDirCapture(const std::vector<std::string>& params);
// ── Repair of datasets left on a temporary mountpoint ────────────────────────
// If a temp-tar run is killed outright (SIGKILL, power loss, the daemon being
// replaced mid-transfer), the dataset stays mounted on a scratch directory and keeps
// kSavedMountpointProp set. Neither the old shell scripts nor the RAII guard can
// cover that case, but the property is exactly the breadcrumb needed to find and undo
// it afterwards.
struct StrandedAltMount {
    std::string dataset;
    std::string savedMp;
    std::string currentMp;
};

std::vector<StrandedAltMount> findStrandedAltMounts();
ExecResult runRepairAltMountpointsCapture(bool apply);
int runStreamPair(const std::vector<std::string>& producer,
                  const std::vector<std::string>& consumer);
bool validSyncCodec(const std::string& codec);
int runSyncTempTarSource(const std::vector<std::string>& params);
int runSyncTempTarDest(const std::vector<std::string>& params);
int runMutateAdvancedFromDir(const std::vector<std::string>& params);
#endif  // _WIN32
#ifdef __APPLE__
std::set<std::string> zvolsExpuestosMacos();
#endif
ExecResult conDispositivoDeVolumen(ExecResult lista);
bool decodeZfsAllowBatch(const std::string& payloadB64, std::vector<std::vector<std::string>>& out,
                         std::string& errMsg);
ExecResult runZfsAllowBatchCapture(const std::string& payloadB64);
ExecResult runCopyLocalCapture(const std::string& payloadB64);
#ifndef _WIN32
bool decodeZfsPipeLocal(const std::string& payloadB64, std::vector<std::string>& sendArgv,
                        std::vector<std::string>& recvArgv, std::string& errMsg);
ExecResult ejecutaTuberiaZfsLocal(const std::vector<std::string>& sendArgv,
                                  const std::vector<std::string>& recvArgv);
ExecResult runZfsPipeLocalCapture(const std::string& payloadB64);
#else
ExecResult ejecutaTuberiaZfsLocal(const std::vector<std::string>&, const std::vector<std::string>&);
#endif  // _WIN32
int runMutateShellGeneric(const std::string& payloadB64);
#ifdef HAVE_LIBZFS_CORE
bool lzcEnsureInit();
ExecResult lzcSnapshotOne(const std::string& snapName);
ExecResult lzcDestroyOneSnap(const std::string& snapName);
ExecResult lzcRollbackTo(const std::string& dataset, const std::string& snapName);
ExecResult lzcClone(const std::string& snapOrigin, const std::string& newDataset);
bool lzcExists(const std::string& name);
#endif  // HAVE_LIBZFS_CORE
bool writeSelfSignedPair(const std::string& certPath, const std::string& keyPath,
                         const std::string& commonName, bool serverAuth, std::string& errMsg);
ExecResult runEnsureTlsCapture();
ExecResult runDumpRefreshBasicsCapture();
#ifdef _WIN32
bool isExecutableFile(const std::string& path);
bool lookupExecutable(const std::string& name);
ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64);
#endif
#ifndef _WIN32
// Directories probed on top of $PATH, mirroring the search list the old shell
// loop prepended: sudo often runs with a trimmed PATH that hides /usr/local/zfs
// or Homebrew.
const char* const kToolSearchDirs[] = {"/opt/homebrew/bin",
                                       "/opt/homebrew/sbin",
                                       "/usr/local/bin",
                                       "/usr/local/sbin",
                                       "/usr/local/zfs/bin",
                                       "/usr/sbin",
                                       "/sbin",
                                       "/usr/bin",
                                       "/bin"};

// Package managers reported alongside the commands, so a refresh needs one round
// trip instead of one per manager. Order and spelling must match what the GUI
// expects (it maps apt-get -> apt itself).
const char* const kProbedPackageManagers[] = {"apt-get", "pacman", "zypper", "brew", "pkg"};

bool isExecutableFile(const std::string& path);
bool lookupExecutable(const std::string& name);
ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64);
#endif
ExecResult runDumpZpoolGuidStatusBatchCapture();
std::string gsaClaveDePeriodo(const std::string& clase, std::time_t cuando);
bool gsaInstanteDeNombre(const std::string& snap, const std::string& clase, std::time_t& out);
ExecResult runDumpGsaRawAllPoolsCapture();
using KVMap = std::unordered_map<std::string, std::string>;

bool gsaBoolOn(const std::string& s);
int gsaIntVal(const std::string& s);
std::string gsaLocalTimestamp();
bool rutaDentroDeUnMountpoint(const std::string& ruta);
ExecResult listaDirectorio(const std::string& ruta);
struct MontajeZfs {
    std::string dataset;
    std::filesystem::path ruta;  // canonizada y sin separador final
};

std::filesystem::path canonizaSinBarra(std::string ruta);
std::vector<MontajeZfs> montajesZfs();
char letraDeTipo(const std::filesystem::file_status& st);
zfsmgr::base::json::Value entradaDeRuta(const std::filesystem::path& p);
ExecResult listaRuta(const std::string& ruta);
// `--dump-path-info <ruta>`: qué es una ruta y a qué dataset pertenece.
//
// El cliente lo usa para dos cosas: decir qué hay ahí (`info`, y si `cd` puede entrar), y
// la EQUIVALENCIA con un dataset. «exact_mountpoint» es lo único que permite tratar la
// ruta como el dataset; estar solo DENTRO de él no basta, y el cliente nunca sube al
// contenedor —`destroy` de un subdirectorio no puede acabar destruyendo el dataset—.
// --- Montaje TEMPORAL de solo lectura, para mirar dentro de un dataset sin montar.
//
// `--mutate-mount-ro-temp <dataset>` lo monta en solo lectura en /tmp/zfsmgr-ro-XXXXXX
// SIN TOCAR NINGUNA PROPIEDAD, y `--mutate-unmount-ro-temp <dir>` lo desmonta y borra el
// directorio. Es lo que hace zfsbootmenu con un entorno de arranque: `mount -t zfs` sobre un
// directorio cualquiera. Un reinicio lo olvida sin dejar nada que reparar, al contrario que
// cambiar el `mountpoint`, que es lo que hace AltMountGuard para escribir.
//
// El caso que lo pidió: un sistema arrancable con `mountpoint=/` y `canmount=noauto`. Montarlo
// «en su sitio» sería encima de la raíz de la máquina en marcha; aquí nunca se monta ahí.
//
// Medido: en Linux hace falta `-o zfsutil` (si no, mount.zfs exige mountpoint=legacy); en
// FreeBSD basta `mount -t zfs`. En los dos `zfs mount` lo enseña en el temporal y la
// propiedad `mountpoint` queda como estaba. En macOS no se ofrece: ver abajo.
static const char* const kPrefijoMontajeRo = "/tmp/zfsmgr-ro-";

ExecResult montaSoloLecturaTemporal(const std::string& dataset);
ExecResult desmontaSoloLecturaTemporal(const std::string& directorio);
ExecResult describeRuta(const std::string& ruta);
ExecResult leeFichero(const std::string& ruta, std::uintmax_t desde, std::uintmax_t cuanto);
std::vector<std::string> gsaDueClasses(const std::string& ds, int hourly, int daily, int weekly,
                                       int monthly, int yearly, std::time_t ahora);
std::string gsaPropValue(const std::string& ds, const std::string& prop, bool localOnly);
// Lo que deja cada paso del planificador: si fue bien, y si no, por qué. Va al registro del
// daemon con su nivel y, con el mismo texto, al estado (`gsa-state.json`), que es lo que
// lee el CLI para decir si las programaciones están en verde.
using GsaLog = std::function<void(const char* nivel, const std::string& linea)>;

ExecResult executeAgentCommandCapture(const std::string& cmd,
                                      const std::vector<std::string>& params, const char* argv0);
std::string generateJobId();
struct GsaPaso {
    bool hecho{false};  // se intentó algo
    bool ok{true};
    std::string error;   // la frase en inglés: el registro y los clientes viejos
    std::string codigo;  // y el código, para que el CLI la diga en su idioma (agentmsg)
    std::vector<std::string> args;

    void falla(const std::string& c, std::vector<std::string> a) {
        ok = false;
        codigo = c;
        args = std::move(a);
        error = zfsmgr::base::agentmsg::inEnglish(codigo, args);
    }
};

// El último fallo de una petición a un par, como código, en ESTE hilo. Lo deja `gsaPeerRpc`
// junto a su texto; así quien lo recibe puede guardarlo con código sin que la función tenga
// que devolver una cosa más.
struct GsaFallo {
    std::string codigo;
    std::vector<std::string> args;
};

extern thread_local GsaFallo t_gsaFalloPar;
bool gsaSnapshotsDe(const std::string& ds, std::vector<zfsmgr::base::gsa::SnapRef>& out,
                    std::string& error);
std::vector<std::string> gsaNombres(const std::vector<zfsmgr::base::gsa::SnapRef>& s);
bool gsaSinCambiosDesde(const std::string& ds, const std::string& previo, bool recursive);
GsaPaso gsaHazSnapshot(const std::string& ds, const std::string& klass, bool recursive,
                       bool permitirRenombrar, std::string& nombre, const GsaLog& log);
GsaPaso gsaPoda(const std::string& ds, const std::string& klass, int keep, bool recursive,
                const std::string& guidProtegido, int& borrados, bool& baseConservada,
                const GsaLog& log);
#ifdef _WIN32
const char* const kPeersPath = "C:\\ProgramData\\ZFSMgr\\agent\\peers.json";

#else
const char* const kPeersPath = "/etc/zfsmgr/peers.json";

#endif
// Definido más abajo, junto al resto del transporte: aquí solo se anuncia para poder
// usarlo desde el nivelado sin mover cuatrocientas líneas de sitio.
using TransferProgressFn =
    std::function<bool(std::uint64_t totalBytes, long elapsedSecs, double rateMiBs)>;

ExecResult runZfsSendToPeerCapture(const std::vector<std::string>& params,
                                   const TransferProgressFn& onProgress,
                                   const std::string& ficheroOrigen = std::string());
struct GsaPeer {
    std::string id;
    std::string host;
    int port{0};
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
};

bool gsaDestinoSoyYo(const std::string& connId, const std::string& selfPorNombre);
std::string gsaYoMismo();
bool gsaCargaPeer(const std::string& id, GsaPeer& out);
// Una petición al daemon de un par, por el mismo protocolo que usa la aplicación:
// `{"cmd":…,"args":[…]}` y respuesta con `rc`, `stdout` y `stderr`.
//
// Distingue NO LLEGAR de que el par conteste que no. Antes las dos cosas eran `false`, y la
// nivelación tomaba «el par está apagado» por «el dataset no existe»: intentaba un envío
// completo y el error que quedaba era el de la escucha, no el de verdad.
enum class GsaRpc { Ok, NoLlega, Rechaza };

GsaRpc gsaPeerRpc(const GsaPeer& peer, const std::string& cmd, const std::vector<std::string>& args,
                  std::string& salida, std::string& error);
#ifndef _WIN32
ExecResult ejecutaTuberiaZfsLocalConAvance(const std::vector<std::string>& sendArgv,
                                           const std::vector<std::string>& recvArgv,
                                           const TransferProgressFn& avance);
#else
ExecResult ejecutaTuberiaZfsLocalConAvance(const std::vector<std::string>&,
                                           const std::vector<std::string>&,
                                           const TransferProgressFn&);
#endif
// Lo que queda del «destino» después de quitar el SSH: si es esta misma máquina.
struct GsaTargetConn {
    std::string mode;  // «local» si el destino es esta máquina
};

bool gsaParado();
bool enDestinoAborta(bool isLocal, const GsaPeer& peer, const std::string& dataset,
                     std::string& error);
ExecResult gsaEnviaFlujo(bool isLocal, const GsaPeer& peer, const std::string& conexion,
                         const std::vector<std::string>& sendArgv, const std::string& recvEn,
                         bool incremental, const std::string& testigoReanudar,
                         const TransferProgressFn& avance);
struct GsaNivelado {
    GsaPaso paso;         // la nivelación en sí
    GsaPaso podaDestino;  // la poda del destino
    int borradosEnDestino{0};
    std::string enviado;   // el snapshot del origen hasta el que se llegó
    std::string guidBase;  // y su guid: la base de la próxima vez, la que la poda protege
};

GsaNivelado gsaNivela(const std::string& ds, bool recursive, const std::string& dstSpec,
                      const GsaTargetConn& target, const TransferProgressFn& avance,
                      const GsaLog& log);
#ifdef _WIN32
const char* const kGsaStatePath = "C:\\ProgramData\\ZFSMgr\\agent\\gsa-state.json";

#else
const char* const kGsaStatePath = "/var/lib/zfsmgr/gsa-state.json";

#endif
extern std::mutex g_gsaPasada;
extern std::mutex g_gsaEstado;
zfsmgr::base::json::Value gsaLeeEstadoSinBloqueo();
void gsaGuardaEstadoSinBloqueo(const zfsmgr::base::json::Value& raiz);
zfsmgr::base::json::Value gsaEntradaDe(const std::string& ds);
void gsaActualizaEntrada(const std::string& ds,
                         const std::function<void(zfsmgr::base::json::Value&)>& cambia);
void gsaPonError(zfsmgr::base::json::Value& v, const std::string& codigo,
                 const std::vector<std::string>& args);
zfsmgr::base::json::Value gsaRegistroDe(const GsaPaso& p, long long cuando);
struct GsaProgramacion {
    bool recursive{false};
    int hourly{0}, daily{0}, weekly{0}, monthly{0}, yearly{0};
    bool levelOn{false};
    std::string dstSpec;
    std::string origin;  // a dataset that PULLS from there (gsa::Schedule::origin)
};

GsaProgramacion gsaLeeProgramacion(const std::string& ds);
// `--peer-check <connection>` and `--dump-gsa-pullers <dataset>` (see gsa.cpp).
ExecResult runPeerCheckCapture(const std::string& id);
ExecResult runGsaPullersCapture(const std::string& ds);
// PULL into `ds` from its origin: every dataset of the origin's tree with GSA snapshots, one
// by one and without -R, holding the base on the source, then pruning here with this
// dataset's own retentions. `soloMirar` plans without sending: `paso.hecho` says whether
// anything is due.
GsaNivelado gsaTrae(const std::string& ds, const GsaProgramacion& p, bool soloMirar,
                    const TransferProgressFn& avance, const GsaLog& log);
// Seconds between two passes of the scheduler.
constexpr long long kGsaPasadaSegs = 300;
std::string gsaAncestroRecursivo(const std::string& ds);
GsaLog gsaLogDelDaemon();
extern std::mutex g_gsaNivMutex;
extern std::condition_variable g_gsaNivCv;
extern int g_gsaNivEnMarcha;
constexpr int kGsaNivMax = 2;

extern std::map<std::string, std::string> g_gsaNivPorDataset;
bool gsaNivelandose(const std::string& ds);
bool gsaTrabajoCancelado(const std::string& id);
std::string gsaLanzaNivelacion(const std::string& ds, bool recursive, const std::string& dstSpec,
                               const GsaTargetConn& target, const GsaLog& log);
std::string gsaProcesaDataset(const std::string& ds, const std::string& selfConn, bool forzado,
                              const GsaLog& log);
void gsaRunOnce(const GsaLog& log);
ExecResult runGsaRunOnceCapture(const std::string& ds);
ExecResult runDumpGsaStatusCapture();
void gsaCierraNivelacionesHuerfanas();
void runGsaSchedulerThread();
std::string generateTransferToken();
std::string generateJobId();
#ifdef _WIN32
void ensureWinsock();
#endif
#ifdef _WIN32
using TransferSocket = SOCKET;

constexpr TransferSocket kSocketInvalido = INVALID_SOCKET;

#else
using TransferSocket = int;

// En POSIX no existe `INVALID_SOCKET`: el valor malo es -1. Tener la constante evita el
// `#ifdef` repetido en cada sitio que comprueba si un socket vale.
constexpr TransferSocket kSocketInvalido = -1;

#endif
void closeTransferSocket(TransferSocket s);
void cierraConRechazo(TransferSocket s);
void informaAlEmisor(TransferSocket s, int rc, const std::string& motivo);
// Lo que contestó el receptor, si contestó. `hayVeredicto` falso quiere decir que cerró sin
// decir nada o que no se pudo leer.
struct VeredictoDelReceptor {
    bool hayVeredicto{false};
    int rc{0};
    std::string motivo;
};

VeredictoDelReceptor leeVeredicto(TransferSocket s, const std::function<bool()>& sigue,
                                  std::chrono::milliseconds plazo);
VeredictoDelReceptor esperaVeredicto(TransferSocket s, const std::function<bool()>& sigue);
void noHeredarTransferSocket(TransferSocket fd);
TransferSocket creaTransferSocket(int family, int type, int proto);
TransferSocket aceptaTransferSocket(TransferSocket listenFd);
// Cuánto se espera a que el receptor lea antes de recuperar el control. No es un abandono:
// al vencer se comprueba la cancelación y se reintenta el mismo envío.
static const int kEsperaEnvioMs = 5000;
// How long a connection to the other daemon may take to open.
static const int kConexionMs = 10000;

// Cuánto se tolera SIN colocar un solo byte antes de dar la transfer por muerta.
// Generoso a propósito: un receptor lento volcando a disco puede tardar, y cortar una
// transfer buena es peor que tardar en cortar una mala.
static const long kAtascoMaximoS = 600;

void ponPlazoDeEnvio(TransferSocket fd, int ms);
bool envioSoloAgotoElPlazo();
TransferSocket conectaConElPar(const std::string& host, const std::string& puerto);
// Lanza un programa y deja su SALIDA colgando de una tubería que lee el llamante.
//
// Existe para que el relé de --zfs-send-to-peer sea UNO y no dos: entre plataformas
// cambia cómo se crea el proceso y cómo se lee, no la lógica de mover los bytes al otro
// extremo ni la de informar del avance, que es donde estarían los errores de verdad.
//
// El error del hijo va a una tubería APARTE, nunca a la de datos. Mezclarlos metería el
// texto de un fallo de `zfs send` dentro del flujo, y el receptor lo recibiría como si
// fueran datos: corrupción silenciosa en lugar de un error visible.
struct SpawnedStdout {
#ifdef _WIN32
    HANDLE readEnd{nullptr};
    HANDLE process{nullptr};
    std::shared_ptr<std::string> errText;
    std::shared_ptr<std::thread> errThread;
#else
    int readEnd{-1};
    pid_t pid{-1};
#endif
    bool ok{false};
};

SpawnedStdout spawnReadingStdout(const std::string& program, const std::vector<std::string>& args);
// The argv of `zfs send` (without «zfs»): `-t <token>` alone to resume, otherwise the flags,
// `-I <base>` if there is one, and the snapshot. The flags must already be checked
// (zfsprops::areValidSendFlags): this runs with privileges.
std::vector<std::string> argvDeZfsSend(const std::string& snap, const std::string& baseSnap,
                                       const std::string& flagsStr, const std::string& resumeToken);
// `--zfs-send-stream`: the answer is the stream itself, on the same TLS connection the caller
// opened. After a first JSON line ({"rc":0,"stream":1}, or the usual error answer), frames of a
// 4-byte big-endian length and that many bytes; a frame of length 0 ends it, and one JSON line
// follows with `zfs send`'s result ({"rc":N,"stderr":"…"}). It is how a machine PULLS a copy:
// it opens the connection, so it needs no open port and nobody has to know its address.
constexpr std::size_t kTramoFlujoMax = 256 * 1024;
// The puller's side: asks `peer` for `--zfs-send-stream <snap> <base> <flags> [<resume>]` and
// feeds it to a local `zfs <recvArgv>`, with progress and cancellation (`avance` returning
// false stops it; the receive, with -s, keeps a resume token).
ExecResult tiraFlujoDelPar(const GsaPeer& peer, const std::vector<std::string>& sendArgs,
                           const std::vector<std::string>& recvArgv,
                           const TransferProgressFn& avance);
bool rutaDeFlujoValida(const std::string& ruta, std::string& err);
SpawnedStdout abreFicheroComoFlujo(const std::string& ruta, std::string& err);
long readSpawnedStdout(SpawnedStdout& s, char* buf, std::size_t len);
void matarSpawnedStdout(SpawnedStdout& s);
int waitSpawnedStdout(SpawnedStdout& s, std::string* errOut = nullptr);
void runTransferReceiveSession(TransferSocket listenFd, const std::string& token,
                               const std::string& dataset, bool force,
                               bool sinProgramacion = false);
bool abrePuertoDeTransferencia(TransferSocket& listenFd, int& puerto, std::string& error);
// ---------------------------------------------------------------------------
// Sincronizar un ÁRBOL DE FICHEROS entre dos daemons.
//
// Mismo transporte que las transferencias de ZFS —el destino abre un puerto y devuelve un
// testigo de un solo uso, el origen se conecta— pero por el cable no va un flujo de
// `zfs send`, va un árbol. Eso es lo que le da a Windows una sincronización de verdad, con
// borrado y con pasada en seco, que por tar no tenía.
//
// El reparto: el DESTINO manda primero su manifiesto —lo que ya tiene— y el ORIGEN decide.
// Es una sola vuelta de red para todo el árbol, en vez de preguntar fichero a fichero.
// ---------------------------------------------------------------------------
namespace AR = zfsmgr::remotetree;

bool mandaTodo(TransferSocket s, const char* datos, std::size_t cuantos);
bool mandaTodo(TransferSocket s, const std::string& t);
bool leeExacto(TransferSocket s, char* destino, std::size_t cuantos);
bool leeLinea(TransferSocket s, std::string& salida, std::size_t tope = 4096);
bool leeManifiesto(TransferSocket s, std::vector<AR::Entry>& salida, std::string& error);
void runTreeReceiveSession(TransferSocket listenFd, const std::string& token,
                           const std::string& raiz);
ExecResult runTreeRecvListenCapture(const std::string& raiz, bool waitInline = false);
ExecResult runTreeSendToPeerCapture(const std::vector<std::string>& params);
// `sinProgramacion`: receive with `-x` for every schedule property (a levelling; see
// gsa::scheduleProperties).
ExecResult runZfsRecvListenCapture(const std::string& dataset, bool force, bool waitInline = false,
                                   bool sinProgramacion = false);
void runFileReceiveSession(TransferSocket listenFd, const std::string& token,
                           const std::string& ruta, const zfsmgr::compresion::Eleccion& comp);
ExecResult runFileRecvListenCapture(const std::string& ruta, bool sobrescribir,
                                    bool waitInline = false,
                                    const std::string& compTexto = std::string());
ExecResult runZfsSendToPeerCapture(const std::vector<std::string>& params,
                                   const TransferProgressFn& onProgress = nullptr,
                                   const std::string& ficheroOrigen);
ExecResult runDumpBlockDevicesCapture();
void runZfsSendToPeerAsync(const std::string& jobId);
std::string resolveDeviceName(const std::string& nombre);
ExecResult runZpoolLabelclear(const std::vector<std::string>& params);
// ── La tabla de verbos ──────────────────────────────────────────────────────────────────
//
// UNA sola tabla para las dos puertas del agente: el RPC (`executeAgentCommandCapture`) y la
// línea de órdenes (`main`). Antes cada puerta tenía su cadena de `if (cmd == …)` con su propia
// copia de cada verbo, y se separaban: `--mutate-zfs-rename` admitía `r` (rename -r) por RPC y
// no por la otra puerta. Ahora un verbo se escribe una vez, aquí, y las dos lo sirven igual.
//
// `soloRpc`: verbos que llevan un SECRETO en sus parámetros (la frase de una clave). Por RPC
// viajan cifrados por mTLS; por la línea de órdenes irían en argv, a la vista de `ps`, así que
// esa puerta los rechaza.
struct Peticion {
    const std::string& cmd;
    const std::vector<std::string>& params;
    const char* argv0;
};

using Manejador = std::function<ExecResult(const Peticion&)>;

struct Verbo {
    Manejador haz;
    bool soloRpc{false};
};

class TablaDeVerbos {
public:
    void pon(std::initializer_list<const char*> nombres, Manejador haz) {
        for (const char* n : nombres) {
            verbos_[n] = Verbo{haz, false};
        }
    }
    void soloPorRpc(const std::string& nombre) { verbos_.at(nombre).soloRpc = true; }
    const Verbo* busca(const std::string& nombre) const {
        const auto it = verbos_.find(nombre);
        return it == verbos_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, Verbo> verbos_;
};

const TablaDeVerbos& tablaDeVerbos();
ExecResult executeAgentCommandCapture(const std::string& cmd,
                                      const std::vector<std::string>& params, const char* argv0);
std::string jsonEscape(const std::string& s);
std::string argsJson(const std::vector<std::string>& args);
std::string jsonEscape(const std::string& s);
const char* jobStateName(JobState s);
JobState jobStateFromName(const std::string& s);
void persistJobsLocked();
void loadPersistedJobsAtStartup();
bool parseJsonStringAt(const std::string& s, std::size_t& i, std::string& out);
bool parseJsonRpcRequest(const std::string& line, std::string& cmd, std::vector<std::string>& args);
bool isDumpCommand(const std::string& cmd);
bool isMutateCommand(const std::string& cmd);
std::string makeRpcCacheKey(const std::string& cmd, const std::vector<std::string>& args);
std::string dumpClassForCommand(const std::string& cmd);
std::string describeJob(const std::string& cmd, const std::vector<std::string>& params);
bool isAsyncSubmittableCommand(const std::string& cmd);
void runSubmittedMutationJob(const std::string& jobId, const std::string& cmd,
                             const std::vector<std::string>& params);
#ifndef _WIN32
extern std::atomic<pid_t> g_zedGrupo;
FILE* abreZpoolEvents(pid_t& grupo);
void cierraZpoolEvents(FILE* fp, pid_t grupo);
#endif
int runServeLoop();
void printUsage(const char* argv0);

}  // namespace zfsmgr::agent
