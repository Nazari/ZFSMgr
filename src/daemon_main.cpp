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
#include <mutex>
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
typedef unsigned int  uint_t;
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
#include <unistd.h>
#include <limits.h>
#else
#include <io.h>
#endif

namespace {

#ifdef _WIN32
using NativeSock = SOCKET;
constexpr NativeSock kInvalidSock = INVALID_SOCKET;
inline void closeSock(NativeSock s) { closesocket(s); }
#else
using NativeSock = int;
constexpr NativeSock kInvalidSock = -1;
inline void closeSock(NativeSock s) { close(s); }
#endif

#ifdef _WIN32
// En Windows no hay /tmp: el ofstream fallaba y writeHeartbeat retornaba en silencio,
// así que el latido nunca se registraba.
constexpr const char* kHeartbeatPath = "C:\\ProgramData\\ZFSMgr\\agent\\heartbeat.log";
#else
constexpr const char* kHeartbeatPath = "/tmp/zfsmgr-agent-heartbeat.log";
#endif
// En Windows no existe /var/lib: el daemon no tenía dónde escribir y la pestaña
// Daemon salía vacía, sin decir por qué (el ofstream fallaba y se retornaba en
// silencio). El comando --dump-daemon-log sí estaba disponible; lo que faltaba era
// el fichero.
#ifdef _WIN32
constexpr const char* kDaemonLogFile = "C:\\ProgramData\\ZFSMgr\\agent\\daemon.log";
#else
constexpr const char* kDaemonLogFile = "/var/lib/zfsmgr/daemon.log";
#endif
constexpr long long kDaemonLogMaxBytes = 2 * 1048576LL;
constexpr const char* kJobsFilePath = "/var/lib/zfsmgr/jobs.json";
// Rutas por defecto. En Windows no existe /etc, así que apuntar ahí dejaba al agente
// sin configuración ni certificados salvo que se le pasara ZFSMGR_AGENT_CONFIG a mano
// — comprobado en un Windows 11 real, donde --serve moría en silencio por eso.
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
const std::string kDefaultTlsClientCertPath_s = std::string(kDefaultTlsDir) + kPathSep + "client.crt";
const char* const kDefaultTlsClientCertPath = kDefaultTlsClientCertPath_s.c_str();
// En Windows el separador es ';' y las rutas Unix no significan nada: con el valor
// de abajo el agente arrancaba y servía TLS, pero NINGÚN comando se encontraba.
#ifdef _WIN32
constexpr const char* kDefaultCommandPath =
    "C:\\Program Files\\OpenZFS On Windows;C:\\Windows\\System32;C:\\Windows";
#else
constexpr const char* kDefaultCommandPath =
    "/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin";
#endif

constexpr const char* kApiVersion = "3";
#ifndef ZFSMGR_AGENT_VERSION_STRING
#define ZFSMGR_AGENT_VERSION_STRING ZFSMGR_APP_VERSION
#endif

std::atomic<bool> g_stop{false};
std::mutex g_daemonLogMutex;

void daemonLog(const std::string& level, const std::string& msg); // forward decl

// ── Background job registry ──────────────────────────────────────────────────

enum class JobState { Queued, Running, Done, Failed, Cancelled };

struct DaemonJob {
    std::string id;
    std::string type;          // "send-to-peer" | "pipe-local"
    std::string snap;
    std::string peerHost;
    int         peerPort{0};
    std::string token;
    std::string baseSnap;
    std::string sendFlags;
    std::string pipeCmd;       // base64-encoded shell command (pipe-local only)
    std::string dstDataset;    // destination dataset (pipe-local only)
    std::string submittedCmd;  // agent command being run (type "mutation")
    std::string resultOut;     // captured stdout of a "mutation" job
    int         resultRc{0};   // exit code of a "mutation" job

    JobState    state{JobState::Queued};
    pid_t       sendPid{-1};
    uint64_t    bytesTransferred{0};
    double      rateMiBs{0.0};
    long        elapsedSecs{0};
    std::string startedAtUtc;
    std::string finishedAtUtc;
    std::string errorText;
    std::vector<std::string> progressLines; // ring buffer, max 5
};

std::mutex g_jobsMutex;
std::unordered_map<std::string, DaemonJob> g_jobs;

// Sumidero de progreso del trabajo que corre EN ESTE HILO, si lo hay.
//
// Desglosar, Ensamblar y Hacia Dir se ejecutan como un trabajo y devuelven al
// terminar, así que no tenían forma de informar de nada por el camino: catorce
// minutos de operación destructiva sin una sola señal de vida en la caja de
// progreso. El contador de bytes que sí muestran las transferencias vive en su
// camino de tuberías y no pasa por aquí.
//
// Es por elemento y no por byte a propósito: rsync habría que transmitirlo en
// vivo, y "5 de 13, Tools/BackupRecovery" ya responde a lo que se pregunta el
// usuario mirando la pantalla.
thread_local std::string t_currentJobId;

void reportJobProgress(const std::string& line) {
    if (t_currentJobId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(t_currentJobId);
    if (it == g_jobs.end()) {
        return;
    }
    it->second.progressLines.push_back(line);
    if (it->second.progressLines.size() > 5) {
        it->second.progressLines.erase(it->second.progressLines.begin());
    }
}

// Forward declarations — defined after jsonEscape
static void persistJobsLocked();
static void loadPersistedJobsAtStartup();
static void runZfsSendToPeerAsync(const std::string& jobId);

struct ExecResult {
    int rc{1};
    std::string out;
    std::string err;
};

ExecResult runExecCapture(const std::string& program, const std::vector<std::string>& args);

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

std::string trim(const std::string& s) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t a = 0;
    while (a < s.size() && isSpace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    std::size_t b = s.size();
    while (b > a && isSpace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
    if (!s.empty() && s.back() == '\n') {
        // std::getline descarta última línea vacía; no hace falta conservarla aquí.
    }
    return lines;
}

std::string firstLineTrimmed(const std::string& raw) {
    const std::size_t nl = raw.find('\n');
    return trim(nl == std::string::npos ? raw : raw.substr(0, nl));
}

bool isTruthyValue(const std::string& raw) {
    const std::string v = toLower(trim(raw));
    return v == "1" || v == "on" || v == "yes" || v == "true";
}

ExecResult getZfsPropertyCapture(const std::string& dataset, const std::string& prop) {
    ExecResult r = runExecCapture("zfs", {"get", "-H", "-o", "value", prop, dataset});
    if (r.rc != 0) {
        return r;
    }
    r.out = firstLineTrimmed(r.out);
    return r;
}

ExecResult getDatasetMountpointCapture(const std::string& dataset) {
    ExecResult mounted = getZfsPropertyCapture(dataset, "mounted");
    if (mounted.rc != 0) {
        return mounted;
    }
    if (!isTruthyValue(mounted.out)) {
        ExecResult ok;
        ok.rc = 0;
        return ok;
    }
    ExecResult mp = getZfsPropertyCapture(dataset, "mountpoint");
    if (mp.rc != 0) {
        return mp;
    }
    const std::string mountpoint = trim(mp.out);
    if (mountpoint.empty()
        || mountpoint == "-"
        || mountpoint == "none"
        || mountpoint == "legacy") {
        ExecResult ok;
        ok.rc = 0;
        return ok;
    }
    ExecResult ok;
    ok.rc = 0;
    ok.out = mountpoint;
    return ok;
}

// Verbos que este binario sirve de verdad, para que el cliente no tenga que adivinarlo.
//
// La alternativa era una tabla escrita en la GUI, y se desincroniza en cuanto se porta
// cualquier verbo: quien lo porta toca este fichero, no el cliente. Se calcula con los
// mismos #ifdef que deciden qué se compila, así que no puede mentir.
std::string agentCapabilityList() {
    std::vector<std::string> caps = {
        "--dump-zfs-allow",
    };
    caps.push_back("--dump-tool-availability");
#ifndef _WIN32
    caps.push_back("--job-submit");
    caps.push_back("--repair-alt-mountpoints");
    caps.push_back("--mutate-advanced-todir");
    caps.push_back("--mutate-rsync-local");
    caps.push_back("--zfs-send-to-peer");
#endif
    // Estas dos compilan en ambos sistemas, pero en Windows dependían de makeTempDir,
    // que devolvía cadena vacía y las hacía fallar siempre con rc=125. Se declaran
    // porque esa parte ya está resuelta.
    caps.push_back("--mutate-advanced-breakdown");
    caps.push_back("--mutate-advanced-assemble");
    std::string out;
    for (size_t i = 0; i < caps.size(); ++i) {
        if (i) out += ",";
        out += caps[i];
    }
    return out;
}

// Crea un directorio temporal DENTRO de un directorio dado, en vez de en el temporal
// del sistema.
//
// Existe porque Ensamblar hacía su escala en /tmp, que en la mayoría de las
// distribuciones es tmpfs, o sea RAM: mover 27 GB reventó con
// "No space left on device" tras llenar los 9,5 GB de /tmp. Y aunque cupiera, estaría
// copiando los datos fuera del pool para devolverlos después.
//
// Dentro del dataset padre el espacio que cuenta es el del pool, la copia no cruza
// sistemas de ficheros, y el paso final puede ser un renombrado en vez de una segunda
// copia entera.
std::string makeTempDirIn(const std::string& baseDir, const char* patternPrefix) {
#ifndef _WIN32
    char templ[PATH_MAX];
    if (std::snprintf(templ, sizeof(templ), "%s/%sXXXXXX", baseDir.c_str(), patternPrefix)
        >= static_cast<int>(sizeof(templ))) {
        return {};
    }
    char* out = ::mkdtemp(templ);
    return out ? std::string(out) : std::string();
#else
    GUID guid;
    if (CoCreateGuid(&guid) != S_OK) {
        return {};
    }
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "%08lX%04X%04X",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3);
    const std::string dir = baseDir + "\\" + patternPrefix + suffix;
    if (!CreateDirectoryA(dir.c_str(), nullptr)) {
        return {};
    }
    return dir;
#endif
}

std::string makeTempDir(const char* patternPrefix) {
#ifndef _WIN32
    char templ[PATH_MAX];
    std::snprintf(templ, sizeof(templ), "/tmp/%sXXXXXX", patternPrefix);
    char* out = ::mkdtemp(templ);
    if (!out) {
        return {};
    }
    return std::string(out);
#else
    // Devolvía cadena vacía, y sus llamantes no lo comprobaban: --mutate-advanced-breakdown
    // y --mutate-advanced-assemble figuraban disponibles y fallaban SIEMPRE con
    // rc=125 "mkdtemp failed", a mitad de una operación destructiva.
    char tempRoot[MAX_PATH];
    const DWORD n = GetTempPathA(sizeof(tempRoot), tempRoot);
    if (n == 0 || n > sizeof(tempRoot)) {
        return {};
    }
    // GUID en vez de un contador: el daemon atiende varias conexiones a la vez y dos
    // operaciones simultáneas no pueden compartir directorio temporal.
    GUID guid;
    if (CoCreateGuid(&guid) != S_OK) {
        return {};
    }
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "%08lX%04X%04X",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3);
    const std::string dir = std::string(tempRoot) + patternPrefix + suffix;
    if (!CreateDirectoryA(dir.c_str(), nullptr)) {
        return {};
    }
    return dir;
#endif
}

// Banderas de preservación de rsync, sondeadas UNA vez: son propiedad del binario
// local, no de cada operación.
//
// El shell antiguo hacía este sondeo antes de cada copia. Al pasar a nativo se
// perdió aquí y runRsyncCopyMoveCapture se quedó con "-aHWS" a secas, de modo que
// todir, assemble y breakdown dejaron de preservar ACLs y atributos extendidos sin
// avisar de nada. Copiar de menos en silencio es la peor forma de perder datos: el
// usuario cree que tiene lo mismo que antes.
const std::vector<std::string>& rsyncPreservationFlags() {
    static const std::vector<std::string> flags = [] {
        std::vector<std::string> out;
        const ExecResult help = runExecCapture("rsync", {"--help"});
        const std::string helpText = help.out + help.err;
        if (runExecCapture("rsync", {"-A", "--version"}).rc == 0) {
            out.push_back("-A");
        }
        if (runExecCapture("rsync", {"-X", "--version"}).rc == 0) {
            out.push_back("-X");
        } else if (helpText.find("--extended-attributes") != std::string::npos) {
            out.push_back("--extended-attributes");  // el rsync de Apple lo llama así
        }
        return out;
    }();
    return flags;
}

ExecResult runRsyncCopyMoveCapture(const std::string& srcDir, const std::string& dstDir,
                                   const std::vector<std::string>& excludes = {}) {
    std::vector<std::string> argv{"-aHWS"};
    const std::vector<std::string>& extra = rsyncPreservationFlags();
    argv.insert(argv.end(), extra.begin(), extra.end());
    // Anclados con "/" inicial: sin eso, excluir "Tools" se comería CUALQUIER
    // directorio llamado así en todo el subárbol, no solo el de la raíz de la copia.
    for (const std::string& ex : excludes) {
        argv.push_back("--exclude=/" + ex + "/");
    }
    argv.push_back(srcDir + "/");
    argv.push_back(dstDir + "/");
    return runExecCapture("rsync", argv);
}

ExecResult runDumpAdvancedBreakdownListCapture(const std::string& dataset) {
    ExecResult mpRes = getDatasetMountpointCapture(dataset);
    if (mpRes.rc != 0) {
        return mpRes;
    }
    const std::string mountpoint = trim(mpRes.out);
    ExecResult r;
    r.rc = 0;
    if (mountpoint.empty()) {
        return r;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root(mountpoint);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return r;
    }

    std::vector<std::string> dirs;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& e = *it;
        std::error_code sec;
        const fs::file_status st = e.symlink_status(sec);
        if (!sec && fs::is_symlink(st)) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }
        if (!sec && fs::is_directory(st)) {
            std::error_code rec;
            fs::path rel = fs::relative(e.path(), root, rec);
            if (!rec && !rel.empty()) {
                bool skip = false;
                for (const auto& c : rel) {
                    if (c == ".zfs") {
                        skip = true;
                        break;
                    }
                }
                if (skip) {
                    it.disable_recursion_pending();
                } else {
                    const std::string relPath = rel.generic_string();
                    if (!relPath.empty() && relPath != ".") {
                        dirs.push_back(relPath);
                    }
                }
            }
        }
        ++it;
    }

    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    r.out = "__MP__=" + mountpoint + "\n";
    for (const std::string& d : dirs) {
        r.out += d;
        r.out.push_back('\n');
    }
    return r;
}

// Cuenta los ficheros que a rsync todavía le quedarían por transferir, en seco.
//
// Es la verificación que hacía el shell antiguo antes de borrar el origen y que se
// perdió al pasar a nativo: la versión intermedia borraba en cuanto rsync devolvía
// 0. Un rsync que termina bien pero deja algo atrás (disco lleno a mitad, un fichero
// que cambió durante la copia) convertía el desglose en pérdida de datos.
//
// El formato itemizado de "-i" pone las banderas en los 11 primeros caracteres;
// ">f" significa fichero que se transferiría. --ignore-existing hace que solo
// aparezca lo que falta, no lo que difiere.
int countPendingRsyncTransfers(const std::string& srcDir, const std::string& dstDir,
                               const std::vector<std::string>& excludes = {}) {
    // Las MISMAS exclusiones que la copia: si no, lo que se dejó fuera a propósito
    // contaría como pendiente y la verificación fallaría siempre.
    std::vector<std::string> argv{"-rni", "--ignore-existing"};
    for (const std::string& ex : excludes) {
        argv.push_back("--exclude=/" + ex + "/");
    }
    argv.push_back(srcDir + "/");
    argv.push_back(dstDir + "/");
    const ExecResult probe = runExecCapture("rsync", argv);
    if (probe.rc != 0) {
        return -1;  // no se pudo comprobar: tratar como "no verificado"
    }
    int pending = 0;
    for (const std::string& line : splitLines(probe.out)) {
        if (line.size() > 11 && line[0] == '>' && line[1] == 'f') {
            ++pending;
        }
    }
    return pending;
}

ExecResult runMutateAdvancedBreakdownCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 2) {
        r.rc = 2;
        r.err = "usage: --mutate-advanced-breakdown <dataset> <dir> [dir...]\n";
        return r;
    }
    const std::string dataset = params[0];
    ExecResult mpRes = getDatasetMountpointCapture(dataset);
    if (mpRes.rc != 0) {
        return mpRes;
    }
    const std::string mountpoint = trim(mpRes.out);
    if (mountpoint.empty()) {
        r.rc = 1;
        r.err = "mountpoint=none\n";
        return r;
    }
    namespace fs = std::filesystem;

    // Antes se convertía cada directorio de arriba abajo, y cada nivel copiaba TODO lo
    // que tenía debajo: un fichero a tres niveles de profundidad se movía tres veces.
    // Medido sobre 28 GB con 5 niveles: 14 minutos y 3,2 veces el volumen, con el pool
    // albergando a la vez el original y las copias intermedias.
    //
    // Ahora cada byte se mueve UNA vez:
    //   1. cada directorio se copia a un dataset con nombre TEMPORAL y plano, excluyendo
    //      los subdirectorios que también van a convertirse;
    //   2. solo cuando TODAS las copias están verificadas se borran los originales;
    //   3. y se renombran los datasets a su nombre definitivo, de arriba abajo.
    //
    // El nombre temporal plano es la pieza que lo hace posible: crear directamente
    // `<ds>/Disks/Bootables/Tools` obliga a `zfs create -p`, que fabrica los datasets
    // intermedios con puntos de montaje heredados que TAPAN los datos aún sin mover.
    //
    // Y borrar al final, en vez de sobre la marcha, hace la operación recuperable: si
    // algo falla a mitad, los originales siguen intactos.
    struct Pending {
        std::string rel;       // ruta relativa dentro del mountpoint
        std::string tmpName;   // dataset temporal, hijo directo de `dataset`
        std::string tmpMp;     // punto de montaje temporal
    };
    std::vector<Pending> pending;

    // Si algo falla, no dejar datasets temporales sueltos por ahí.
    struct TempCleanup {
        std::vector<Pending>* items;
        bool armed{true};
        ~TempCleanup() {
            if (!armed || !items) {
                return;
            }
            for (const Pending& p : *items) {
                (void)runExecCapture("zfs", {"destroy", "-r", p.tmpName});
                std::error_code ec;
                std::filesystem::remove_all(p.tmpMp, ec);
            }
        }
    } cleanup{&pending};

    // Los seleccionados, normalizados, para calcular exclusiones entre ellos.
    std::vector<std::string> rels;
    for (std::size_t i = 1; i < params.size(); ++i) {
        const std::string rel = trim(params[i]);
        if (!rel.empty()) {
            rels.push_back(rel);
        }
    }

    // ---- Fase 1: copiar cada directorio a su dataset temporal ----
    for (std::size_t i = 0; i < rels.size(); ++i) {
        const std::string& rel = rels[i];
        const fs::path srcPath = fs::path(mountpoint) / fs::path(rel);
        std::error_code ec;
        if (!fs::is_directory(srcPath, ec)) {
            r.out += "skip_not_a_directory=" + rel + "\n";
            continue;
        }
        const std::string child = dataset + "/" + rel;
        if (runExecCapture("zfs", {"list", "-H", "-o", "name", child}).rc == 0) {
            r.out += "child_exists=" + child + "\n";
            continue;
        }

        // Lo que NO hay que copiar aquí: los seleccionados que cuelgan de este.
        std::vector<std::string> excludes;
        for (const std::string& other : rels) {
            if (other.size() > rel.size() + 1 && other.compare(0, rel.size(), rel) == 0
                && other[rel.size()] == '/') {
                excludes.push_back(other.substr(rel.size() + 1));
            }
        }

        // ANTES de copiar, no solo al terminar: un directorio grande puede tardar
        // minutos, y avisando solo al acabar la caja de progreso se queda muda justo
        // cuando más falta hace. Medido: en un ensamblado de 27 G se emitió UNA línea,
        // al final, porque un elemento se llevó el 99 % del tiempo.
        reportJobProgress("[BREAKDOWN] copiando " + std::to_string(i + 1) + " de "
                          + std::to_string(rels.size()) + ": " + rel);
        const std::string tmpMp = makeTempDir("zfsmgr-breakdown-child-");
        if (tmpMp.empty()) {
            r.rc = 125;
            r.err = "no se pudo crear el punto de montaje temporal\n";
            return r;
        }
        // Nombre plano: hijo DIRECTO del dataset base, así no hay intermedios que crear.
        const std::string tmpName = dataset + "/" + fs::path(tmpMp).filename().string();
        ExecResult create = runExecCapture("zfs", {"create", "-o", "mountpoint=" + tmpMp, tmpName});
        if (create.rc != 0) {
            std::error_code rmec;
            fs::remove_all(tmpMp, rmec);
            return create;
        }
        (void)runExecCapture("zfs", {"mount", tmpName});
        pending.push_back(Pending{rel, tmpName, tmpMp});

        int left = -1;
        for (int attempt = 0; attempt < 5; ++attempt) {
            ExecResult rs = runRsyncCopyMoveCapture(srcPath.string(), tmpMp, excludes);
            if (rs.rc != 0) {
                return rs;
            }
            left = countPendingRsyncTransfers(srcPath.string(), tmpMp, excludes);
            if (left == 0) {
                break;
            }
        }
        if (left != 0) {
            r.rc = 42;
            r.err = "verify_failed=" + child + " pending="
                    + (left < 0 ? std::string("unknown") : std::to_string(left)) + "\n";
            return r;
        }
        // Con la marca [BREAKDOWN]: la interfaz FILTRA las líneas de progreso y solo
        // deja pasar las que la llevan. Sin ella se descartaban en silencio y la caja
        // de Progreso seguía vacía aunque el daemon las estuviera emitiendo.
        reportJobProgress("[BREAKDOWN] copiado " + std::to_string(i + 1) + " de "
                          + std::to_string(rels.size()) + ": " + rel);
    }

    // ---- Fase 2: todo verificado; ahora sí se borra y se renombra ----
    // Borrado de los originales primero, de más profundo a menos, para que el punto de
    // montaje definitivo esté libre cuando se renombre.
    std::vector<const Pending*> byDepth;
    for (const Pending& p : pending) {
        byDepth.push_back(&p);
    }
    const auto depthOf = [](const std::string& v) {
        return std::count(v.begin(), v.end(), '/');
    };
    std::sort(byDepth.begin(), byDepth.end(), [&](const Pending* a, const Pending* b) {
        return depthOf(a->rel) > depthOf(b->rel);
    });
    for (const Pending* p : byDepth) {
        const fs::path srcPath = fs::path(mountpoint) / fs::path(p->rel);
        std::error_code rmec;
        fs::remove_all(srcPath, rmec);
        if (rmec) {
            r.rc = 1;
            r.err = "remove_all failed for " + srcPath.string() + "\n";
            return r;
        }
    }

    // Renombrado de menos profundo a más: `zfs rename` exige que el padre exista.
    std::sort(byDepth.begin(), byDepth.end(), [&](const Pending* a, const Pending* b) {
        return depthOf(a->rel) < depthOf(b->rel);
    });
    for (const Pending* p : byDepth) {
        const std::string child = dataset + "/" + p->rel;
        const fs::path srcPath = fs::path(mountpoint) / fs::path(p->rel);
        ExecResult ren = runExecCapture("zfs", {"rename", p->tmpName, child});
        if (ren.rc != 0) {
            // A estas alturas los originales YA están borrados, así que los datasets
            // temporales son la única copia. Destruirlos sería perder los datos: se
            // desarma la limpieza y se dice dónde están.
            cleanup.armed = false;
            ren.err += "\nLos datos están a salvo en datasets temporales bajo " + dataset
                       + ". Renómbrelos a mano; NO los destruya.\n";
            for (const Pending* q : byDepth) {
                ren.err += "  " + q->tmpName + "  ->  " + dataset + "/" + q->rel + "\n";
            }
            return ren;
        }
        // El punto de montaje explícito NO viaja con el renombrado: sigue apuntando al
        // temporal, así que hay que fijarlo al definitivo.
        ExecResult setMp = runExecCapture("zfs", {"set", "mountpoint=" + srcPath.string(), child});
        if (setMp.rc != 0) {
            // Igual que arriba: el dataset ya tiene su nombre bueno y contiene los
            // únicos datos. Solo le falta el punto de montaje.
            cleanup.armed = false;
            setMp.err += "\nEl dataset " + child + " tiene los datos; falta fijarle "
                         "mountpoint=" + srcPath.string() + "\n";
            return setMp;
        }
        (void)runExecCapture("zfs", {"mount", child});
        std::error_code tmpEc;
        fs::remove_all(p->tmpMp, tmpEc);
        r.out += "[BREAKDOWN] ok " + p->rel + " -> " + child + "\n";
    }
    cleanup.armed = false;
    r.rc = 0;
    return r;
}

ExecResult runMutateAdvancedAssembleCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 2) {
        r.rc = 2;
        r.err = "usage: --mutate-advanced-assemble <dataset> <child> [child...]\n";
        return r;
    }
    const std::string dataset = params[0];
    ExecResult mpRes = getDatasetMountpointCapture(dataset);
    if (mpRes.rc != 0) {
        return mpRes;
    }
    const std::string parentMp = trim(mpRes.out);
    if (parentMp.empty()) {
        r.rc = 2;
        r.err = "mountpoint=none\n";
        return r;
    }
    namespace fs = std::filesystem;
    for (std::size_t i = 1; i < params.size(); ++i) {
        const std::string child = trim(params[i]);
        if (child.empty()) {
            continue;
        }
        // Puede que ya no exista: la lista que ofrece la interfaz incluye TODOS los
        // niveles, y ensamblar un hijo destruye recursivamente lo que cuelga de él. Si
        // se seleccionan padre e hijo, al llegar al hijo ya está absorbido. Antes eso
        // abortaba la operación entera con error, aunque el resultado fuera correcto.
        if (runExecCapture("zfs", {"list", "-H", "-o", "name", child}).rc != 0) {
            r.out += "[ASSEMBLE] ya absorbido " + child + "\n";
            continue;
        }
        (void)runExecCapture("zfs", {"mount", child});
        ExecResult cmpRes = getDatasetMountpointCapture(child);
        if (cmpRes.rc != 0) {
            return cmpRes;
        }
        const std::string childMp = trim(cmpRes.out);
        if (childMp.empty()) {
            continue;
        }
        const std::size_t pos = child.find_last_of('/');
        const std::string bn = (pos == std::string::npos) ? child : child.substr(pos + 1);
        if (bn.empty()) {
            continue;
        }
        // En el dataset PADRE, que es donde los datos tienen que acabar. Antes iba al
        // temporal del sistema: en Linux suele ser tmpfs, así que 27 GB de datos se
        // volcaban en RAM y la operación moría con "No space left on device".
        reportJobProgress("[ASSEMBLE] ensamblando " + std::to_string(i) + " de "
                          + std::to_string(params.size() - 1) + ": " + bn);
        const std::string tmp = makeTempDirIn(parentMp, ".zfsmgr-assemble-");
        if (tmp.empty()) {
            r.rc = 125;
            r.err = "no se pudo crear el directorio temporal en " + parentMp + "\n";
            return r;
        }
        ExecResult rsA = runRsyncCopyMoveCapture(childMp, tmp);
        if (rsA.rc != 0) {
            return rsA;
        }
        // Verificar ANTES de destruir. Esto NO es reponer nada: el shell antiguo
        // tampoco comprobaba, destruía en cuanto rsync devolvía 0. Aquí el riesgo es
        // mayor que en el desglose, porque "zfs destroy -r" no admite vuelta atrás:
        // si la copia quedó incompleta, los datos no están en ningún sitio.
        {
            int pending = countPendingRsyncTransfers(childMp, tmp);
            for (int attempt = 1; pending != 0 && attempt < 5; ++attempt) {
                ExecResult again = runRsyncCopyMoveCapture(childMp, tmp);
                if (again.rc != 0) {
                    return again;
                }
                pending = countPendingRsyncTransfers(childMp, tmp);
            }
            if (pending != 0) {
                r.rc = 42;
                r.err = "verify_failed=" + child + " pending="
                        + (pending < 0 ? std::string("unknown") : std::to_string(pending))
                        + " (no se destruye el dataset)\n";
                return r;
            }
        }
        ExecResult destroy = runExecCapture("zfs", {"destroy", "-r", child});
        if (destroy.rc != 0) {
            return destroy;
        }
        const fs::path dst = fs::path(parentMp) / bn;
        // Renombrado, no segunda copia: la escala está en el mismo sistema de ficheros
        // que el destino, así que esto es instantáneo. Antes se copiaba TODO otra vez
        // —los datos viajaban dos veces por cada hijo ensamblado—.
        std::error_code rmDst;
        fs::remove(dst, rmDst);  // el punto de montaje vacío que dejó el destroy
        std::error_code rnec;
        fs::rename(tmp, dst, rnec);
        if (rnec) {
            // Red de seguridad por si escala y destino acabaran en sistemas distintos.
            std::error_code mkec;
            fs::create_directories(dst, mkec);
            if (mkec) {
                r.rc = 1;
                r.err = "create_directories failed\n";
                return r;
            }
            ExecResult rsB = runRsyncCopyMoveCapture(tmp, dst.string());
            if (rsB.rc != 0) {
                return rsB;
            }
            std::error_code rmec;
            fs::remove_all(tmp, rmec);
        }
        reportJobProgress("[ASSEMBLE] ensamblado " + std::to_string(i) + " de "
                          + std::to_string(params.size() - 1) + ": " + bn);
        r.out += "[ASSEMBLE] ok " + child + " -> " + dst.string() + "\n";
    }
    r.rc = 0;
    return r;
}

std::string stripQuotes(const std::string& s) {
    std::string v = trim(s);
    if (v.size() >= 2) {
        const char a = v.front();
        const char b = v.back();
        if ((a == '\'' && b == '\'') || (a == '"' && b == '"')) {
            v = v.substr(1, v.size() - 2);
        }
    }
    return trim(v);
}

AgentRuntimeConfig loadRuntimeConfig() {
    AgentRuntimeConfig cfg;
    const char* envPath = std::getenv("ZFSMGR_AGENT_CONFIG");
    const char* configPath = (envPath && *envPath) ? envPath : kDefaultAgentConfigPath;
    std::ifstream f(configPath);
    if (!f.is_open()) {
        return cfg;
    }
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = stripQuotes(t.substr(eq + 1));
        if (key == "AGENT_BIND" && !value.empty()) {
            cfg.bind = value;
        } else if (key == "AGENT_PORT" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed > 0 && parsed <= 65535) {
                    cfg.port = parsed;
                }
            } catch (...) {
            }
        } else if (key == "TLS_CERT" && !value.empty()) {
            cfg.tlsCert = value;
        } else if (key == "TLS_KEY" && !value.empty()) {
            cfg.tlsKey = value;
        } else if (key == "TLS_CLIENT_CERT" && !value.empty()) {
            cfg.tlsClientCert = value;
        } else if (key == "AGENT_PATH" && !value.empty()) {
            cfg.commandPath = value;
        } else if (key == "CACHE_TTL_FAST_MS" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed >= 0 && parsed <= 600000) {
                    cfg.cacheTtlFastMs = parsed;
                }
            } catch (...) {
            }
        } else if (key == "CACHE_MAX_ENTRIES" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed >= 0 && parsed <= 100000) {
                    cfg.cacheMaxEntries = parsed;
                }
            } catch (...) {
            }
        } else if (key == "TRANSFER_BIND_ADDR" && !value.empty()) {
            cfg.transferBindAddr = value;
        }
    }
    return cfg;
}

void applyRuntimeEnvironment(const AgentRuntimeConfig& cfg) {
    if (cfg.commandPath.empty()) {
        return;
    }
#ifdef _WIN32
    _putenv_s("PATH", cfg.commandPath.c_str());
#else
    setenv("PATH", cfg.commandPath.c_str(), 1);
#endif
}

std::string readFirstLineFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {};
    }
    std::string line;
    std::getline(f, line);
    return trim(line);
}

// Lee el log del daemon desde un desplazamiento, o su COLA si se pide un máximo.
//
// Por qué existe: el cliente pide el log desde el desplazamiento 0 en cada arranque, y un
// daemon que lleva días en marcha tiene un latido por minuto. Medido en una VM Windows:
// 27.413 líneas de "HBEAT agent alive" transferidas y procesadas para mostrar las últimas.
//
// Se emite primero `__ZFSMGR_LOG_OFFSET__:<n>` con el desplazamiento ABSOLUTO donde
// termina lo devuelto. Sin eso el cliente no puede continuar: calculaba el siguiente
// desplazamiento sumando el tamaño de lo recibido, y al saltar a la cola esa cuenta
// dejaría de corresponder con el fichero. Un cliente viejo ve una línea que no reconoce.
std::string readDaemonLogRange(long long offset, long long maxBytes) {
    std::ifstream lf(kDaemonLogFile, std::ios::binary);
    if (!lf.is_open()) {
        return std::string();
    }
    lf.seekg(0, std::ios::end);
    const long long size = static_cast<long long>(lf.tellg());
    long long start = (offset > 0 && offset <= size) ? offset : 0;
    if (maxBytes > 0 && (size - start) > maxBytes) {
        start = size - maxBytes;
        // Alinear al siguiente salto de línea para no entregar media línea.
        lf.clear();
        lf.seekg(start);
        std::string partial;
        if (std::getline(lf, partial)) {
            const long long afterLine = static_cast<long long>(lf.tellg());
            if (afterLine >= 0 && afterLine <= size) {
                start = afterLine;
            }
        }
    }
    lf.clear();
    lf.seekg(start);
    std::ostringstream oss;
    oss << lf.rdbuf();
    std::ostringstream head;
    head << "__ZFSMGR_LOG_OFFSET__:" << size << "\n";
    return head.str() + oss.str();
}

std::string utcNowIsoString() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return {};
    }
    return buf;
}

std::string compactSpaces(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || std::isspace(c)) {
            if (!prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(c));
            prevSpace = false;
        }
    }
    return trim(out);
}

int decodeWaitStatus(int status) {
#ifndef _WIN32
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 125;
#else
    if (status < 0) {
        return 125;
    }
    // En Windows/system() y pclose() suelen devolver directamente el RC.
    if (status > 255) {
        return (status >> 8) & 0xff;
    }
    return status & 0xff;
#endif
}

int runExecStreaming(const std::string& program, const std::vector<std::string>& args) {
#ifndef _WIN32
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork failed\n";
        return 125;
    }
    if (pid == 0) {
        execvp(program.c_str(), argv.data());
        std::perror("execvp");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::perror("waitpid");
        return 125;
    }
    return decodeWaitStatus(status);
#else
    auto quoteArg = [](const std::string& a) -> std::string {
        std::string q = "\"";
        for (char c : a) {
            if (c == '"') {
                q += "\\\"";
            } else {
                q.push_back(c);
            }
        }
        q.push_back('"');
        return q;
    };
    std::string cmd = quoteArg(program);
    for (const std::string& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }
    // cmd.exe quita la PRIMERA y la ÚLTIMA comilla de la línea que recibe, así que
    // "zpool" "--version" le llega como zpool" "--version y responde que no es un
    // comando reconocido. Envolver toda la línea en un par extra hace que lo que
    // elimine sea ese par y el resto quede intacto. Comprobado contra el agente
    // nativo corriendo en un Windows 11 real.
    cmd = "\"" + cmd + "\"";
    const int rc = std::system(cmd.c_str());
    return decodeWaitStatus(rc);
#endif
}

ExecResult runExecCapture(const std::string& program, const std::vector<std::string>& args) {
    ExecResult r;
#ifndef _WIN32
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        r.rc = 125;
        r.err = "pipe failed";
        if (outPipe[0] >= 0) {
            close(outPipe[0]);
        }
        if (outPipe[1] >= 0) {
            close(outPipe[1]);
        }
        if (errPipe[0] >= 0) {
            close(errPipe[0]);
        }
        if (errPipe[1] >= 0) {
            close(errPipe[1]);
        }
        return r;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.rc = 125;
        r.err = "fork failed";
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return r;
    }

    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        execvp(program.c_str(), argv.data());
        std::perror("execvp");
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);

    auto readAllFd = [](int fd) {
        std::string out;
        char buf[4096];
        while (true) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    };

    r.out = readAllFd(outPipe[0]);
    r.err = readAllFd(errPipe[0]);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        r.rc = 125;
        r.err += "\nwaitpid failed";
        return r;
    }
    r.rc = decodeWaitStatus(status);
    return r;
#else
    // CreateProcess con dos tuberías, en vez de _popen.
    //
    // _popen obligaba a fusionar stderr en stdout con "2>&1", así que r.err salía
    // SIEMPRE vacío en Windows y los llamantes que discriminan por .err se comportaban
    // distinto que en Unix. Además pasaba por cmd.exe, que se come la primera y la
    // última comilla de la línea, y hacía falta un par extra envolviéndolo todo para
    // compensarlo. CreateProcess no invoca ningún shell: desaparecen las dos cosas.
    //
    // El entrecomillado sigue las reglas de la línea de comandos de Windows: las barras
    // invertidas solo se duplican cuando preceden a una comilla.
    auto quoteArg = [](const std::string& a) -> std::string {
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
            return a;
        }
        std::string q = "\"";
        size_t slashes = 0;
        for (char c : a) {
            if (c == '\\') {
                ++slashes;
                q.push_back(c);
                continue;
            }
            if (c == '"') {
                q.append(slashes + 1, '\\');
            }
            slashes = 0;
            q.push_back(c);
        }
        q.append(slashes, '\\');
        q.push_back('"');
        return q;
    };
    std::string cmd = quoteArg(program);
    for (const std::string& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRd = nullptr, outWr = nullptr, errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        r.rc = 125;
        r.err = "CreatePipe failed";
        return r;
    }
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        CloseHandle(outRd);
        CloseHandle(outWr);
        r.rc = 125;
        r.err = "CreatePipe failed";
        return r;
    }
    // Los extremos de lectura NO se heredan: si el hijo se queda con ellos, la tubería
    // nunca da EOF y la lectura se cuelga para siempre.
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (!started) {
        CloseHandle(outRd);
        CloseHandle(errRd);
        r.rc = 127;
        r.err = "cannot start " + program;
        return r;
    }

    // stderr se drena en otro hilo: leer una tubería entera y luego la otra se bloquea
    // en cuanto el hijo llena la que no se está leyendo.
    auto drain = [](HANDLE h, std::string* into) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            into->append(buf, n);
        }
        CloseHandle(h);
    };
    std::thread errThread(drain, errRd, &r.err);
    drain(outRd, &r.out);
    errThread.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    r.rc = GetExitCodeProcess(pi.hProcess, &exitCode) ? static_cast<int>(exitCode) : 125;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
#endif
}

#ifndef _WIN32
static ExecResult runExecCaptureWithStdin(const std::string& program,
                                           const std::vector<std::string>& args,
                                           const std::string& stdinData) {
    ExecResult r;
    int inPipe[2]  = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        r.rc = 125; r.err = "pipe failed\n";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) close(fd);
        }
        return r;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.rc = 125; r.err = "fork failed\n";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) close(fd);
        return r;
    }
    if (pid == 0) {
        dup2(inPipe[0],  STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) close(fd);
        execvp(program.c_str(), argv.data());
        std::perror("execvp"); _exit(127);
    }
    close(inPipe[0]); close(outPipe[1]); close(errPipe[1]);

    // Write stdin data, then close write end so child sees EOF
    if (!stdinData.empty()) {
        std::size_t written = 0;
        while (written < stdinData.size()) {
            const ssize_t n = write(inPipe[1], stdinData.data() + written, stdinData.size() - written);
            if (n < 0) { if (errno == EINTR) continue; break; }
            written += static_cast<std::size_t>(n);
        }
    }
    close(inPipe[1]);

    auto readAllFd = [](int fd) {
        std::string out; char buf[4096];
        while (true) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n == 0) break;
            if (n < 0) { if (errno == EINTR) continue; break; }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    };
    r.out = readAllFd(outPipe[0]);
    r.err = readAllFd(errPipe[0]);
    close(outPipe[0]); close(errPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { r.rc = 125; r.err += "\nwaitpid failed"; return r; }
    r.rc = decodeWaitStatus(status);
    return r;
}
#endif

bool startsWith(const std::string& s, const std::string& pref) {
    return s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
}

void onSignal(int) {
    g_stop.store(true);
}

void writeHeartbeat() {
    std::ofstream f(kHeartbeatPath, std::ios::app);
    if (!f.is_open()) {
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return;
    }
    f << buf << " agent alive\n";
    daemonLog("HBEAT", "agent alive");
}

static void rotateDaemonLog() {
    std::ifstream check(kDaemonLogFile, std::ios::ate | std::ios::binary);
    if (!check.is_open()) return;
    const auto sz = check.tellg();
    check.close();
    if (sz < static_cast<std::streamoff>(kDaemonLogMaxBytes)) return;
    for (int i = 4; i >= 1; --i) {
        const std::string from = std::string(kDaemonLogFile) + "." + std::to_string(i);
        const std::string to   = std::string(kDaemonLogFile) + "." + std::to_string(i + 1);
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(kDaemonLogFile, (std::string(kDaemonLogFile) + ".1").c_str());
}

void daemonLog(const std::string& level, const std::string& msg) {
    const std::string ts = utcNowIsoString();
    if (ts.empty()) return;
    std::lock_guard<std::mutex> lock(g_daemonLogMutex);
    rotateDaemonLog();
    std::ofstream f(kDaemonLogFile, std::ios::app);
    if (!f.is_open()) {
        // El directorio puede no existir todavía en una instalación recién hecha.
        // Antes se retornaba sin más y el log quedaba mudo para siempre.
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(kDaemonLogFile).parent_path(), ec);
        f.open(kDaemonLogFile, std::ios::app);
        if (!f.is_open()) {
            return;
        }
    }
    f << ts << " " << level << " " << msg << "\n";
}

std::string detectOsLine() {
#ifdef _WIN32
    // Sin esto la línea de sistema salía siendo el mensaje de error de cmd al no
    // encontrar "uname", que es lo que la ficha de conexión mostraba tal cual. Se lee
    // del registro, que es de donde la propia GUI ya lo sacaba por su cuenta.
    {
        char buf[256];
        DWORD sz = sizeof(buf);
        std::string product;
        std::string display;
        if (RegGetValueA(HKEY_LOCAL_MACHINE,
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "ProductName", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
            product = buf;
        }
        sz = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE,
                         "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "DisplayVersion", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) {
            sz = sizeof(buf);
            if (RegGetValueA(HKEY_LOCAL_MACHINE,
                             "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             "CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
                display = std::string("build ") + buf;
            }
        } else {
            display = buf;
        }
        if (product.empty()) {
            return "Windows";
        }
        return display.empty() ? product : product + " " + display;
    }
#else
    ExecResult unames = runExecCapture("uname", {"-s"});
    const std::string unameS = trim(unames.out);
    if (unameS == "Linux") {
        std::ifstream f("/etc/os-release");
        if (f.is_open()) {
            std::string line;
            std::string name;
            std::string ver;
            while (std::getline(f, line)) {
                if (startsWith(line, "NAME=")) {
                    name = line.substr(5);
                } else if (startsWith(line, "VERSION_ID=")) {
                    ver = line.substr(11);
                }
            }
            auto stripQuote = [](std::string x) {
                x = trim(x);
                if (x.size() >= 2) {
                    const char a = x.front();
                    const char b = x.back();
                    if ((a == '\'' && b == '\'') || (a == '"' && b == '"')) {
                        x = x.substr(1, x.size() - 2);
                    }
                }
                return trim(x);
            };
            name = stripQuote(name);
            ver = stripQuote(ver);
            const std::string combined = trim(name + " " + ver);
            if (!combined.empty()) {
                return combined;
            }
        }
        return "Linux";
    }
    if (unameS == "Darwin") {
        const std::string pn = trim(runExecCapture("sw_vers", {"-productName"}).out);
        const std::string pv = trim(runExecCapture("sw_vers", {"-productVersion"}).out);
        const std::string combined = trim(pn + " " + pv);
        return combined.empty() ? "macOS" : combined;
    }
    if (unameS == "FreeBSD") {
        std::string v = trim(runExecCapture("freebsd-version", {"-k"}).out);
        if (v.empty()) {
            v = trim(runExecCapture("freebsd-version", {}).out);
        }
        if (v.empty()) {
            v = trim(runExecCapture("uname", {"-r"}).out);
        }
        if (!v.empty()) {
            return "FreeBSD " + v;
        }
        return "FreeBSD";
    }
    const std::string ua = trim(runExecCapture("uname", {"-a"}).out);
    return ua.empty() ? unameS : ua;
#endif
}

std::string detectMachineUuid() {
#ifdef _WIN32
    // MachineGuid del registro: es el mismo identificador que la GUI ya leía por su
    // cuenta para deduplicar conexiones, así que ambos lados coinciden.
    {
        char buf[128];
        DWORD sz = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                         "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                         nullptr, buf, &sz) == ERROR_SUCCESS) {
            return trim(std::string(buf));
        }
        return std::string();
    }
#else
    std::string id = readFirstLineFile("/etc/machine-id");
    if (!id.empty()) {
        return id;
    }
    id = readFirstLineFile("/var/lib/dbus/machine-id");
    if (!id.empty()) {
        return id;
    }
    const ExecResult io = runExecCapture("ioreg", {"-rd1", "-c", "IOPlatformExpertDevice"});
    for (const std::string& line : splitLines(io.out + "\n" + io.err)) {
        const auto pos = line.find("IOPlatformUUID");
        if (pos == std::string::npos) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq + 1 >= line.size()) {
            continue;
        }
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        val = trim(val);
        if (!val.empty()) {
            return val;
        }
    }
    return {};
#endif
}

std::string detectZfsVersionRaw() {
    const std::vector<std::pair<std::string, std::vector<std::string>>> tries = {
        {"zfs", {"version"}},
        {"zfs", {"--version"}},
        {"zpool", {"--version"}},
    };
    for (const auto& t : tries) {
        const ExecResult e = runExecCapture(t.first, t.second);
        const std::string merged = compactSpaces(e.out + "\n" + e.err);
        if (e.rc == 0 && !merged.empty()) {
            return merged;
        }
    }
    return {};
}

bool isAllowedMutationOp(const std::string& tool, const std::string& opRaw) {
    static const std::set<std::string> zfsAllowed = {
        "create", "destroy", "rollback", "clone", "rename", "set", "inherit", "mount", "unmount",
        "hold", "release", "load-key", "unload-key", "change-key", "promote", "allow", "unallow",
    };
    static const std::set<std::string> zpoolAllowed = {
        "create", "destroy", "add", "remove", "attach", "detach", "replace", "offline", "online",
        "clear", "export", "import", "scrub", "trim", "initialize", "sync", "upgrade", "reguid",
        "split", "checkpoint",
    };
    const std::string op = toLower(trim(opRaw));
    if (tool == "zfs") {
        return zfsAllowed.count(op) > 0;
    }
    if (tool == "zpool") {
        return zpoolAllowed.count(op) > 0;
    }
    return false;
}

int fromBase64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool decodeBase64(const std::string& in, std::string& out) {
    out.clear();
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int d = fromBase64(c);
        if (d < 0) {
            return false;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

bool parseJsonStringArray(const std::string& json, std::vector<std::string>& out) {
    out.clear();
    std::size_t i = 0;
    auto skipWs = [&]() {
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
    };
    skipWs();
    if (i >= json.size() || json[i] != '[') {
        return false;
    }
    ++i;
    skipWs();
    if (i < json.size() && json[i] == ']') {
        ++i;
        skipWs();
        return i == json.size();
    }

    while (i < json.size()) {
        skipWs();
        if (i >= json.size() || json[i] != '"') {
            return false;
        }
        ++i;
        std::string s;
        while (i < json.size()) {
            const char c = json[i++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (i >= json.size()) {
                    return false;
                }
                const char e = json[i++];
                switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u':
                    if (i + 4 > json.size()) {
                        return false;
                    }
                    i += 4;
                    s.push_back('?');
                    break;
                default:
                    return false;
                }
            } else {
                s.push_back(c);
            }
        }
        out.push_back(s);
        skipWs();
        if (i >= json.size()) {
            return false;
        }
        if (json[i] == ',') {
            ++i;
            continue;
        }
        if (json[i] == ']') {
            ++i;
            skipWs();
            return i == json.size();
        }
        return false;
    }
    return false;
}

int runGenericMutation(const std::string& tool, const std::string& payloadB64) {
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        std::cerr << "invalid generic payload\n";
        return 2;
    }
    std::vector<std::string> arr;
    if (!parseJsonStringArray(decoded, arr) || arr.empty()) {
        std::cerr << "invalid generic payload\n";
        return 2;
    }
    if (!isAllowedMutationOp(tool, arr.front())) {
        std::cerr << "unsupported " << tool << " mutation op\n";
        return 2;
    }
    return runExecStreaming(tool, arr);
}

ExecResult runGenericMutationCapture(const std::string& tool, const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        r.rc = 2;
        r.err = "invalid generic payload\n";
        return r;
    }
    std::vector<std::string> arr;
    if (!parseJsonStringArray(decoded, arr) || arr.empty()) {
        r.rc = 2;
        r.err = "invalid generic payload\n";
        return r;
    }
    if (!isAllowedMutationOp(tool, arr.front())) {
        r.rc = 2;
        r.err = "unsupported " + tool + " mutation op\n";
        return r;
    }
    return runExecCapture(tool, arr);
}

#ifndef _WIN32
// ── Sync via tar with a temporary mountpoint ─────────────────────────────────
// Typed replacement for the buildUnixTemporaryTar{Source,Destination}Script shell
// scripts. A dataset that is not mounted is temporarily relocated to a scratch
// mountpoint so its contents can be streamed, then put back.
//
// The original mountpoint is stashed in a ZFS user property before moving it, exactly
// as the shell helpers did: that way a run killed outright still leaves a breadcrumb
// identifying the dataset and its intended mountpoint.
constexpr const char* kSavedMountpointProp = "org.fc16.zfsmgr:savedmountpoint";

std::atomic<pid_t> g_streamChildPid{-1};

// Async-signal-safe: only touches an atomic and calls kill(). The actual restore
// happens in AltMountGuard's destructor once the wait below returns.
void onStreamSignal(int) {
    g_stop.store(true);
    const pid_t child = g_streamChildPid.load();
    if (child > 0) {
        kill(child, SIGTERM);
    }
}

struct AltMountGuard {
    std::string dataset;
    std::string tmpDir;
    bool active{false};

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
        const ExecResult cur = getZfsPropertyCapture(ds, "mountpoint");
        const std::string savedMp = (cur.rc == 0) ? trim(cur.out) : std::string();
        // Record the original mountpoint before touching anything.
        (void)runExecCapture("zfs",
                             {"set", std::string(kSavedMountpointProp) + "=" + savedMp, ds});
        const ExecResult setRes = runExecCapture("zfs", {"set", "mountpoint=" + tmpDir, ds});
        if (setRes.rc != 0) {
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, ds});
            rmdir(tmpDir.c_str());
            tmpDir.clear();
            errMsg = setRes.err.empty() ? "cannot set temporary mountpoint\n" : setRes.err;
            return false;
        }
        active = true;  // from here on the dataset must be restored
        const ExecResult mountRes = runExecCapture("zfs", {"mount", ds});
        if (mountRes.rc != 0) {
            errMsg = mountRes.err.empty() ? "cannot mount on temporary mountpoint\n" : mountRes.err;
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
        (void)runExecCapture("zfs", {"unmount", dataset});
        const ExecResult saved = getZfsPropertyCapture(dataset, kSavedMountpointProp);
        const std::string savedMp = (saved.rc == 0) ? trim(saved.out) : std::string();
        if (!savedMp.empty() && savedMp != "-") {
            (void)runExecCapture("zfs", {"set", "mountpoint=" + savedMp, dataset});
        }
        (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, dataset});
        if (!tmpDir.empty()) {
            rmdir(tmpDir.c_str());
            tmpDir.clear();
        }
    }

    ~AltMountGuard() { release(); }
};


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

bool pathIsZfsMountpoint(const std::string& path) {
    const ExecResult mounts = runExecCapture("zfs", {"mount"});
    if (mounts.rc != 0) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path want = std::filesystem::weakly_canonical(path, ec);
    const std::string wantStr = ec ? path : want.string();
    for (const std::string& line : splitLines(mounts.out)) {
        // "<dataset><espacios><mountpoint>"; el mountpoint puede llevar espacios.
        const std::size_t sep = line.find_first_of(" \t");
        if (sep == std::string::npos) {
            continue;
        }
        std::size_t mpStart = line.find_first_not_of(" \t", sep);
        if (mpStart == std::string::npos) {
            continue;
        }
        const std::string mp = trim(line.substr(mpStart));
        if (mp.empty()) {
            continue;
        }
        std::error_code mec;
        const std::filesystem::path canon = std::filesystem::weakly_canonical(mp, mec);
        if ((mec ? mp : canon.string()) == wantStr) {
            return true;
        }
    }
    return false;
}

ExecResult runMutateAdvancedToDirCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 3) {
        r.rc = 2;
        r.err = "usage: --mutate-advanced-todir <dataset> <dst-dir> <delete-source-0|1>\n";
        return r;
    }
    namespace fs = std::filesystem;
    const std::string dataset = params[0];
    const std::string dstDir = params[1];
    const bool deleteSource = isTruthyValue(params[2]);

    if (dstDir.empty() || dstDir[0] != '/') {
        r.rc = 2;
        r.err = "destination directory must be an absolute path\n";
        return r;
    }
    if (pathIsZfsMountpoint(dstDir)) {
        r.rc = 2;
        r.err = "destination directory is already a zfs mountpoint\n";
        return r;
    }

    // Reubicar el dataset a un mountpoint temporal: sin esto, el destino no puede
    // ser la propia ruta donde el dataset estaba montado, que es el caso principal
    // (convertir un dataset en un directorio en su mismo sitio).
    AltMountGuard mountGuard;
    std::string srcMp;
    std::string mountErr;
    if (!mountGuard.engage(dataset, srcMp, mountErr)) {
        r.rc = 1;
        r.err = mountErr.empty() ? "cannot relocate dataset to a temporary mountpoint\n" : mountErr;
        return r;
    }

    const fs::path dst(dstDir);
    const fs::path parent = dst.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        r.rc = 1;
        r.err = "cannot create destination parent directory\n";
        return r;
    }

    // Temporal hermano del destino para que el rename final sea atómico.
    const fs::path staging = parent / (dst.filename().string() + ".zfsmgr-new");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        r.rc = 1;
        r.err = "cannot create staging directory next to destination\n";
        return r;
    }

    ExecResult rs = runRsyncCopyMoveCapture(srcMp, staging.string());
    if (rs.rc != 0) {
        fs::remove_all(staging, ec);
        return rs;
    }

    // Apartar el destino existente; el guard lo repone si algo falla a partir de aquí.
    DestBackupGuard backupGuard;
    backupGuard.target = dst;
    if (fs::exists(dst, ec)) {
        fs::path candidate = parent / (dst.filename().string() + ".zfsmgr-bak");
        for (int i = 1; fs::exists(candidate, ec); ++i) {
            candidate = parent / (dst.filename().string() + ".zfsmgr-bak-" + std::to_string(i));
        }
        fs::rename(dst, candidate, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            r.rc = 1;
            r.err = "cannot move existing destination aside\n";
            return r;
        }
        backupGuard.backup = candidate;
        backupGuard.active = true;
    }

    fs::rename(staging, dst, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        r.rc = 1;
        r.err = "cannot move copied data into place\n";
        return r;  // el guard repone el destino anterior
    }

    if (deleteSource) {
        (void)runExecCapture("zfs", {"unmount", dataset});
        ExecResult d = runExecCapture("zfs", {"destroy", "-r", dataset});
        if (d.rc != 0) {
            return d;  // el guard repone el destino: el dataset sigue vivo
        }
        mountGuard.active = false;  // ya no existe: no hay nada que restaurar
        mountGuard.release();
    } else {
        mountGuard.release();  // devuelve el dataset a su mountpoint original
        (void)runExecCapture("zfs", {"unmount", dataset});
        (void)runExecCapture("zfs", {"set", "canmount=off", dataset});
    }

    // A partir de aquí la operación es definitiva: se descarta el respaldo.
    backupGuard.commit();
    if (!backupGuard.backup.empty()) {
        fs::remove_all(backupGuard.backup, ec);
    }
    r.rc = 0;
    r.out = deleteSource ? "[TODIR] ok\n" : "[TODIR] ok (dataset preserved unmounted)\n";
    return r;
}


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

std::vector<StrandedAltMount> findStrandedAltMounts() {
    std::vector<StrandedAltMount> out;
    // -s local restricts this to datasets where we explicitly set the property.
    const ExecResult r = runExecCapture(
        "zfs", {"get", "-H", "-o", "name,value", "-s", "local", kSavedMountpointProp});
    if (r.rc != 0) {
        return out;
    }
    std::istringstream iss(r.out);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmedLine = trim(line);
        if (trimmedLine.empty()) {
            continue;
        }
        const std::size_t tab = trimmedLine.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        StrandedAltMount s;
        s.dataset = trim(trimmedLine.substr(0, tab));
        s.savedMp = trim(trimmedLine.substr(tab + 1));
        if (s.dataset.empty()) {
            continue;
        }
        const ExecResult cur = getZfsPropertyCapture(s.dataset, "mountpoint");
        s.currentMp = (cur.rc == 0) ? trim(cur.out) : std::string();
        out.push_back(std::move(s));
    }
    return out;
}

// --repair-alt-mountpoints [apply]
// Without "apply" it only reports, so it is safe to call for diagnosis.
ExecResult runRepairAltMountpointsCapture(bool apply) {
    ExecResult r;
    const std::vector<StrandedAltMount> stranded = findStrandedAltMounts();
    std::string out;
    int repaired = 0;
    int failed = 0;
    for (const StrandedAltMount& s : stranded) {
        out += "STRANDED=" + s.dataset + " saved=" + s.savedMp + " current=" + s.currentMp + "\n";
        if (!apply) {
            continue;
        }
        // Mirror the guard's restore order: unmount first, then put the mountpoint
        // back, then drop the breadcrumb.
        (void)runExecCapture("zfs", {"unmount", s.dataset});
        bool ok = true;
        if (!s.savedMp.empty() && s.savedMp != "-") {
            const ExecResult setRes =
                runExecCapture("zfs", {"set", "mountpoint=" + s.savedMp, s.dataset});
            ok = (setRes.rc == 0);
            if (!ok) {
                r.err += "cannot restore mountpoint for " + s.dataset + ": " + setRes.err;
            }
        }
        if (ok) {
            // Only drop the breadcrumb once the mountpoint is actually back, so a
            // failed repair stays discoverable on the next run.
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, s.dataset});
            out += "REPAIRED=" + s.dataset + " -> " + s.savedMp + "\n";
            ++repaired;
        } else {
            ++failed;
        }
    }
    out += "STRANDED_COUNT=" + std::to_string(stranded.size()) + "\n";
    out += "REPAIRED_COUNT=" + std::to_string(repaired) + "\n";
    out += "FAILED_COUNT=" + std::to_string(failed) + "\n";
    r.out = out;
    r.rc = (failed > 0) ? 1 : 0;
    return r;
}

// Runs `producer | consumer`, both execvp'd, with the outer ends left on our own
// stdin/stdout so the tar stream flows through the SSH pipeline.
int runStreamPair(const std::vector<std::string>& producer,
                  const std::vector<std::string>& consumer) {
    auto toArgv = [](const std::vector<std::string>& v) {
        std::vector<char*> a;
        a.reserve(v.size() + 1);
        for (const std::string& s : v) {
            a.push_back(const_cast<char*>(s.c_str()));
        }
        a.push_back(nullptr);
        return a;
    };
    if (consumer.empty()) {
        std::vector<char*> a = toArgv(producer);
        const pid_t pid = fork();
        if (pid < 0) return 125;
        if (pid == 0) {
            execvp(producer[0].c_str(), a.data());
            _exit(127);
        }
        g_streamChildPid.store(pid);
        int st = 0;
        waitpid(pid, &st, 0);
        g_streamChildPid.store(-1);
        return decodeWaitStatus(st);
    }
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return 125;
    std::vector<char*> pa = toArgv(producer);
    std::vector<char*> ca = toArgv(consumer);
    const pid_t p1 = fork();
    if (p1 < 0) { close(fds[0]); close(fds[1]); return 125; }
    if (p1 == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        execvp(producer[0].c_str(), pa.data());
        _exit(127);
    }
    const pid_t p2 = fork();
    if (p2 < 0) { close(fds[0]); close(fds[1]); int st = 0; waitpid(p1, &st, 0); return 125; }
    if (p2 == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);
        execvp(consumer[0].c_str(), ca.data());
        _exit(127);
    }
    close(fds[0]);
    close(fds[1]);
    g_streamChildPid.store(p1);
    int st1 = 0;
    int st2 = 0;
    waitpid(p1, &st1, 0);
    waitpid(p2, &st2, 0);
    g_streamChildPid.store(-1);
    const int rc2 = decodeWaitStatus(st2);
    return (rc2 != 0) ? rc2 : decodeWaitStatus(st1);
}

bool validSyncCodec(const std::string& codec) {
    return codec == "none" || codec == "zstd" || codec == "gzip";
}

// --mutate-sync-temp-tar-source <dataset> [none|zstd|gzip]
int runSyncTempTarSource(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-sync-temp-tar-source <dataset> [none|zstd|gzip]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string codec = params.size() > 1 ? toLower(trim(params[1])) : std::string("none");
    if (dataset.empty() || !validSyncCodec(codec)) {
        std::cerr << "invalid dataset or codec\n";
        return 2;
    }
    std::signal(SIGINT, onStreamSignal);
    std::signal(SIGTERM, onStreamSignal);

    AltMountGuard guard;
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::string errMsg;
        if (!guard.engage(dataset, mp, errMsg)) {
            std::cerr << errMsg;
            return 41;
        }
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (mp.empty() || !fs::is_directory(fs::path(mp), ec)) {
        std::cerr << "could not resolve a usable mountpoint\n";
        return 41;
    }
    const std::vector<std::string> tarCmd = {"tar", "--acls", "--xattrs", "-cpf", "-", "-C", mp, "."};
    if (codec == "zstd") {
        return runStreamPair(tarCmd, {"zstd", "-1", "-T0", "-q", "-c"});
    }
    if (codec == "gzip") {
        return runStreamPair(tarCmd, {"gzip", "-1", "-c"});
    }
    return runStreamPair(tarCmd, {});
}

// --mutate-sync-temp-tar-dest <dataset> [none|zstd|gzip]
int runSyncTempTarDest(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-sync-temp-tar-dest <dataset> [none|zstd|gzip]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string codec = params.size() > 1 ? toLower(trim(params[1])) : std::string("none");
    if (dataset.empty() || !validSyncCodec(codec)) {
        std::cerr << "invalid dataset or codec\n";
        return 2;
    }
    std::signal(SIGINT, onStreamSignal);
    std::signal(SIGTERM, onStreamSignal);

    AltMountGuard guard;
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::string errMsg;
        if (!guard.engage(dataset, mp, errMsg)) {
            std::cerr << errMsg;
            return 41;
        }
    }
    if (mp.empty()) {
        std::cerr << "could not resolve a usable mountpoint\n";
        return 41;
    }
    namespace fs = std::filesystem;
    std::error_code mkec;
    fs::create_directories(fs::path(mp), mkec);
    const std::vector<std::string> tarCmd = {"tar", "--acls", "--xattrs", "-xpf", "-", "-C", mp};
    if (codec == "zstd") {
        return runStreamPair({"zstd", "-d", "-q", "-c", "-"}, tarCmd);
    }
    if (codec == "gzip") {
        return runStreamPair({"gzip", "-d", "-c", "-"}, tarCmd);
    }
    return runStreamPair(tarCmd, {});
}

// --mutate-advanced-fromdir: typed replacement for the "Desde Dir" destination shell
// script. Mounts the dataset, resolves its effective mountpoint, creates the target
// subdirectory and extracts the tar stream arriving on stdin — all with execvp, no
// shell. Completes the set alongside --mutate-advanced-breakdown/assemble/todir.
//
// CLI only, deliberately: this reads the tar stream from stdin, and the RPC channel
// has no stdin. The GUI invokes it as the destination end of an SSH pipeline.
int runMutateAdvancedFromDir(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-advanced-fromdir <dataset> [relative-subdir]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string rel = params.size() > 1 ? trim(params[1]) : std::string();
    if (dataset.empty()) {
        std::cerr << "empty dataset\n";
        return 2;
    }
    // Refuse anything that could escape the dataset's mountpoint.
    if (!rel.empty() && (rel.front() == '/' || rel.find("..") != std::string::npos)) {
        std::cerr << "invalid relative subdirectory\n";
        return 2;
    }

    (void)runExecCapture("zfs", {"set", "canmount=on", dataset});
    (void)runExecCapture("zfs", {"mount", dataset});
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    const std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::cerr << "could not resolve effective mountpoint\n";
        return 4;
    }

    const std::string dst = rel.empty() ? mp : (mp + "/" + rel);
    namespace fs = std::filesystem;
    std::error_code mkec;
    fs::create_directories(fs::path(dst), mkec);
    if (mkec) {
        std::cerr << "cannot create destination directory\n";
        return 1;
    }
    std::cout << "[FROMDIR] tar recv -> " << dst << "\n";
    std::cout.flush();
    // stdin stays inherited so tar consumes the incoming stream.
    return runExecStreaming("tar", {"--acls", "--xattrs", "-xpf", "-", "-C", dst});
}
#endif // _WIN32

// --mutate-zfs-allow-batch: typed replacement for the old semicolon-joined shell
// batch that used to be routed through --mutate-shell-generic. Payload is
// base64(JSON array); each element is base64(JSON array) holding one
// `zfs allow|unallow ...` argv (without the leading "zfs"). Only allow/unallow
// ops are accepted, and every entry runs via execvp — never through a shell.
bool decodeZfsAllowBatch(const std::string& payloadB64,
                         std::vector<std::vector<std::string>>& out,
                         std::string& errMsg) {
    out.clear();
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        errMsg = "invalid allow-batch payload\n";
        return false;
    }
    std::vector<std::string> items;
    if (!parseJsonStringArray(decoded, items) || items.empty()) {
        errMsg = "invalid allow-batch payload\n";
        return false;
    }
    for (const std::string& itemB64 : items) {
        std::string innerJson;
        std::vector<std::string> argv;
        if (!decodeBase64(itemB64, innerJson) || !parseJsonStringArray(innerJson, argv) || argv.empty()) {
            errMsg = "invalid allow-batch entry\n";
            return false;
        }
        const std::string op = toLower(trim(argv.front()));
        if (op != "allow" && op != "unallow") {
            errMsg = "unsupported allow-batch op: " + argv.front() + "\n";
            return false;
        }
        out.push_back(std::move(argv));
    }
    return true;
}

ExecResult runZfsAllowBatchCapture(const std::string& payloadB64) {
    ExecResult r;
    std::vector<std::vector<std::string>> batch;
    std::string errMsg;
    if (!decodeZfsAllowBatch(payloadB64, batch, errMsg)) {
        r.rc = 2;
        r.err = errMsg;
        return r;
    }
    r.rc = 0;
    for (const std::vector<std::string>& argv : batch) {
        const ExecResult sub = runExecCapture("zfs", argv);
        r.out += sub.out;
        r.err += sub.err;
        if (sub.rc != 0 && r.rc == 0) {
            r.rc = sub.rc;  // run all entries; surface the first failure
        }
    }
    return r;
}

int runZfsAllowBatch(const std::string& payloadB64) {
    std::vector<std::vector<std::string>> batch;
    std::string errMsg;
    if (!decodeZfsAllowBatch(payloadB64, batch, errMsg)) {
        std::cerr << errMsg;
        return 2;
    }
    int rc = 0;
    for (const std::vector<std::string>& argv : batch) {
        const int sub = runExecStreaming("zfs", argv);
        if (sub != 0 && rc == 0) {
            rc = sub;  // run all entries; surface the first failure
        }
    }
    return rc;
}

// --mutate-rsync-local: typed rsync sync. Replaces the shell command that used to
// probe rsync capabilities inline (`rsync --help | grep ...`) and interpolate the
// resulting $RSYNC_OPTS into a shell line. The capability probe now runs here, the
// daemon assembles the argv, and rsync is exec'd directly — no shell involved.
//
// Payload is base64(JSON array) of strings:
//   [delete("0"|"1"), dryRun("0"|"1"), rsh, dstHost, src1, dst1, src2, dst2, ...]
// There must be at least one src/dst pair and the trailing path list must be even.
// Multiple pairs cover the subdataset case, which previously chained N rsync calls
// with `set -e; ... && ...`: the probe runs once and the pairs execute in order,
// stopping at the first failure (same semantics).
// rsh/dstHost are empty for a same-host sync. When dstHost is set the destination
// becomes "<dstHost>:<dst>/" and rsh (if non-empty) is passed as -e; rsync applies
// its own quote-aware tokenization to that string, matching the previous behavior.
bool buildRsyncLocalPlan(const std::string& payloadB64,
                         std::vector<std::vector<std::string>>& argvsOut,
                         std::string& errMsg) {
    argvsOut.clear();
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        errMsg = "invalid rsync payload\n";
        return false;
    }
    std::vector<std::string> f;
    if (!parseJsonStringArray(decoded, f) || f.size() < 6 || ((f.size() - 4) % 2) != 0) {
        errMsg = "invalid rsync payload\n";
        return false;
    }
    const std::string delFlag = trim(f[0]);
    const std::string dryFlag = trim(f[1]);
    const std::string rsh = f[2];
    const std::string dstHost = trim(f[3]);
    if ((delFlag != "0" && delFlag != "1") || (dryFlag != "0" && dryFlag != "1")) {
        errMsg = "invalid rsync flag\n";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> pairs;
    for (std::size_t i = 4; i + 1 < f.size(); i += 2) {
        const std::string src = trim(f[i]);
        const std::string dst = trim(f[i + 1]);
        if (src.empty() || dst.empty() || src[0] != '/' || dst[0] != '/') {
            errMsg = "rsync paths must be absolute\n";
            return false;
        }
        pairs.emplace_back(src, dst);
    }
    if (pairs.empty()) {
        errMsg = "invalid rsync payload\n";
        return false;
    }

    // Capability probe (previously done inline in shell), run once for all pairs.
    const ExecResult help = runExecCapture("rsync", {"--help"});
    const std::string helpText = help.out + help.err;
    const bool hasInfo = helpText.find("--info") != std::string::npos;

    std::vector<std::string> base;
    base.push_back("-aHWS");
    if (delFlag == "1") {
        base.push_back("--delete");
    }
    if (dryFlag == "1") {
        base.push_back("--dry-run");
    }
    // Mismo sondeo que runRsyncCopyMoveCapture, compartido para que no puedan
    // divergir otra vez.
    for (const std::string& f : rsyncPreservationFlags()) {
        base.push_back(f);
    }
    base.push_back(hasInfo ? "--info=progress2" : "--progress");
    if (!dstHost.empty() && !rsh.empty()) {
        base.push_back("-e");
        base.push_back(rsh);
    }

    for (const auto& p : pairs) {
        std::vector<std::string> argv = base;
        argv.push_back(p.first + "/");
        argv.push_back(dstHost.empty() ? (p.second + "/") : (dstHost + ":" + p.second + "/"));
        argvsOut.push_back(std::move(argv));
    }
    return true;
}

ExecResult runRsyncLocalCapture(const std::string& payloadB64) {
    ExecResult r;
    std::vector<std::vector<std::string>> argvs;
    std::string errMsg;
    if (!buildRsyncLocalPlan(payloadB64, argvs, errMsg)) {
        r.rc = 2;
        r.err = errMsg;
        return r;
    }
    r.rc = 0;
    for (const std::vector<std::string>& argv : argvs) {
        const ExecResult sub = runExecCapture("rsync", argv);
        r.out += sub.out;
        r.err += sub.err;
        if (sub.rc != 0) {
            r.rc = sub.rc;  // stop at first failure, like the previous `set -e`
            break;
        }
    }
    return r;
}

int runRsyncLocal(const std::string& payloadB64) {
    std::vector<std::vector<std::string>> argvs;
    std::string errMsg;
    if (!buildRsyncLocalPlan(payloadB64, argvs, errMsg)) {
        std::cerr << errMsg;
        return 2;
    }
    for (const std::vector<std::string>& argv : argvs) {
        const int sub = runExecStreaming("rsync", argv);
        if (sub != 0) {
            return sub;  // stop at first failure, like the previous `set -e`
        }
    }
    return 0;
}

#ifndef _WIN32
// --zfs-pipe-local: typed same-host `zfs send ... | zfs recv ...`. Replaces the
// shell pipeline that used to be routed through --mutate-shell-generic. Payload is
// base64(JSON array) of exactly two elements, each base64(JSON array) holding one
// argv without the leading "zfs": entry 0 must start with "send", entry 1 with
// "recv". The daemon wires the pipe itself and execvp()s both ends, so no shell is
// involved and nothing but zfs send/recv can be expressed.
bool decodeZfsPipeLocal(const std::string& payloadB64,
                        std::vector<std::string>& sendArgv,
                        std::vector<std::string>& recvArgv,
                        std::string& errMsg) {
    sendArgv.clear();
    recvArgv.clear();
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        errMsg = "invalid pipe-local payload\n";
        return false;
    }
    std::vector<std::string> items;
    if (!parseJsonStringArray(decoded, items) || items.size() != 2) {
        errMsg = "invalid pipe-local payload\n";
        return false;
    }
    auto decodeOne = [&](const std::string& b64,
                         const char* expectOp,
                         std::vector<std::string>& out) -> bool {
        std::string json;
        if (!decodeBase64(b64, json) || !parseJsonStringArray(json, out) || out.empty()) {
            errMsg = "invalid pipe-local entry\n";
            return false;
        }
        if (toLower(trim(out.front())) != expectOp) {
            errMsg = std::string("pipe-local entry must start with ") + expectOp + "\n";
            return false;
        }
        return true;
    };
    if (!decodeOne(items[0], "send", sendArgv)) {
        return false;
    }
    if (!decodeOne(items[1], "recv", recvArgv)) {
        return false;
    }
    return true;
}

ExecResult runZfsPipeLocalCapture(const std::string& payloadB64) {
    ExecResult r;
    std::vector<std::string> sendArgv;
    std::vector<std::string> recvArgv;
    std::string errMsg;
    if (!decodeZfsPipeLocal(payloadB64, sendArgv, recvArgv, errMsg)) {
        r.rc = 2;
        r.err = errMsg;
        return r;
    }

    int dataPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(dataPipe) != 0) {
        r.rc = 125;
        r.err = "pipe failed\n";
        return r;
    }
    if (pipe(errPipe) != 0) {
        close(dataPipe[0]);
        close(dataPipe[1]);
        r.rc = 125;
        r.err = "pipe failed\n";
        return r;
    }

    auto buildArgv = [](const std::vector<std::string>& v) {
        std::vector<char*> a;
        a.reserve(v.size() + 2);
        a.push_back(const_cast<char*>("zfs"));
        for (const std::string& s : v) {
            a.push_back(const_cast<char*>(s.c_str()));
        }
        a.push_back(nullptr);
        return a;
    };
    std::vector<char*> sendA = buildArgv(sendArgv);
    std::vector<char*> recvA = buildArgv(recvArgv);

    auto closeAll = [&]() {
        close(dataPipe[0]);
        close(dataPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
    };

    const pid_t sendPid = fork();
    if (sendPid < 0) {
        closeAll();
        r.rc = 125;
        r.err = "fork failed\n";
        return r;
    }
    if (sendPid == 0) {
        dup2(dataPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        closeAll();
        execvp("zfs", sendA.data());
        _exit(127);
    }

    const pid_t recvPid = fork();
    if (recvPid < 0) {
        closeAll();
        int st = 0;
        waitpid(sendPid, &st, 0);
        r.rc = 125;
        r.err = "fork failed\n";
        return r;
    }
    if (recvPid == 0) {
        dup2(dataPipe[0], STDIN_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        closeAll();
        execvp("zfs", recvA.data());
        _exit(127);
    }

    // Parent must drop both data-pipe ends so recv sees EOF when send finishes.
    close(dataPipe[0]);
    close(dataPipe[1]);
    close(errPipe[1]);

    std::string errAccum;
    char buf[4096];
    while (true) {
        const ssize_t n = read(errPipe[0], buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        errAccum.append(buf, static_cast<std::size_t>(n));
    }
    close(errPipe[0]);

    int sendStatus = 0;
    int recvStatus = 0;
    waitpid(sendPid, &sendStatus, 0);
    waitpid(recvPid, &recvStatus, 0);
    const int sendRc = decodeWaitStatus(sendStatus);
    const int recvRc = decodeWaitStatus(recvStatus);

    r.err = errAccum;
    r.rc = (recvRc != 0) ? recvRc : sendRc;
    return r;
}
#endif // _WIN32

int runMutateShellGeneric(const std::string& payloadB64) {
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        std::cerr << "invalid shell payload\n";
        return 2;
    }
    decoded = trim(decoded);
    if (decoded.empty()) {
        std::cerr << "empty shell payload\n";
        return 2;
    }
    return runExecStreaming("sh", {"-lc", decoded});
}

ExecResult runMutateShellGenericCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        r.rc = 2;
        r.err = "invalid shell payload\n";
        return r;
    }
    decoded = trim(decoded);
    if (decoded.empty()) {
        r.rc = 2;
        r.err = "empty shell payload\n";
        return r;
    }
    return runExecCapture("sh", {"-lc", decoded});
}

#ifdef HAVE_LIBZFS_CORE
// ── libzfs_core native helpers (subprocess fallback used on any lzc error) ──

static bool s_lzcReady = false;

static bool lzcEnsureInit() {
    if (!s_lzcReady) {
        s_lzcReady = (libzfs_core_init() == 0);
    }
    return s_lzcReady;
}

static ExecResult lzcSnapshotOne(const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) { r.rc = -1; r.err = "libzfs_core_init failed\n"; return r; }
    nvlist_t* snaps = nullptr;
    if (nvlist_alloc(&snaps, NV_UNIQUE_NAME, 0) != 0) {
        r.rc = ENOMEM; r.err = "nvlist_alloc failed\n"; return r;
    }
    nvlist_add_boolean(snaps, snapName.c_str());
    nvlist_t* errlist = nullptr;
    r.rc = lzc_snapshot(snaps, nullptr, &errlist);
    nvlist_free(snaps);
    if (r.rc != 0) {
        r.err = std::string("cannot create snapshot '") + snapName + "': " + strerror(r.rc) + "\n";
        if (errlist) nvlist_free(errlist);
    }
    return r;
}

static ExecResult lzcDestroyOneSnap(const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) { r.rc = -1; r.err = "libzfs_core_init failed\n"; return r; }
    nvlist_t* snaps = nullptr;
    if (nvlist_alloc(&snaps, NV_UNIQUE_NAME, 0) != 0) {
        r.rc = ENOMEM; r.err = "nvlist_alloc failed\n"; return r;
    }
    nvlist_add_boolean(snaps, snapName.c_str());
    nvlist_t* errlist = nullptr;
    r.rc = lzc_destroy_snaps(snaps, B_FALSE, &errlist);
    nvlist_free(snaps);
    if (r.rc != 0) {
        r.err = std::string("cannot destroy '") + snapName + "': " + strerror(r.rc) + "\n";
        if (errlist) nvlist_free(errlist);
    }
    return r;
}

static ExecResult lzcRollbackTo(const std::string& dataset, const std::string& snapName) {
    ExecResult r;
    if (!lzcEnsureInit()) { r.rc = -1; r.err = "libzfs_core_init failed\n"; return r; }
    r.rc = lzc_rollback_to(dataset.c_str(), snapName.c_str());
    if (r.rc != 0) {
        r.err = std::string("cannot rollback '") + dataset + "' to '" + snapName + "': " + strerror(r.rc) + "\n";
    }
    return r;
}

static ExecResult lzcClone(const std::string& snapOrigin, const std::string& newDataset) {
    ExecResult r;
    if (!lzcEnsureInit()) { r.rc = -1; r.err = "libzfs_core_init failed\n"; return r; }
    r.rc = lzc_clone(newDataset.c_str(), snapOrigin.c_str(), nullptr);
    if (r.rc != 0) {
        r.err = std::string("cannot clone '") + snapOrigin + "' to '" + newDataset + "': " + strerror(r.rc) + "\n";
    }
    return r;
}

static bool lzcExists(const std::string& name) {
    return lzcEnsureInit() && lzc_exists(name.c_str()) != B_FALSE;
}
#endif // HAVE_LIBZFS_CORE

int runDumpRefreshBasics() {
    const std::string osLine = compactSpaces(detectOsLine());
    const std::string machineUuid = compactSpaces(detectMachineUuid());
    const std::string zraw = compactSpaces(detectZfsVersionRaw());
    std::cout << "OS_LINE=" << osLine << "\n";
    std::cout << "MACHINE_UUID=" << machineUuid << "\n";
    std::cout << "ZFS_VERSION_RAW=" << zraw << "\n";
    return 0;
}


// ── Generación del material TLS por el propio agente ─────────────────────────
// Antes lo hacía un openssl EXTERNO invocado por shell desde la GUI. En Windows eso
// no vale: openssl no está en el PATH (comprobado en un Windows 11 real; solo
// aparece si hay Git instalado, que no es garantía). El agente ya enlaza OpenSSL de
// forma estática, así que puede emitirlos él y el requisito desaparece en todas las
// plataformas.
//
// Los certificados llevan subjectAltName, keyUsage y extendedKeyUsage porque sin
// ellos el backend TLS de Apple los rechaza (ver el arreglo de la 0.90.3).
bool writeSelfSignedPair(const std::string& certPath,
                         const std::string& keyPath,
                         const std::string& commonName,
                         bool serverAuth,
                         std::string& errMsg) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) {
        errMsg = "no se pudo generar la clave RSA\n";
        return false;
    }
    X509* x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        errMsg = "no se pudo crear el certificado\n";
        return false;
    }
    X509_set_version(x, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(x, name);  // autofirmado: emisor = sujeto

    auto addExt = [&](int nid, const char* value) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (ex) {
            X509_add_ext(x, ex, -1);
            X509_EXTENSION_free(ex);
        }
    };
    addExt(NID_basic_constraints, "critical,CA:TRUE");
    addExt(NID_subject_alt_name,
           "DNS:zfsmgr-agent-server,DNS:zfsmgr-agent,DNS:localhost,IP:127.0.0.1");
    addExt(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
    addExt(NID_ext_key_usage, serverAuth ? "serverAuth" : "clientAuth");

    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x);
        EVP_PKEY_free(pkey);
        errMsg = "no se pudo firmar el certificado\n";
        return false;
    }

    bool ok = false;
    if (FILE* cf = std::fopen(certPath.c_str(), "wb")) {
        ok = PEM_write_X509(cf, x) == 1;
        std::fclose(cf);
    }
    if (ok) {
        ok = false;
        if (FILE* kf = std::fopen(keyPath.c_str(), "wb")) {
            ok = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            std::fclose(kf);
        }
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (!ok) {
        errMsg = "no se pudieron escribir los ficheros PEM\n";
        return false;
    }
#ifndef _WIN32
    ::chmod(certPath.c_str(), 0600);
    ::chmod(keyPath.c_str(), 0600);
#endif
    return true;
}

// Genera el material que falte. No toca lo existente salvo que carezca de SAN, misma
// regla que el bootstrap por shell, para que los hosts ya aprovisionados se corrijan.
ExecResult runEnsureTlsCapture() {
    ExecResult r;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kDefaultTlsDir, ec);

    const std::string serverCrt = kDefaultTlsCertPath;
    const std::string serverKey = kDefaultTlsKeyPath;
    const std::string clientCrt = kDefaultTlsClientCertPath;
    const std::string clientKey = std::string(kDefaultTlsDir) + kPathSep + "client.key";

    auto needsRegen = [](const std::string& certPath) {
        std::error_code e;
        if (!std::filesystem::exists(certPath, e) || std::filesystem::file_size(certPath, e) == 0) {
            return true;
        }
        FILE* f = std::fopen(certPath.c_str(), "rb");
        if (!f) {
            return true;
        }
        X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
        std::fclose(f);
        if (!x) {
            return true;
        }
        const int idx = X509_get_ext_by_NID(x, NID_subject_alt_name, -1);
        X509_free(x);
        return idx < 0;  // sin subjectAltName: se rehace
    };

    std::string err;
    if (needsRegen(serverCrt)
        && !writeSelfSignedPair(serverCrt, serverKey, "zfsmgr-agent-server", true, err)) {
        r.rc = 1;
        r.err = err;
        return r;
    }
    if (needsRegen(clientCrt)
        && !writeSelfSignedPair(clientCrt, clientKey, "zfsmgr-agent-client", false, err)) {
        r.rc = 1;
        r.err = err;
        return r;
    }
    r.rc = 0;
    r.out = "TLS_DIR=" + std::string(kDefaultTlsDir) + "\n";
    return r;
}

ExecResult runDumpRefreshBasicsCapture() {
    ExecResult r;
    const std::string osLine = compactSpaces(detectOsLine());
    const std::string machineUuid = compactSpaces(detectMachineUuid());
    const std::string zraw = compactSpaces(detectZfsVersionRaw());
    std::ostringstream ss;
    ss << "OS_LINE=" << osLine << "\n";
    ss << "MACHINE_UUID=" << machineUuid << "\n";
    ss << "ZFS_VERSION_RAW=" << zraw << "\n";
    r.rc = 0;
    r.out = ss.str();
    return r;
}

#ifdef _WIN32
// En Windows no hay gestores de paquetes que sondear ni rutas tipo /usr/local/bin: el
// agente resuelve por PATH, que él mismo fija en kDefaultCommandPath. Se compila la
// sonda igual que en Unix porque la pregunta es la misma —¿puede el agente ejecutar
// esta herramienta?— y sin ella la aplicación no puede decir por qué falla una acción.
bool isExecutableFile(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool lookupExecutable(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos) {
        return false;
    }
    const char* pathEnv = ::getenv("PATH");
    if (!pathEnv) {
        return false;
    }
    const std::string exts[] = {"", ".exe", ".cmd", ".bat"};
    std::string acc;
    std::istringstream ps(pathEnv);
    std::string dir;
    while (std::getline(ps, dir, ';')) {
        if (dir.empty()) {
            continue;
        }
        for (const std::string& ext : exts) {
            if (isExecutableFile(dir + "\\" + name + ext)) {
                return true;
            }
        }
    }
    return false;
}

ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    std::vector<std::string> wanted;
    if (!decodeBase64(payloadB64, decoded) || !parseJsonStringArray(decoded, wanted)) {
        r.rc = 2;
        r.err = "invalid tool-availability payload\n";
        return r;
    }
    std::ostringstream ss;
    for (const std::string& raw : wanted) {
        const std::string name = trim(raw);
        if (!name.empty()) {
            ss << (lookupExecutable(name) ? "OK:" : "KO:") << name << "\n";
        }
    }
    r.rc = 0;
    r.out = ss.str();
    return r;
}
#endif

#ifndef _WIN32
// Directories probed on top of $PATH, mirroring the search list the old shell
// loop prepended: sudo often runs with a trimmed PATH that hides /usr/local/zfs
// or Homebrew.
const char* const kToolSearchDirs[] = {"/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin",
                                       "/usr/local/sbin",   "/usr/local/zfs/bin", "/usr/sbin",
                                       "/sbin",             "/usr/bin",           "/bin"};

// Package managers reported alongside the commands, so a refresh needs one round
// trip instead of one per manager. Order and spelling must match what the GUI
// expects (it maps apt-get -> apt itself).
const char* const kProbedPackageManagers[] = {"apt-get", "pacman", "zypper", "brew", "pkg"};

bool isExecutableFile(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

// Equivalent of `command -v <name>`, without a shell. Names containing a slash
// are rejected rather than resolved: the caller asks for tools by bare name, and
// accepting a path would turn this into an arbitrary-file existence oracle.
bool lookupExecutable(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos) {
        return false;
    }
    if (const char* pathEnv = ::getenv("PATH")) {
        std::istringstream ps(pathEnv);
        std::string dir;
        while (std::getline(ps, dir, ':')) {
            if (!dir.empty() && isExecutableFile(dir + "/" + name)) {
                return true;
            }
        }
    }
    for (const char* dir : kToolSearchDirs) {
        if (isExecutableFile(std::string(dir) + "/" + name)) {
            return true;
        }
    }
    return false;
}

// Command availability probe. Payload is base64(JSON array) with the tool names
// to look for; output keeps the OK:/KO:/PM: line format the GUI already parses.
// This replaces a `for c in %1; do command -v ...` loop that interpolated the
// names straight into the remote shell.
ExecResult runDumpToolAvailabilityCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    std::vector<std::string> wanted;
    if (!decodeBase64(payloadB64, decoded) || !parseJsonStringArray(decoded, wanted)) {
        r.rc = 2;
        r.err = "invalid tool-availability payload\n";
        return r;
    }
    std::ostringstream ss;
    for (const std::string& raw : wanted) {
        const std::string name = trim(raw);
        if (name.empty()) {
            continue;
        }
        ss << (lookupExecutable(name) ? "OK:" : "KO:") << name << "\n";
    }
    for (const char* pm : kProbedPackageManagers) {
        ss << "PM:" << pm << ":" << (lookupExecutable(pm) ? "1" : "0") << "\n";
    }
    r.rc = 0;
    r.out = ss.str();
    return r;
}
#endif

int runDumpZpoolGuidStatusBatch() {
    ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc != 0) {
        if (!pools.out.empty()) {
            std::cout << pools.out;
        }
        if (!pools.err.empty()) {
            std::cerr << pools.err;
        }
        return pools.rc;
    }
    for (const std::string& raw : splitLines(pools.out)) {
        const std::string pool = trim(raw);
        if (pool.empty()) {
            continue;
        }
        ExecResult guid = runExecCapture("zpool", {"get", "-H", "-o", "value", "guid", pool});
        ExecResult status = runExecCapture("zpool", {"status", "-v", pool});
        std::cout << "__ZFSMGR_POOL__:" << pool << "\n";
        std::cout << "__ZFSMGR_GUID__:" << trim(splitLines(guid.out).empty() ? std::string() : splitLines(guid.out).front()) << "\n";
        std::cout << "__ZFSMGR_STATUS_BEGIN__\n";
        if (!status.out.empty()) {
            std::cout << status.out;
            if (status.out.back() != '\n') {
                std::cout << '\n';
            }
        }
        if (!status.err.empty()) {
            std::cout << status.err;
            if (status.err.back() != '\n') {
                std::cout << '\n';
            }
        }
        std::cout << "__ZFSMGR_STATUS_END__\n";
    }
    return 0;
}

ExecResult runDumpZpoolGuidStatusBatchCapture() {
    ExecResult r;
    ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc != 0) {
        return pools;
    }
    std::ostringstream out;
    std::ostringstream err;
    for (const std::string& raw : splitLines(pools.out)) {
        const std::string pool = trim(raw);
        if (pool.empty()) {
            continue;
        }
        ExecResult guid = runExecCapture("zpool", {"get", "-H", "-o", "value", "guid", pool});
        ExecResult status = runExecCapture("zpool", {"status", "-v", pool});
        out << "__ZFSMGR_POOL__:" << pool << "\n";
        const std::vector<std::string> guidLines = splitLines(guid.out);
        out << "__ZFSMGR_GUID__:" << trim(guidLines.empty() ? std::string() : guidLines.front()) << "\n";
        out << "__ZFSMGR_STATUS_BEGIN__\n";
        if (!status.out.empty()) {
            out << status.out;
            if (status.out.back() != '\n') {
                out << '\n';
            }
        }
        if (!status.err.empty()) {
            out << status.err;
            if (status.err.back() != '\n') {
                out << '\n';
            }
        }
        out << "__ZFSMGR_STATUS_END__\n";
        if (guid.rc != 0 || status.rc != 0) {
            r.rc = status.rc != 0 ? status.rc : guid.rc;
        }
    }
    r.out = out.str();
    r.err = err.str();
    if (r.rc == 1 && r.out.empty() && r.err.empty()) {
        r.rc = 0;
    }
    return r;
}

int runDumpGsaRawAllPools() {
    ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc != 0) {
        if (!pools.out.empty()) {
            std::cout << pools.out;
        }
        if (!pools.err.empty()) {
            std::cerr << pools.err;
        }
        return pools.rc;
    }

    for (const std::string& raw : splitLines(pools.out)) {
        const std::string pool = trim(raw);
        if (pool.empty()) {
            continue;
        }
        ExecResult one = runExecCapture(
            "zfs",
            {"get", "-H", "-o", "name,property,value", "-r",
             "org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino", pool});
        if (!one.out.empty()) {
            std::cout << one.out;
            if (one.out.back() != '\n') {
                std::cout << '\n';
            }
        }
        if (!one.err.empty()) {
            std::cerr << one.err;
            if (one.err.back() != '\n') {
                std::cerr << '\n';
            }
        }
    }
    return 0;
}

ExecResult runDumpGsaRawAllPoolsCapture() {
    ExecResult r;
    ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc != 0) {
        return pools;
    }
    std::ostringstream out;
    std::ostringstream err;
    int rc = 0;
    for (const std::string& raw : splitLines(pools.out)) {
        const std::string pool = trim(raw);
        if (pool.empty()) {
            continue;
        }
        ExecResult one = runExecCapture(
            "zfs",
            {"get", "-H", "-o", "name,property,value", "-r",
             "org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino", pool});
        if (!one.out.empty()) {
            out << one.out;
            if (one.out.back() != '\n') {
                out << '\n';
            }
        }
        if (!one.err.empty()) {
            err << one.err;
            if (one.err.back() != '\n') {
                err << '\n';
            }
        }
        if (one.rc != 0) {
            rc = one.rc;
        }
    }
    r.rc = rc;
    r.out = out.str();
    r.err = err.str();
    return r;
}

int runDumpGsaConnectionsConf() {
    std::ifstream f("/etc/zfsmgr/gsa-connections.conf");
    if (!f.is_open()) {
        return 0;
    }
    std::cout << f.rdbuf();
    return 0;
}

ExecResult runDumpGsaConnectionsConfCapture() {
    ExecResult r;
    std::ifstream f("/etc/zfsmgr/gsa-connections.conf");
    if (!f.is_open()) {
        r.rc = 0;
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    r.rc = 0;
    r.out = ss.str();
    return r;
}

// ── GSA scheduler ────────────────────────────────────────────────────────────

constexpr const char* kGsaConfPath         = "/etc/zfsmgr/gsa.conf";
constexpr const char* kGsaDefaultLogFile   = "/var/lib/zfsmgr/gsa.log";
constexpr const char* kGsaDefaultConnFile  = "/etc/zfsmgr/gsa-connections.conf";
constexpr long long   kGsaLogMaxBytes      = 1048576; // 1 MiB

using KVMap = std::unordered_map<std::string, std::string>;

bool gsaBoolOn(const std::string& s) {
    std::string lo = s;
    for (char& c : lo) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return lo == "on" || lo == "yes" || lo == "true" || lo == "1";
}

int gsaIntVal(const std::string& s) {
    if (s.empty()) return 0;
    for (unsigned char c : s) { if (!std::isdigit(c)) return 0; }
    try { return std::stoi(s); } catch (...) { return 0; }
}

// Reads KEY=VALUE pairs from a shell-sourceable conf file.
KVMap gsaReadKV(const std::string& path) {
    KVMap out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        out[trim(t.substr(0, eq))] = stripQuotes(t.substr(eq + 1));
    }
    return out;
}

struct GsaConf {
    std::string configDir;
    std::string logFile;
    std::string connectionsFile;
};

GsaConf gsaLoadConf() {
    GsaConf c;
    const KVMap kv = gsaReadKV(kGsaConfPath);
    auto get = [&](const char* k, const std::string& def) -> std::string {
        const auto it = kv.find(k);
        return (it != kv.end() && !it->second.empty()) ? it->second : def;
    };
    c.configDir       = get("CONFIG_DIR", "/var/lib/zfsmgr");
    c.logFile         = get("LOG_FILE", kGsaDefaultLogFile);
    c.connectionsFile = get("CONNECTIONS_FILE", kGsaDefaultConnFile);
    return c;
}

struct GsaTargetConn {
    std::string mode; // "local" or "ssh"
    std::string host;
    std::string user;
    std::string port;
    std::string key;
    std::string pass;
    bool useSudo{false};
    std::string knownHostsFile;
};

bool gsaResolveTarget(const KVMap& connKv, const std::string& connId,
                      const std::string& selfConn, GsaTargetConn& out) {
    if (connId == selfConn) {
        out.mode = "local";
        return true;
    }
    const std::string prefix = "connection_" + connId + "_";
    auto get = [&](const char* suffix) -> std::string {
        const auto it = connKv.find(prefix + suffix);
        return (it != connKv.end()) ? it->second : std::string();
    };
    const std::string mode = get("MODE");
    if (mode.empty()) return false;
    out.mode    = mode;
    out.host    = get("HOST");
    out.user    = get("USER");
    out.port    = get("PORT");
    out.key     = get("KEY");
    out.pass    = get("PASS");
    out.useSudo = gsaBoolOn(get("USE_SUDO"));
    const auto kh = connKv.find("KNOWN_HOSTS_FILE");
    if (kh != connKv.end()) out.knownHostsFile = kh->second;
    return !out.host.empty() && !out.user.empty();
}

void gsaRotateLog(const std::string& logFile) {
    std::ifstream f(logFile, std::ios::ate | std::ios::binary);
    if (!f.is_open()) return;
    const auto sz = static_cast<long long>(f.tellg());
    f.close();
    if (sz < kGsaLogMaxBytes) return;
    for (int i = 3; i >= 1; --i) {
        const std::string from = logFile + "." + std::to_string(i);
        const std::string to   = logFile + "." + std::to_string(i + 1);
        std::remove(to.c_str());
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(logFile.c_str(), (logFile + ".1").c_str());
}

// Returns a logger function that appends timestamped lines to the GSA log.
std::function<void(const std::string&)> gsaMakeLogger(const std::string& logFile) {
    return [logFile](const std::string& msg) {
        const std::string ts = utcNowIsoString();
        const std::string line = ts + " " + msg + "\n";
        std::ofstream f(logFile, std::ios::app);
        if (f.is_open()) f << line;
    };
}

// Returns "YYYYMMDD-HHMMSS" in local time.
std::string gsaLocalTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0) return "00000000-000000";
    return buf;
}

struct GsaTimeFields {
    int hour{0};  // 0-23
    int dow{0};   // 1=Mon … 7=Sun
    int dom{0};   // 1-31
    int month{0}; // 1-12
};

GsaTimeFields gsaLocalTimeFields() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    GsaTimeFields f;
    f.hour  = tm.tm_hour;
    f.dom   = tm.tm_mday;
    f.month = tm.tm_mon + 1;
    // tm_wday: 0=Sun … 6=Sat → convert to ISO 8601: 1=Mon … 7=Sun
    f.dow   = (tm.tm_wday == 0) ? 7 : tm.tm_wday;
    return f;
}

// Returns the snapshot classes that are due given the retention counts and time.
std::vector<std::string> gsaDueClasses(int hourly, int daily, int weekly,
                                       int monthly, int yearly,
                                       const GsaTimeFields& tf) {
    std::vector<std::string> due;
    if (hourly  > 0)                                          due.push_back("hourly");
    if (daily   > 0 && tf.hour == 0)                         due.push_back("daily");
    if (weekly  > 0 && tf.hour == 0 && tf.dow == 7)          due.push_back("weekly");
    if (monthly > 0 && tf.hour == 0 && tf.dom == 1)          due.push_back("monthly");
    if (yearly  > 0 && tf.hour == 0 && tf.dom == 1 && tf.month == 1) due.push_back("yearly");
    return due;
}

std::string gsaPropValue(const std::string& ds, const std::string& prop, bool localOnly) {
    std::vector<std::string> args = {"get", "-H", "-o", "value"};
    if (localOnly) { args.push_back("-s"); args.push_back("local"); }
    args.push_back(prop);
    args.push_back(ds);
    const ExecResult r = runExecCapture("zfs", args);
    if (r.rc != 0) return {};
    const std::string val = trim(r.out);
    return (val == "-") ? std::string() : val;
}

// Creates a snapshot; returns the snapshot short name on success.
std::string gsaCreateSnapshot(const std::string& ds, const std::string& klass,
                               bool recursive,
                               const std::function<void(const std::string&)>& log) {
    const std::string stamp    = gsaLocalTimestamp();
    const std::string snapName = "GSA-" + klass + "-" + stamp;
    const std::string full     = ds + "@" + snapName;
    log("GSA snapshot attempt for " + ds + ": class=" + klass
        + " recursive=" + (recursive ? "yes" : "no") + " snap=" + snapName);
    // Already exists?
    const ExecResult check = runExecCapture("zfs",
        {"list", "-H", "-t", "snapshot", "-o", "name", full});
    if (check.rc == 0 && trim(check.out).find(full) != std::string::npos) {
        log("GSA snapshot skip for " + ds + ": " + snapName + " ya existe");
        return snapName;
    }
    std::vector<std::string> args = {"snapshot"};
    if (recursive) args.push_back("-r");
    args.push_back(full);
    const ExecResult r = runExecCapture("zfs", args);
    if (r.rc != 0) {
        log("GSA snapshot error for " + ds + ": " + trim(r.err));
        return {};
    }
    log("GSA snapshot created for " + ds + ": " + snapName);
    return snapName;
}

void gsaPruneSnapshots(const std::string& ds, const std::string& klass,
                       int keep, bool recursive,
                       const std::function<void(const std::string&)>& log) {
    if (keep <= 0) return;
    const ExecResult lst = runExecCapture("zfs",
        {"list", "-H", "-t", "snapshot", "-o", "name", "-s", "creation", "-r", ds});
    if (lst.rc != 0) return;
    const std::string prefix = ds + "@GSA-" + klass + "-";
    std::vector<std::string> matching;
    for (const std::string& rawLine : splitLines(lst.out)) {
        const std::string line = trim(rawLine);
        if (line.empty()) continue;
        if (line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0)
            matching.push_back(line);
    }
    const int total = static_cast<int>(matching.size());
    if (total <= keep) return;
    const int removeCount = total - keep;
    for (int i = 0; i < removeCount; ++i) {
        const std::string& full = matching[static_cast<std::size_t>(i)];
        const std::string shortName = full.substr(ds.size() + 1); // after "@"
        std::vector<std::string> args = {"destroy"};
        if (recursive) args.push_back("-r");
        args.push_back(ds + "@" + shortName);
        const ExecResult r = runExecCapture("zfs", args);
        if (r.rc != 0)
            log("GSA prune error for " + ds + "@" + shortName + ": " + trim(r.err));
        else
            log("GSA pruned " + ds + "@" + shortName);
    }
}

// Run a command on the SSH target, return combined exit code.
int gsaRunViaSsh(const GsaTargetConn& tc, const std::string& remoteCmd) {
    std::vector<std::string> sshArgs;
    sshArgs.push_back("-o"); sshArgs.push_back("BatchMode=yes");
    sshArgs.push_back("-o"); sshArgs.push_back("ConnectTimeout=10");
    sshArgs.push_back("-o"); sshArgs.push_back("LogLevel=ERROR");
    if (!tc.knownHostsFile.empty()) {
        sshArgs.push_back("-o"); sshArgs.push_back("StrictHostKeyChecking=yes");
        sshArgs.push_back("-o"); sshArgs.push_back("UserKnownHostsFile=" + tc.knownHostsFile);
    }
    if (!tc.port.empty()) { sshArgs.push_back("-p"); sshArgs.push_back(tc.port); }
    if (!tc.key.empty())  { sshArgs.push_back("-i"); sshArgs.push_back(tc.key); }
    sshArgs.push_back(tc.user + "@" + tc.host);
    sshArgs.push_back(remoteCmd);
    const ExecResult r = runExecCapture("ssh", sshArgs);
    return r.rc;
}

void gsaLevelSnapshot(const std::string& ds, bool recursive,
                      const std::string& snapName, const std::string& dstSpec,
                      bool levelOn, const GsaTargetConn& target,
                      const std::function<void(const std::string&)>& log) {
    if (!levelOn || snapName.empty() || dstSpec.empty()) return;
    const std::size_t sep = dstSpec.find("::");
    if (sep == std::string::npos || sep == 0 || sep + 2 >= dstSpec.size()) {
        log("GSA level skip for " + ds + ": invalid destination " + dstSpec);
        return;
    }
    const std::string dstDataset = dstSpec.substr(sep + 2);
    if (dstDataset.empty()) {
        log("GSA level skip for " + ds + ": empty destination dataset in " + dstSpec);
        return;
    }

    const bool isLocal = (target.mode == "local");
    std::string recvOpts = "-u";
    std::string sendOpts = "-wLEc";
    if (recursive) { recvOpts = "-u -s"; sendOpts = "-wLEcR"; }

    // Check if destination dataset exists and get its latest snapshot.
    std::string baseSnap;
    bool targetExists = false;
    if (isLocal) {
        const ExecResult chk = runExecCapture("zfs",
            {"list", "-H", "-o", "name", dstDataset});
        targetExists = (chk.rc == 0);
        if (targetExists) {
            const ExecResult snaps = runExecCapture("zfs",
                {"list", "-H", "-t", "snapshot", "-o", "name", "-s", "creation", "-d", "1", dstDataset});
            if (snaps.rc == 0 && !trim(snaps.out).empty()) {
                const std::vector<std::string> lines = splitLines(snaps.out);
                for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                    const std::string l = trim(lines[static_cast<std::size_t>(i)]);
                    if (!l.empty()) { baseSnap = l.substr(dstDataset.size() + 1); break; }
                }
            }
        }
    } else {
        if (gsaRunViaSsh(target, "true") != 0) {
            log("GSA level skip for " + ds + ": SSH not available to " + target.host);
            return;
        }
        const int chkRc = gsaRunViaSsh(target,
            "zfs list -H -o name " + dstDataset + " >/dev/null 2>&1");
        targetExists = (chkRc == 0);
        if (targetExists) {
            std::vector<std::string> sshArgs;
            sshArgs.push_back("-o"); sshArgs.push_back("BatchMode=yes");
            sshArgs.push_back("-o"); sshArgs.push_back("ConnectTimeout=10");
            sshArgs.push_back("-o"); sshArgs.push_back("LogLevel=ERROR");
            if (!target.knownHostsFile.empty()) {
                sshArgs.push_back("-o"); sshArgs.push_back("StrictHostKeyChecking=yes");
                sshArgs.push_back("-o"); sshArgs.push_back("UserKnownHostsFile=" + target.knownHostsFile);
            }
            if (!target.port.empty()) { sshArgs.push_back("-p"); sshArgs.push_back(target.port); }
            if (!target.key.empty())  { sshArgs.push_back("-i"); sshArgs.push_back(target.key); }
            sshArgs.push_back(target.user + "@" + target.host);
            sshArgs.push_back("zfs list -H -t snapshot -o name -s creation -d 1 " + dstDataset + " 2>/dev/null");
            const ExecResult snaps = runExecCapture("ssh", sshArgs);
            if (snaps.rc == 0 && !trim(snaps.out).empty()) {
                const std::vector<std::string> lines = splitLines(snaps.out);
                for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                    const std::string l = trim(lines[static_cast<std::size_t>(i)]);
                    if (!l.empty()) { baseSnap = l.substr(dstDataset.size() + 1); break; }
                }
            }
        }
    }

    if (!baseSnap.empty()) {
        if (baseSnap.compare(0, 4, "GSA-") != 0) {
            log("GSA level error for " + ds + ": destination has manual snapshots (" + dstDataset + "@" + baseSnap + ")");
            return;
        }
        const ExecResult chkSrc = runExecCapture("zfs",
            {"list", "-H", "-t", "snapshot", "-o", "name", ds + "@" + baseSnap});
        if (chkSrc.rc != 0) {
            log("GSA level skip for " + ds + ": base snapshot " + baseSnap + " not in source");
            return;
        }
        recvOpts = isLocal ? "-u -F" : "-u -F";
        if (recursive) recvOpts = "-u -s -F";
    }

    std::string recvCmd;
    if (target.useSudo) {
        recvCmd = "sudo -n sh -lc 'zfs recv " + recvOpts + " " + dstDataset + "'";
    } else {
        recvCmd = "zfs recv " + recvOpts + " " + dstDataset;
    }

    std::string sendCmd = "zfs send " + sendOpts;
    if (!baseSnap.empty()) sendCmd += " -i @" + baseSnap;
    sendCmd += " " + ds + "@" + snapName;

    std::string fullCmd;
    if (isLocal) {
        fullCmd = sendCmd + " | sh -lc '" + recvCmd + "'";
    } else {
        std::string sshPrefix = "ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR";
        if (!target.knownHostsFile.empty())
            sshPrefix += " -o StrictHostKeyChecking=yes -o UserKnownHostsFile=" + target.knownHostsFile;
        if (!target.port.empty()) sshPrefix += " -p " + target.port;
        if (!target.key.empty())  sshPrefix += " -i " + target.key;
        sshPrefix += " " + target.user + "@" + target.host;
        fullCmd = sendCmd + " | " + sshPrefix + " " + recvCmd;
    }

    log("GSA level start for " + ds + " -> " + dstDataset);
    const int rc = std::system(fullCmd.c_str()); // NOLINT
    if (rc != 0)
        log("GSA level error for " + ds + ": exit " + std::to_string(rc));
    else
        log("GSA level done for " + ds + " -> " + dstDataset);
}

void gsaRunOnce(const std::function<void(const std::string&)>& log) {
    // Load GSA conf; skip silently if not configured.
    std::ifstream confCheck(kGsaConfPath);
    if (!confCheck.is_open()) return;
    confCheck.close();

    const GsaConf conf  = gsaLoadConf();
    const KVMap connKv  = gsaReadKV(conf.connectionsFile);
    const std::string selfConn = [&]() -> std::string {
        const auto it = connKv.find("SELF_CONNECTION");
        return (it != connKv.end()) ? it->second : "local";
    }();

    const ExecResult dsResult = runExecCapture("zfs", {"list", "-H", "-o", "name", "-t", "filesystem"});
    if (dsResult.rc != 0 || trim(dsResult.out).empty()) return;

    const GsaTimeFields tf = gsaLocalTimeFields();
    std::vector<std::string> processedRecursiveRoots;

    auto isDescendant = [](const std::string& ds, const std::string& root) {
        return ds.size() > root.size() + 1
               && ds.compare(0, root.size(), root) == 0
               && ds[root.size()] == '/';
    };

    for (const std::string& rawLine : splitLines(dsResult.out)) {
        if (g_stop.load()) break;
        const std::string ds = trim(rawLine);
        if (ds.empty()) continue;

        const std::string enabledVal = gsaPropValue(ds, "org.fc16.gsa:activado", /*local=*/true);
        if (!gsaBoolOn(enabledVal)) continue;

        // Skip if already covered by a recursive ancestor.
        bool coveredByRoot = false;
        for (const std::string& root : processedRecursiveRoots) {
            if (isDescendant(ds, root)) { coveredByRoot = true; break; }
        }
        if (coveredByRoot) continue;

        // Skip if a recursive GSA ancestor exists.
        bool hasRecursiveAncestor = false;
        std::string probe = ds;
        while (true) {
            const std::size_t slash = probe.rfind('/');
            if (slash == std::string::npos) break;
            probe = probe.substr(0, slash);
            if (gsaBoolOn(gsaPropValue(probe, "org.fc16.gsa:activado", /*local=*/true))
                && gsaBoolOn(gsaPropValue(probe, "org.fc16.gsa:recursivo", /*local=*/false))) {
                hasRecursiveAncestor = true;
                break;
            }
        }
        if (hasRecursiveAncestor) continue;

        const bool recursive = gsaBoolOn(gsaPropValue(ds, "org.fc16.gsa:recursivo", false));
        const int hourly     = gsaIntVal(gsaPropValue(ds, "org.fc16.gsa:horario",   false));
        const int daily      = gsaIntVal(gsaPropValue(ds, "org.fc16.gsa:diario",    false));
        const int weekly     = gsaIntVal(gsaPropValue(ds, "org.fc16.gsa:semanal",   false));
        const int monthly    = gsaIntVal(gsaPropValue(ds, "org.fc16.gsa:mensual",   false));
        const int yearly     = gsaIntVal(gsaPropValue(ds, "org.fc16.gsa:anual",     false));
        const bool levelOn   = gsaBoolOn(gsaPropValue(ds, "org.fc16.gsa:nivelar",  false));
        const std::string dstSpec = gsaPropValue(ds, "org.fc16.gsa:destino",        false);

        const std::vector<std::string> due = gsaDueClasses(hourly, daily, weekly, monthly, yearly, tf);
        {
            std::string dueStr;
            for (const std::string& c : due) { if (!dueStr.empty()) dueStr += ','; dueStr += c; }
            log("GSA evaluate " + ds + ": recursive=" + (recursive ? "yes" : "no")
                + " hourly=" + std::to_string(hourly) + " daily=" + std::to_string(daily)
                + " weekly=" + std::to_string(weekly) + " monthly=" + std::to_string(monthly)
                + " yearly=" + std::to_string(yearly) + " due=" + dueStr);
        }
        if (due.empty()) continue;

        // Resolve destination connection once per dataset.
        GsaTargetConn target;
        bool targetResolved = false;
        if (levelOn && !dstSpec.empty()) {
            const std::size_t sep = dstSpec.find("::");
            if (sep != std::string::npos) {
                const std::string connId = dstSpec.substr(0, sep);
                targetResolved = gsaResolveTarget(connKv, connId, selfConn, target);
                if (!targetResolved)
                    log("GSA level skip for " + ds + ": connection not resolvable (" + connId + ")");
            }
        }

        for (const std::string& klass : due) {
            if (g_stop.load()) break;
            const std::string snapName = gsaCreateSnapshot(ds, klass, recursive, log);
            if (snapName.empty()) continue;
            const int keep = (klass == "hourly") ? hourly
                           : (klass == "daily")   ? daily
                           : (klass == "weekly")  ? weekly
                           : (klass == "monthly") ? monthly
                           : (klass == "yearly")  ? yearly : 0;
            gsaPruneSnapshots(ds, klass, keep, recursive, log);
            if (levelOn && targetResolved)
                gsaLevelSnapshot(ds, recursive, snapName, dstSpec, true, target, log);
        }

        if (recursive) processedRecursiveRoots.push_back(ds);
    }
}

void runGsaSchedulerThread() {
    // Wait until just after the next hour boundary, then run each hour.
    while (!g_stop.load()) {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        // Seconds remaining until next :00:00
        const int secsUntilHour = 3600 - (tm.tm_min * 60 + tm.tm_sec);
        // Wait in 1-second increments so we can respond to g_stop.
        for (int i = 0; i < secsUntilHour && !g_stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_stop.load()) break;

        const GsaConf conf = gsaLoadConf();
        gsaRotateLog(conf.logFile);
        const auto log = gsaMakeLogger(conf.logFile);
        log("GSA scheduler wake version " ZFSMGR_AGENT_VERSION_STRING);
        try {
            gsaRunOnce(log);
        } catch (...) {
            log("GSA scheduler: uncaught exception in gsaRunOnce");
        }
        log("GSA scheduler done");
    }
}


#ifndef _WIN32
static ExecResult runZfsSendToPeerCapture(const std::vector<std::string>& params) {
    ExecResult r;
    // params: snap peer_host peer_port token [base_snap [send_flags]]
    if (params.size() < 4) {
        r.rc = 2;
        r.err = "usage: zfsmgr-agent --zfs-send-to-peer <snap> <peer_host> <peer_port> <token> [<base_snap> [<send_flags>]]\n";
        return r;
    }
    const std::string snap     = params[0];
    const std::string peerHost = params[1];
    int peerPort = 0;
    try { peerPort = std::stoi(params[2]); } catch (...) {}
    const std::string token    = params[3];
    const std::string baseSnap = (params.size() > 4 && !params[4].empty()) ? params[4] : "";
    const std::string flagsStr = params.size() > 5 ? params[5] : "";
    if (snap.empty() || peerHost.empty() || peerPort <= 0 || token.size() != 64) {
        r.rc = 2; r.err = "invalid arguments for --zfs-send-to-peer\n"; return r;
    }
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(peerPort);
    if (getaddrinfo(peerHost.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        r.rc = 1; r.err = "cannot resolve peer host: " + peerHost + "\n"; return r;
    }
    int sockFd = -1;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) { sockFd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (sockFd < 0) { r.rc = 1; r.err = "cannot connect to peer " + peerHost + ":" + portStr + "\n"; return r; }
    const std::string tokenLine = token + "\n";
    (void)write(sockFd, tokenLine.c_str(), tokenLine.size());
    std::vector<std::string> sendArgs = {"send"};
    if (!flagsStr.empty()) {
        std::istringstream iss(flagsStr);
        std::string tok;
        while (iss >> tok) sendArgs.push_back(tok);
    }
    if (!baseSnap.empty()) { sendArgs.push_back("-I"); sendArgs.push_back(baseSnap); }
    sendArgs.push_back(snap);
    std::vector<char*> argv2;
    argv2.push_back(const_cast<char*>("zfs"));
    for (const std::string& a : sendArgs) argv2.push_back(const_cast<char*>(a.c_str()));
    argv2.push_back(nullptr);
    int pipeFds[2];
    if (pipe(pipeFds) != 0) { close(sockFd); r.rc = 1; r.err = "pipe() failed\n"; return r; }
    const pid_t sendPid = fork();
    if (sendPid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);
        close(sockFd);
        execvp("zfs", argv2.data());
        _exit(127);
    }
    close(pipeFds[1]);
    if (sendPid < 0) { close(pipeFds[0]); close(sockFd); r.rc = 1; r.err = "fork() failed\n"; return r; }
    uint64_t totalBytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastReport = t0;
    char buf[65536];
    bool relayOk = true;
    for (;;) {
        const ssize_t n = read(pipeFds[0], buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t done = 0;
        while (done < n) {
            const ssize_t w = write(sockFd, buf + done, static_cast<size_t>(n - done));
            if (w <= 0) { relayOk = false; break; }
            done += w;
        }
        if (!relayOk) break;
        totalBytes += static_cast<uint64_t>(n);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 2) {
            const long elapsed = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
            const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
            const double rate = elapsed > 0 ? mib / static_cast<double>(elapsed) : 0.0;
            char line[256];
            snprintf(line, sizeof(line), "BYTES=%llu  %.1f MiB  @ %.1f MiB/s  elapsed %lds\n",
                     (unsigned long long)totalBytes, mib, rate, elapsed);
            (void)write(STDOUT_FILENO, line, strlen(line));
            lastReport = now;
        }
    }
    close(pipeFds[0]);
    close(sockFd);
    {
        const auto now = std::chrono::steady_clock::now();
        const long elapsed = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
        const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
        const double rate = elapsed > 0 ? mib / static_cast<double>(elapsed) : mib;
        char line[256];
        snprintf(line, sizeof(line), "BYTES=%llu  %.1f MiB  @ %.1f MiB/s  elapsed %lds [done]\n",
                 (unsigned long long)totalBytes, mib, rate, elapsed);
        (void)write(STDOUT_FILENO, line, strlen(line));
    }
    int status = 0;
    if (sendPid > 0) waitpid(sendPid, &status, 0);
    r.rc = decodeWaitStatus(status);
    if (r.rc != 0) r.err = "zfs send failed (exit " + std::to_string(r.rc) + ")\n";
    else if (!relayOk) { r.rc = 1; r.err = "relay write to peer failed\n"; }
    return r;
}

// Async variant: runs the same relay loop in a background thread started by
// the --zfs-send-to-peer-async RPC handler. Updates g_jobs[jobId] in place.
static void runZfsSendToPeerAsync(const std::string& jobId) {
    std::string snap, peerHost, baseSnap, sendFlags, token;
    int peerPort = 0;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) return;
        snap      = it->second.snap;
        peerHost  = it->second.peerHost;
        peerPort  = it->second.peerPort;
        token     = it->second.token;
        baseSnap  = it->second.baseSnap;
        sendFlags = it->second.sendFlags;
    }

    auto setFailed = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) return;
        it->second.state        = JobState::Failed;
        it->second.errorText    = msg;
        it->second.finishedAtUtc = utcNowIsoString();
        persistJobsLocked();
    };

    // Resolve and connect
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string portStr = std::to_string(peerPort);
    if (getaddrinfo(peerHost.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        setFailed("cannot resolve peer host: " + peerHost); return;
    }
    int sockFd = -1;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) { sockFd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (sockFd < 0) { setFailed("cannot connect to peer " + peerHost + ":" + portStr); return; }

    const std::string tokenLine = token + "\n";
    (void)write(sockFd, tokenLine.c_str(), tokenLine.size());

    // Build zfs send argv
    std::vector<std::string> sendArgs = {"send"};
    if (!sendFlags.empty()) {
        std::istringstream iss(sendFlags);
        std::string tok;
        while (iss >> tok) sendArgs.push_back(tok);
    }
    if (!baseSnap.empty()) { sendArgs.push_back("-I"); sendArgs.push_back(baseSnap); }
    sendArgs.push_back(snap);
    std::vector<char*> argv2;
    argv2.push_back(const_cast<char*>("zfs"));
    for (const std::string& a : sendArgs) argv2.push_back(const_cast<char*>(a.c_str()));
    argv2.push_back(nullptr);

    int pipeFds[2];
    if (pipe(pipeFds) != 0) { close(sockFd); setFailed("pipe() failed"); return; }
    const pid_t sendPid = fork();
    if (sendPid < 0) { close(pipeFds[0]); close(pipeFds[1]); close(sockFd); setFailed("fork() failed"); return; }
    if (sendPid == 0) {
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);
        close(sockFd);
        execvp("zfs", argv2.data());
        _exit(127);
    }
    close(pipeFds[1]);

    // Store PID immediately so --job-cancel can kill it
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(jobId);
        if (it != g_jobs.end()) it->second.sendPid = sendPid;
    }

    uint64_t totalBytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastReport = t0;
    char buf[65536];
    bool relayOk = true;

    for (;;) {
        // Check for cancellation
        {
            std::lock_guard<std::mutex> lock(g_jobsMutex);
            auto it = g_jobs.find(jobId);
            if (it != g_jobs.end() && it->second.state == JobState::Cancelled) break;
        }
        const ssize_t n = read(pipeFds[0], buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t done = 0;
        while (done < n) {
            const ssize_t w = write(sockFd, buf + done, static_cast<size_t>(n - done));
            if (w <= 0) { relayOk = false; break; }
            done += w;
        }
        if (!relayOk) break;
        totalBytes += static_cast<uint64_t>(n);

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 2) {
            const long elapsed = static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
            const double mib  = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
            const double rate = elapsed > 0 ? mib / static_cast<double>(elapsed) : 0.0;
            char line[256];
            snprintf(line, sizeof(line), "BYTES=%llu  %.1f MiB  @ %.1f MiB/s  elapsed %lds",
                     (unsigned long long)totalBytes, mib, rate, elapsed);
            {
                std::lock_guard<std::mutex> lock(g_jobsMutex);
                auto it = g_jobs.find(jobId);
                if (it != g_jobs.end()) {
                    it->second.bytesTransferred = totalBytes;
                    it->second.rateMiBs         = rate;
                    it->second.elapsedSecs      = elapsed;
                    it->second.progressLines.push_back(line);
                    if (it->second.progressLines.size() > 5)
                        it->second.progressLines.erase(it->second.progressLines.begin());
                }
            }
            daemonLog("DEBUG", std::string("job ") + jobId + " " + line);
            lastReport = now;
        }
    }
    close(pipeFds[0]);
    close(sockFd);

    int status = 0;
    if (sendPid > 0) waitpid(sendPid, &status, 0);
    const int rc = decodeWaitStatus(status);

    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(jobId);
    if (it == g_jobs.end()) return;
    if (it->second.state == JobState::Cancelled) {
        // already marked cancelled by --job-cancel handler
    } else if (rc != 0) {
        it->second.state     = JobState::Failed;
        it->second.errorText = "zfs send exited " + std::to_string(rc);
    } else if (!relayOk) {
        it->second.state     = JobState::Failed;
        it->second.errorText = "relay write to peer failed";
    } else {
        it->second.state = JobState::Done;
    }
    it->second.bytesTransferred = totalBytes;
    it->second.finishedAtUtc    = utcNowIsoString();
    daemonLog("INFO", "job " + jobId + " finished state=" + (it->second.errorText.empty() ? "done" : "failed"));
    persistJobsLocked();
}


#endif // _WIN32

ExecResult executeAgentCommandCapture(const std::string& cmd,
                                      const std::vector<std::string>& params,
                                      const char* argv0) {
    ExecResult r;
    if (cmd == "--health") {
        r.rc = 0;
        r.out =
            "STATUS=OK\n"
            "VERSION=" ZFSMGR_AGENT_VERSION_STRING "\n"
            "API=3\n"
            "SERVER=1\n"
            "CACHE_ENTRIES=0\n"
            "CACHE_MAX_ENTRIES=0\n"
            "CACHE_INVALIDATIONS=0\n"
            "POOL_INVALIDATIONS=0\n"
            "RPC_FAILURES=0\n"
            "RPC_COMMANDS=\n"
            "ZED_ACTIVE=0\n";
        r.out += "CAPS=" + agentCapabilityList() + "\n";
        return r;
    }
    if (cmd == "--dump-refresh-basics") {
        return runDumpRefreshBasicsCapture();
    }
    if (cmd == "--dump-tool-availability") {
        if (params.size() < 1) {
            r.rc = 2;
            r.err = std::string("usage: ") + argv0 + " --dump-tool-availability <payload-b64>\n";
            return r;
        }
        return runDumpToolAvailabilityCapture(params[0]);
    }
    if (cmd == "--dump-zfs-version") {
        r = runExecCapture("zfs", {"version"});
        if (r.rc != 0 || trim(r.out).empty()) {
            r = runExecCapture("zfs", {"--version"});
        }
        if (r.rc != 0 || trim(r.out + "\n" + r.err).empty()) {
            r = runExecCapture("zpool", {"--version"});
        }
        return r;
    }
    if (cmd == "--dump-zfs-mount") return runExecCapture("zfs", {"mount", "-j"});
    if (cmd == "--dump-zpool-list") return runExecCapture("zpool", {"list", "-j"});
    if (cmd == "--dump-zpool-import-probe") {
        ExecResult a = runExecCapture("zpool", {"import"});
        ExecResult b = runExecCapture("zpool", {"import", "-s"});
        r.rc = b.rc;
        r.out = a.out + b.out;
        r.err = a.err + b.err;
        return r;
    }
    if (cmd == "--dump-zpool-guid-status-batch") return runDumpZpoolGuidStatusBatchCapture();
    if (cmd == "--dump-zpool-guid") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zpool-guid <pool>\n"; return r; }
        return runExecCapture("zpool", {"get", "-H", "-o", "value", "guid", params[0]});
    }
    if (cmd == "--dump-zpool-status") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zpool-status <pool>\n"; return r; }
        return runExecCapture("zpool", {"status", "-v", params[0]});
    }
    if (cmd == "--dump-zpool-status-p") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zpool-status-p <pool>\n"; return r; }
        return runExecCapture("zpool", {"status", "-P", params[0]});
    }
    if (cmd == "--dump-zpool-history") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zpool-history <pool>\n"; return r; }
        return runExecCapture("zpool", {"history", params[0]});
    }
    if (cmd == "--dump-zpool-get-all") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zpool-get-all <pool>\n"; return r; }
        return runExecCapture("zpool", {"get", "-j", "all", params[0]});
    }
    if (cmd == "--dump-zfs-list-all") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-list-all <dataset>\n"; return r; }
        return runExecCapture("zfs", {"list", "-H", "-p", "-t", "filesystem,volume,snapshot", "-o",
                                      "name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount",
                                      "-r", params[0]});
    }
    if (cmd == "--dump-zfs-guid-map") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-guid-map <dataset>\n"; return r; }
        return runExecCapture("zfs", {"get", "-H", "-o", "name,value", "guid", "-r", params[0]});
    }
    if (cmd == "--dump-zfs-list-children") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-list-children <dataset>\n"; return r; }
        return runExecCapture("zfs", {"list", "-H", "-o", "name", "-r", params[0]});
    }
    if (cmd == "--dump-advanced-breakdown-list") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-advanced-breakdown-list <dataset>\n"; return r; }
        return runDumpAdvancedBreakdownListCapture(params[0]);
    }
    if (cmd == "--dump-zfs-get-prop") {
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-get-prop <prop> <dataset>\n"; return r; }
        return runExecCapture("zfs", {"get", "-H", "-o", "value", params[0], params[1]});
    }
    if (cmd == "--dump-zfs-get-all") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-get-all <dataset>\n"; return r; }
        return runExecCapture("zfs", {"get", "-j", "all", params[0]});
    }
    if (cmd == "--dump-zfs-get-json") {
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-get-json <props> <dataset>\n"; return r; }
        return runExecCapture("zfs", {"get", "-j", params[0], params[1]});
    }
    if (cmd == "--dump-zfs-diff") {
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-diff <snap1> <snap2>\n"; return r; }
        // -H: salida tabulada de verdad. Sin él, zfs imprime los renombrados como
        // "viejo -> nuevo" en un solo campo, y el parseo de la GUI —escrito para
        // campos separados por tabulador— se quedaba sin la ruta anterior.
        return runExecCapture("zfs", {"diff", "-H", params[0], params[1]});
    }
    if (cmd == "--dump-zfs-allow") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-allow <dataset>\n"; return r; }
        return runExecCapture("zfs", {"allow", params[0]});
    }
    if (cmd == "--dump-zfs-allow-batch") {
        if (params.empty()) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-allow-batch <ds1> [ds2...]\n"; return r; }
        r.rc = 0;
        for (const auto& ds : params) {
            r.out += "__ZFSMGR_ALLOW_BEGIN__ " + ds + "\n";
            ExecResult res = runExecCapture("zfs", {"allow", ds});
            r.out += res.out;
            if (!res.err.empty()) r.out += res.err;
            r.out += "__ZFSMGR_ALLOW_RC__ " + ds + " " + std::to_string(res.rc) + "\n";
            r.out += "__ZFSMGR_ALLOW_END__ " + ds + "\n";
            if (res.rc != 0) r.rc = res.rc;
        }
        return r;
    }
    if (cmd == "--dump-zfs-get-gsa-raw-all-pools") return runDumpGsaRawAllPoolsCapture();
    if (cmd == "--dump-zfs-get-gsa-raw-recursive") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-get-gsa-raw-recursive <dataset>\n"; return r; }
        return runExecCapture(
            "zfs",
            {"get", "-H", "-o", "name,property,value,source", "-r",
             "org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino",
             params[0]});
    }
    if (cmd == "--dump-gsa-connections-conf") return runDumpGsaConnectionsConfCapture();
    if (cmd == "--dump-gsa-log") {
        const GsaConf conf = gsaLoadConf();
        ExecResult lr;
        std::ifstream f(conf.logFile);
        if (!f.is_open()) { lr.rc = 0; return lr; }
        std::ostringstream ss; ss << f.rdbuf();
        lr.rc = 0; lr.out = ss.str();
        return lr;
    }

    if (cmd == "--mutate-zfs-snapshot") {
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-snapshot <target> <recursive>\n"; return r; }
        const bool recursive = (params[1] == "1" || toLower(params[1]) == "true" || toLower(params[1]) == "on");
#ifdef HAVE_LIBZFS_CORE
        if (!recursive) {
            ExecResult lr = lzcSnapshotOne(params[0]);
            if (lr.rc == 0) return lr;
        }
#endif
        std::vector<std::string> a = {"snapshot"};
        if (recursive) a.push_back("-r");
        a.push_back(params[0]);
        return runExecCapture("zfs", a);
    }
    if (cmd == "--mutate-zfs-destroy") {
        if (params.size() < 3) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-destroy <target> <force> <scope>\n"; return r; }
        const bool force = (params[1] == "1" || toLower(params[1]) == "true" || toLower(params[1]) == "on");
        const std::string& scope = params[2];
        const bool isSnap = params[0].find('@') != std::string::npos;
        const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
        // lzc_destroy_snaps handles single snapshots without scope/force cleanly
        if (isSnap && noScope && !force) {
            ExecResult lr = lzcDestroyOneSnap(params[0]);
            if (lr.rc == 0) return lr;
        }
#endif
        std::vector<std::string> a = {"destroy"};
        if (force) a.push_back("-f");
        if (scope == "R") a.push_back("-R");
        else if (scope == "r") a.push_back("-r");
        a.push_back(params[0]);
        return runExecCapture("zfs", a);
    }
    if (cmd == "--mutate-zfs-rollback") {
        if (params.size() < 3) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-rollback <snap> <force> <scope>\n"; return r; }
        const bool force = (params[1] == "1" || toLower(params[1]) == "true" || toLower(params[1]) == "on");
        const std::string& scope = params[2];
        const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
        if (!force && noScope) {
            const auto atPos = params[0].find('@');
            if (atPos != std::string::npos) {
                ExecResult lr = lzcRollbackTo(params[0].substr(0, atPos), params[0]);
                if (lr.rc == 0) return lr;
            }
        }
#endif
        std::vector<std::string> a = {"rollback"};
        if (force) a.push_back("-f");
        if (scope == "R") a.push_back("-R");
        else if (scope == "r") a.push_back("-r");
        a.push_back(params[0]);
        return runExecCapture("zfs", a);
    }
    if (cmd == "--dump-zfs-exists") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --dump-zfs-exists <dataset>\n"; return r; }
#ifdef HAVE_LIBZFS_CORE
        const bool exists = lzcExists(params[0]);
        r.rc = exists ? 0 : 1;
        r.out = exists ? "EXISTS=yes\n" : "EXISTS=no\n";
        return r;
#endif
        ExecResult sub = runExecCapture("zfs", {"list", "-H", "-o", "name", params[0]});
        r.rc = (sub.rc == 0) ? 0 : 1;
        r.out = (sub.rc == 0) ? "EXISTS=yes\n" : "EXISTS=no\n";
        return r;
    }
    if (cmd == "--mutate-zfs-clone") {
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-clone <origin-snap> <new-dataset>\n"; return r; }
#ifdef HAVE_LIBZFS_CORE
        ExecResult lr = lzcClone(params[0], params[1]);
        if (lr.rc == 0) return lr;
#endif
        return runExecCapture("zfs", {"clone", params[0], params[1]});
    }
    if (cmd == "--mutate-zfs-load-key") {
#ifndef _WIN32
        if (params.size() < 2) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-load-key <dataset-b64> <passphrase-b64>\n"; return r; }
        std::string dataset, passphrase;
        if (!decodeBase64(params[0], dataset) || !decodeBase64(params[1], passphrase)) {
            r.rc = 2; r.err = "invalid base64 argument\n"; return r;
        }
        dataset = trim(dataset);
        if (dataset.empty()) { r.rc = 2; r.err = "empty dataset\n"; return r; }
        return runExecCaptureWithStdin("zfs", {"load-key", dataset}, passphrase + "\n");
#else
        r.rc = 2; r.err = "not supported on Windows\n"; return r;
#endif
    }
    if (cmd == "--mutate-zfs-change-key") {
#ifndef _WIN32
        if (params.size() < 3) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-change-key <dataset-b64> <passphrase-b64> <flags-b64>\n"; return r; }
        std::string dataset, passphrase, flagsStr;
        if (!decodeBase64(params[0], dataset) || !decodeBase64(params[1], passphrase) || !decodeBase64(params[2], flagsStr)) {
            r.rc = 2; r.err = "invalid base64 argument\n"; return r;
        }
        dataset = trim(dataset);
        flagsStr = trim(flagsStr);
        if (dataset.empty()) { r.rc = 2; r.err = "empty dataset\n"; return r; }
        std::vector<std::string> changeArgs = {"change-key"};
        {
            std::istringstream iss(flagsStr);
            std::string tok;
            while (iss >> tok) changeArgs.push_back(tok);
        }
        changeArgs.push_back(dataset);
        return runExecCaptureWithStdin("zfs", changeArgs, passphrase + "\n" + passphrase + "\n");
#else
        r.rc = 2; r.err = "not supported on Windows\n"; return r;
#endif
    }
    if (cmd == "--mutate-zfs-generic") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-generic <payload-b64>\n"; return r; }
        return runGenericMutationCapture("zfs", params[0]);
    }
    if (cmd == "--mutate-advanced-breakdown") {
        if (params.size() < 2) {
            r.rc = 2;
            r.err = std::string("usage: ") + argv0 + " --mutate-advanced-breakdown <dataset> <dir> [dir...]\n";
            return r;
        }
        return runMutateAdvancedBreakdownCapture(params);
    }
    if (cmd == "--mutate-advanced-assemble") {
        if (params.size() < 2) {
            r.rc = 2;
            r.err = std::string("usage: ") + argv0 + " --mutate-advanced-assemble <dataset> <child> [child...]\n";
            return r;
        }
        return runMutateAdvancedAssembleCapture(params);
    }
#ifndef _WIN32
    // todir usa AltMountGuard, que solo existe en Unix. En Windows la GUI no pasa
    // por el agente para esta acción: usa su propio script de PowerShell.
    if (cmd == "--mutate-advanced-todir") {
        if (params.size() < 3) {
            r.rc = 2;
            r.err = std::string("usage: ") + argv0 + " --mutate-advanced-todir <dataset> <dst-dir> <delete-source-0|1>\n";
            return r;
        }
        return runMutateAdvancedToDirCapture(params);
    }
#endif
    if (cmd == "--mutate-zpool-generic") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zpool-generic <payload-b64>\n"; return r; }
        return runGenericMutationCapture("zpool", params[0]);
    }
    if (cmd == "--mutate-zfs-allow-batch") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-zfs-allow-batch <payload-b64>\n"; return r; }
        return runZfsAllowBatchCapture(params[0]);
    }
    if (cmd == "--mutate-rsync-local") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --mutate-rsync-local <payload-b64>\n"; return r; }
        return runRsyncLocalCapture(params[0]);
    }
    // --mutate-shell-generic is deliberately NOT served over RPC: it runs an arbitrary
    // shell as root, so exposing it here would let any client holding the mTLS
    // certificate bypass every typed --mutate-* whitelist. The GUI only ever invokes
    // it as a CLI command over SSH+sudo (see daemonizeShellMutationArgs), where the
    // caller already has root and it grants no extra privilege. The CLI handler in
    // main() stays for those flows.

#ifndef _WIN32
    if (cmd == "--zfs-pipe-local") {
        if (params.size() < 1) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --zfs-pipe-local <payload-b64>\n"; return r; }
        return runZfsPipeLocalCapture(params[0]);
    }
    if (cmd == "--repair-alt-mountpoints") {
        // Fully typed and argument-free apart from the apply switch: it only ever acts
        // on datasets carrying our own savedmountpoint property, so it is safe to
        // serve over RPC.
        const bool apply = !params.empty() && toLower(trim(params[0])) == "apply";
        return runRepairAltMountpointsCapture(apply);
    }
    if (cmd == "--zfs-recv-listen") {
        // params: dataset [force=0|1]
        if (params.empty()) { r.rc = 2; r.err = std::string("usage: ") + argv0 + " --zfs-recv-listen <dataset> [force=1]\n"; return r; }
        const std::string dataset = params[0];
        const bool force = params.size() > 1 && (params[1] == "1" || toLower(params[1]) == "true");

        // Generate 32-byte (64-hex-char) random token
        auto genToken = []() -> std::string {
            unsigned char buf[32] = {};
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd >= 0) { (void)read(fd, buf, sizeof(buf)); close(fd); }
            else { for (int i = 0; i < 32; ++i) buf[i] = static_cast<unsigned char>(std::rand() & 0xff); }
            static const char hex[] = "0123456789abcdef";
            std::string out; out.reserve(64);
            for (int i = 0; i < 32; ++i) {
                out.push_back(hex[(buf[i] >> 4) & 0xf]);
                out.push_back(hex[buf[i] & 0xf]);
            }
            return out;
        };
        const std::string token = genToken();

        // Open ephemeral TCP listener
        const AgentRuntimeConfig cfg2 = loadRuntimeConfig();
        const std::string bindAddr = cfg2.transferBindAddr.empty() ? std::string("0.0.0.0") : cfg2.transferBindAddr;
        struct addrinfo hints2{};
        hints2.ai_family = AF_INET;
        hints2.ai_socktype = SOCK_STREAM;
        hints2.ai_flags = AI_PASSIVE;
        struct addrinfo* res2 = nullptr;
        if (getaddrinfo(bindAddr.c_str(), nullptr, &hints2, &res2) != 0 || !res2) {
            r.rc = 1; r.err = "getaddrinfo failed for transfer bind\n"; return r;
        }
        int listenFd = socket(res2->ai_family, res2->ai_socktype, res2->ai_protocol);
        if (listenFd < 0) { freeaddrinfo(res2); r.rc = 1; r.err = "socket failed\n"; return r; }
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listenFd, res2->ai_addr, static_cast<socklen_t>(res2->ai_addrlen)) != 0) {
            close(listenFd); freeaddrinfo(res2); r.rc = 1; r.err = "bind failed\n"; return r;
        }
        freeaddrinfo(res2);
        if (listen(listenFd, 1) != 0) {
            close(listenFd); r.rc = 1; r.err = "listen failed\n"; return r;
        }
        // Determine actual port
        struct sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        getsockname(listenFd, reinterpret_cast<struct sockaddr*>(&bound), &blen);
        const int assignedPort = static_cast<int>(ntohs(bound.sin_port));

        // Spawn background thread: accept one connection, verify token, pipe to zfs recv
        struct RecvState { int listenFd; std::string token; std::string dataset; bool force; };
        auto* state = new RecvState{listenFd, token, dataset, force};
        std::thread([state]() {
            struct RecvState s = *state;
            delete state;
            // Wait up to 300 seconds for source to connect
            struct timeval tv{}; tv.tv_sec = 300;
            fd_set rfds; FD_ZERO(&rfds); FD_SET(s.listenFd, &rfds);
            const int sel = select(s.listenFd + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) { close(s.listenFd); return; }
            const int clientFd = accept(s.listenFd, nullptr, nullptr);
            close(s.listenFd);
            if (clientFd < 0) return;
            // Read token line (64 hex chars + '\n')
            std::string recvToken;
            recvToken.reserve(65);
            char ch = 0;
            while (recvToken.size() < 65) {
                const ssize_t n = read(clientFd, &ch, 1);
                if (n <= 0) break;
                if (ch == '\n') break;
                recvToken.push_back(ch);
            }
            if (recvToken != s.token) { close(clientFd); return; }
            // Fork zfs recv with stdin from clientFd
            std::vector<std::string> args = {"recv"};
            if (s.force) args.push_back("-F");
            args.push_back("-u");
            args.push_back("-s");
            args.push_back(s.dataset);
            std::vector<char*> argv2;
            argv2.push_back(const_cast<char*>("zfs"));
            for (const std::string& a : args) argv2.push_back(const_cast<char*>(a.c_str()));
            argv2.push_back(nullptr);
            const pid_t pid = fork();
            if (pid == 0) {
                dup2(clientFd, STDIN_FILENO);
                close(clientFd);
                execvp("zfs", argv2.data());
                _exit(127);
            }
            close(clientFd);
            if (pid > 0) waitpid(pid, nullptr, 0);
        }).detach();

        r.rc = 0;
        r.out = "PORT=" + std::to_string(assignedPort) + "\nTOKEN=" + token + "\n";
        return r;
    }

    if (cmd == "--zfs-send-to-peer") {
        return runZfsSendToPeerCapture(params);
    }
#endif

    r.rc = 2;
    r.err = std::string("unknown command: ") + cmd + "\n";
    return r;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

// ── Job persistence ──────────────────────────────────────────────────────────

static const char* jobStateName(JobState s) {
    switch (s) {
    case JobState::Queued:    return "queued";
    case JobState::Running:   return "running";
    case JobState::Done:      return "done";
    case JobState::Failed:    return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

static JobState jobStateFromName(const std::string& s) {
    if (s == "queued")    return JobState::Queued;
    if (s == "running")   return JobState::Running;
    if (s == "done")      return JobState::Done;
    if (s == "cancelled") return JobState::Cancelled;
    return JobState::Failed;
}

// Called under g_jobsMutex. Serialises g_jobs to kJobsFilePath.
// Retains at most 20 jobs; prunes oldest terminal (Done/Failed/Cancelled) entries first.
static void persistJobsLocked() {
    // Collect ordered by startedAtUtc for pruning
    std::vector<std::pair<std::string, const DaemonJob*>> all;
    all.reserve(g_jobs.size());
    for (const auto& kv : g_jobs) all.push_back({kv.first, &kv.second});

    // Prune old terminal jobs beyond 20 total
    while (all.size() > 20) {
        // find oldest terminal
        auto oldest = all.end();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const JobState st = it->second->state;
            if (st == JobState::Done || st == JobState::Failed || st == JobState::Cancelled) {
                if (oldest == all.end() || it->second->startedAtUtc < oldest->second->startedAtUtc)
                    oldest = it;
            }
        }
        if (oldest == all.end()) break;
        g_jobs.erase(oldest->first);
        all.erase(oldest);
    }

    std::ofstream f(kJobsFilePath, std::ios::trunc);
    if (!f.is_open()) return;
    f << "[\n";
    bool first = true;
    for (const auto& kv : g_jobs) {
        const DaemonJob& j = kv.second;
        if (!first) f << ",\n";
        first = false;
        f << "{\"id\":\"" << jsonEscape(j.id)
          << "\",\"type\":\"" << jsonEscape(j.type)
          << "\",\"state\":\"" << jobStateName(j.state)
          << "\",\"snap\":\"" << jsonEscape(j.snap)
          << "\",\"peerHost\":\"" << jsonEscape(j.peerHost)
          << "\",\"peerPort\":" << j.peerPort
          << ",\"baseSnap\":\"" << jsonEscape(j.baseSnap)
          << "\",\"sendFlags\":\"" << jsonEscape(j.sendFlags)
          << "\",\"pipeCmd\":\"" << jsonEscape(j.pipeCmd)
          << "\",\"dstDataset\":\"" << jsonEscape(j.dstDataset)
          << "\",\"bytes\":" << j.bytesTransferred
          << ",\"rateMiBs\":" << j.rateMiBs
          << ",\"elapsedSecs\":" << j.elapsedSecs
          << ",\"startedAt\":\"" << jsonEscape(j.startedAtUtc)
          << "\",\"finishedAt\":\"" << jsonEscape(j.finishedAtUtc)
          << "\",\"error\":\"" << jsonEscape(j.errorText)
          << "\"}";
    }
    f << "\n]\n";
}

// Called once at daemon startup, before the accept loop. Reads persisted jobs;
// any job left in Running state is marked Failed (daemon restarted mid-transfer).
static void loadPersistedJobsAtStartup() {
    std::ifstream f(kJobsFilePath, std::ios::binary);
    if (!f.is_open()) return;
    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    // Minimal JSON array parser: extract objects via key=value text matching.
    // We look for repeated {...} blobs and parse known keys with simple search.
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    std::string::size_type pos = 0;
    while (true) {
        const auto ob = raw.find('{', pos);
        if (ob == std::string::npos) break;
        const auto cb = raw.find('}', ob);
        if (cb == std::string::npos) break;
        const std::string blob = raw.substr(ob, cb - ob + 1);
        pos = cb + 1;

        auto getStr = [&](const std::string& key) -> std::string {
            const std::string pat = "\"" + key + "\":\"";
            auto p = blob.find(pat);
            if (p == std::string::npos) return {};
            p += pat.size();
            auto q = blob.find('"', p);
            if (q == std::string::npos) return {};
            return blob.substr(p, q - p);
        };
        auto getNum = [&](const std::string& key) -> long long {
            const std::string pat = "\"" + key + "\":";
            auto p = blob.find(pat);
            if (p == std::string::npos) return 0;
            p += pat.size();
            try { return std::stoll(blob.substr(p)); } catch (...) { return 0; }
        };

        DaemonJob j;
        j.id            = getStr("id");
        j.type          = getStr("type");
        j.state         = jobStateFromName(getStr("state"));
        j.snap          = getStr("snap");
        j.peerHost      = getStr("peerHost");
        j.peerPort      = static_cast<int>(getNum("peerPort"));
        j.baseSnap      = getStr("baseSnap");
        j.sendFlags     = getStr("sendFlags");
        j.pipeCmd       = getStr("pipeCmd");
        j.dstDataset    = getStr("dstDataset");
        j.bytesTransferred = static_cast<uint64_t>(getNum("bytes"));
        j.rateMiBs      = static_cast<double>(getNum("rateMiBs"));
        j.elapsedSecs   = static_cast<long>(getNum("elapsedSecs"));
        j.startedAtUtc  = getStr("startedAt");
        j.finishedAtUtc = getStr("finishedAt");
        j.errorText     = getStr("error");

        if (j.id.empty()) continue;

        // Jobs still Running when the daemon stopped can never be recovered
        if (j.state == JobState::Running || j.state == JobState::Queued) {
            j.state        = JobState::Failed;
            j.errorText    = "daemon restarted while running";
            j.finishedAtUtc = utcNowIsoString();
        }
        g_jobs[j.id] = std::move(j);
    }
}

bool parseJsonStringAt(const std::string& s, std::size_t& i, std::string& out) {
    out.clear();
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (i >= s.size()) {
                return false;
            }
            const char e = s[i++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (i + 4 > s.size()) return false;
                i += 4;
                out.push_back('?');
                break;
            default:
                return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool parseJsonRpcRequest(const std::string& line, std::string& cmd, std::vector<std::string>& args) {
    cmd.clear();
    args.clear();
    std::size_t i = 0;
    auto skipWs = [&]() {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
    };
    skipWs();
    if (i >= line.size() || line[i] != '{') return false;
    ++i;
    while (i < line.size()) {
        skipWs();
        if (i < line.size() && line[i] == '}') {
            ++i;
            break;
        }
        std::string key;
        if (!parseJsonStringAt(line, i, key)) return false;
        skipWs();
        if (i >= line.size() || line[i] != ':') return false;
        ++i;
        skipWs();
        if (key == "cmd") {
            if (!parseJsonStringAt(line, i, cmd)) return false;
        } else if (key == "args") {
            std::size_t arrStart = i;
            int depth = 0;
            bool inString = false;
            bool escape = false;
            for (; i < line.size(); ++i) {
                const char ch = line[i];
                if (inString) {
                    if (escape) {
                        escape = false;
                    } else if (ch == '\\') {
                        escape = true;
                    } else if (ch == '"') {
                        inString = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    inString = true;
                    continue;
                }
                if (ch == '[') {
                    ++depth;
                } else if (ch == ']') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            if (i > line.size()) return false;
            if (arrStart >= line.size()) return false;
            if (!parseJsonStringArray(line.substr(arrStart, i - arrStart), args)) return false;
        } else {
            // Consumir valor genérico simple para mantener tolerancia.
            if (i < line.size() && line[i] == '"') {
                std::string tmp;
                if (!parseJsonStringAt(line, i, tmp)) return false;
            } else {
                while (i < line.size() && line[i] != ',' && line[i] != '}') ++i;
            }
        }
        skipWs();
        if (i < line.size() && line[i] == ',') {
            ++i;
            continue;
        }
        if (i < line.size() && line[i] == '}') {
            ++i;
            break;
        }
    }
    return !cmd.empty();
}

bool isDumpCommand(const std::string& cmd) {
    return startsWith(cmd, "--dump-");
}

bool isMutateCommand(const std::string& cmd) {
    return startsWith(cmd, "--mutate-");
}

std::string makeRpcCacheKey(const std::string& cmd, const std::vector<std::string>& args) {
    std::string key = cmd;
    key.push_back('\n');
    for (const std::string& a : args) {
        key += a;
        key.push_back('\n');
    }
    return key;
}

std::string dumpClassForCommand(const std::string& cmd) {
    if (cmd == "--dump-zfs-get-gsa-raw-all-pools"
        || cmd == "--dump-zfs-get-gsa-raw-recursive"
        || cmd == "--dump-gsa-connections-conf"
        || cmd == "--dump-gsa-log") {
        return "gsa";
    }
    if (startsWith(cmd, "--dump-zfs-")) {
        return "zfs";
    }
    if (startsWith(cmd, "--dump-zpool-")) {
        return "zpool";
    }
    if (cmd == "--dump-refresh-basics" || cmd == "--dump-tool-availability") {
        return "system";
    }
    return "other";
}

// ── Long mutations as background jobs ────────────────────────────────────────
// These operations copy real data and routinely outlast any RPC read timeout. Run
// synchronously they forced the client to wait, and a timeout there is ambiguous:
// the daemon keeps working, so the client cannot safely retry. Submitting them as
// jobs makes the RPC answer immediate and turns "did it finish?" into a question
// with an actual answer (--job-status).
bool isAsyncSubmittableCommand(const std::string& cmd) {
    return cmd == "--mutate-advanced-breakdown"
           || cmd == "--mutate-advanced-assemble"
           || cmd == "--mutate-advanced-todir"
           || cmd == "--mutate-rsync-local";
}

void runSubmittedMutationJob(const std::string& jobId,
                             const std::string& cmd,
                             const std::vector<std::string>& params) {
    const ExecResult r = executeAgentCommandCapture(cmd, params, "zfsmgr-agent");
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(jobId);
    if (it == g_jobs.end()) {
        return;
    }
    DaemonJob& j = it->second;
    if (j.state != JobState::Cancelled) {
        j.state = (r.rc == 0) ? JobState::Done : JobState::Failed;
    }
    j.resultRc = r.rc;
    j.resultOut = r.out;
    if (r.rc != 0 && j.errorText.empty()) {
        j.errorText = r.err.empty() ? ("exit " + std::to_string(r.rc)) : r.err;
    }
    j.finishedAtUtc = utcNowIsoString();
    persistJobsLocked();
    daemonLog("INFO", "job " + jobId + " finished rc=" + std::to_string(r.rc) + " cmd=" + cmd);
}

int runServeLoop() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 2;
#endif
    const AgentRuntimeConfig cfg = loadRuntimeConfig();
    applyRuntimeEnvironment(cfg);
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cfg.tlsCert.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, cfg.tlsKey.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(ctx) != 1
        || SSL_CTX_load_verify_locations(ctx, cfg.tlsClientCert.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);

    NativeSock listenFd = kInvalidSock;
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = nullptr;
    const std::string bindHost = cfg.bind.empty() ? std::string(kDefaultBind) : cfg.bind;
    const std::string bindPort = std::to_string(cfg.port > 0 ? cfg.port : kDefaultPort);
    if (getaddrinfo(bindHost.c_str(), bindPort.c_str(), &hints, &res) != 0) {
        SSL_CTX_free(ctx);
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        const NativeSock fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSock) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        if (bind(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0
            && listen(fd, 32) == 0) {
            listenFd = fd;
#ifndef _WIN32
            // Keep the listening socket out of child processes: the ZED watcher runs
            // `zpool events` through popen(), which would otherwise inherit this fd.
            // A surviving child then keeps the port bound after the daemon exits, and
            // the next start fails to bind for no visible reason.
            const int fdFlags = fcntl(fd, F_GETFD, 0);
            if (fdFlags >= 0) {
                fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
            }
#endif
            break;
        }
        closeSock(fd);
    }
    freeaddrinfo(res);
    if (listenFd == kInvalidSock) {
        SSL_CTX_free(ctx);
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifndef _WIN32
    std::signal(SIGHUP, onSignal);
#endif
    loadPersistedJobsAtStartup();
    daemonLog("INFO", std::string("daemon started version=") + ZFSMGR_AGENT_VERSION_STRING
                      + " api=" + kApiVersion
                      + " bind=" + bindHost + ":" + bindPort);
    auto lastHeartbeat = std::chrono::steady_clock::now() - std::chrono::minutes(2);
    struct CacheEntry {
        ExecResult result;
        std::chrono::steady_clock::time_point ts;
        std::string cmd;
        std::vector<std::string> args;
        std::string dumpClass;
    };
    std::unordered_map<std::string, CacheEntry> rpcCache;
    std::mutex runtimeMutex;
    std::set<std::string> rpcCommands;
    std::atomic<long long> rpcFailures{0};
    std::atomic<long long> cacheInvalidations{0};
    std::atomic<long long> poolInvalidations{0};
    std::atomic<long long> zedRestarts{0};
    std::atomic<bool> zedActive{false};
    std::atomic<bool> zedInvalidateRequested{false};
    std::atomic<long long> zedEventSeq{0}; // monotonically increments on each ZED event
    std::string zedLastEventUtc;

    std::thread zedThread([&]() {
#ifndef _WIN32
        // Tree-modifying operations we care about (from history_internal_name).
        static const std::set<std::string> kTreeOps = {
            "snapshot", "destroy", "create", "clone",
            "rename",   "rollback", "receive", "hold", "release",
        };

        while (!g_stop.load()) {
            // stdbuf -oL forces line-buffered output so each event flushes immediately
            // when stdout is a pipe (default full-buffering hides events until 4-8KB fills).
            FILE* fp = popen("PATH=\"$PATH:/sbin:/usr/sbin:/usr/local/sbin:/usr/local/bin\""
                             " stdbuf -oL zpool events -f -H -v 2>/dev/null"
                             " || zpool events -f -H -v 2>/dev/null", "r");
            if (!fp) {
                zedActive.store(false);
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            // Phase 1: drain historical events that arrive immediately after popen.
            // zpool events dumps the full event log on start — we skip this initial
            // burst by reading with a short select() timeout. Once no data arrives
            // within 300 ms, the historical flush is considered complete.
            {
                int fd = fileno(fp);
                char drainBuf[4096];
                bool eof = false;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                while (!eof && !g_stop.load()) {
                    auto rem = std::chrono::duration_cast<std::chrono::microseconds>(
                        deadline - std::chrono::steady_clock::now()).count();
                    if (rem <= 0) break;
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(fd, &rfds);
                    struct timeval tv;
                    tv.tv_sec = 0;
                    tv.tv_usec = static_cast<suseconds_t>(std::min<long long>(rem, 100000));
                    const int ready = select(fd + 1, &rfds, nullptr, nullptr, &tv);
                    if (ready <= 0) break; // timeout — no more immediate data
                    if (!fgets(drainBuf, sizeof(drainBuf), fp)) { eof = true; }
                    // reset deadline on each line received
                    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                }
                if (eof) {
                    pclose(fp);
                    zedActive.store(false);
                    if (!g_stop.load()) {
                        zedRestarts.fetch_add(1);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    continue;
                }
            }

            zedActive.store(true);
            daemonLog("INFO", "ZED thread active (zpool events running)");

            // Phase 2: block-read new events and fire for relevant ones.
            // Header lines start with any non-whitespace character (format varies
            // by platform: ISO "2026-..." or locale "Apr 25 2026...").
            // Class is always the last tab-separated field on the header line.
            std::string evClass, evOp;
            bool evClassRelevant = false;
            // Debounce: coalesce bursts (e.g. "zfs snap -r" fires one event per
            // child dataset). Fire at most once per 2 seconds.
            // Initialize far enough in the past so the first event always fires.
            // Do NOT use time_point::min() — subtracting it from now() wraps to negative.
            auto lastFireTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);

            auto isHistoryClass = [](const std::string& cls) {
                return cls == "history_event" || cls == "sysevent.fs.zfs.history_event";
            };

            auto fireIfRelevant = [&]() {
                if (evClass.empty()) return;
                bool fire = false;
                if (evClass.compare(0, 24, "sysevent.fs.zfs.dataset_") == 0
                    || evClass.compare(0, 25, "sysevent.fs.zfs.snapshot_") == 0) {
                    fire = true;
                } else if (isHistoryClass(evClass) && kTreeOps.count(evOp) > 0) {
                    fire = true;
                }
                if (fire) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastFireTime < std::chrono::seconds(2)) return;
                    lastFireTime = now;
                    zedInvalidateRequested.store(true);
                    zedEventSeq.fetch_add(1);
                    const std::string nowUtc = utcNowIsoString();
                    if (!nowUtc.empty()) {
                        std::lock_guard<std::mutex> lock(runtimeMutex);
                        zedLastEventUtc = nowUtc;
                    }
                    daemonLog("INFO", "ZED event cls=" + evClass + " op=" + evOp + " ts=" + nowUtc);
                }
            };

            char line[4096];
            while (!g_stop.load()) {
                if (!fgets(line, sizeof(line), fp)) break;
                std::string s(line);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                // Blank line = event separator: fire now rather than waiting for next
                // event's header (which may never arrive, e.g. last event in stream).
                if (s.empty()) {
                    fireIfRelevant();
                    evClass.clear();
                    evOp.clear();
                    evClassRelevant = false;
                    continue;
                }

                if (!std::isspace(static_cast<unsigned char>(s[0]))) {
                    // New event header — also fire for any preceding event not yet
                    // fired (guards against streams without blank line separators).
                    fireIfRelevant();
                    evClass.clear();
                    evOp.clear();
                    evClassRelevant = false;
                    // Class is the last tab-separated token (works for both ISO
                    // "TS\tCLASS" and locale "Apr DD YYYY HH:MM:SS\tCLASS" formats).
                    const size_t tab = s.rfind('\t');
                    evClass = (tab != std::string::npos) ? s.substr(tab + 1) : "";
                    while (!evClass.empty()
                           && (evClass.back() == '\r' || evClass.back() == ' ')) {
                        evClass.pop_back();
                    }
                    evClassRelevant = isHistoryClass(evClass)
                                   || (evClass.compare(0, 24, "sysevent.fs.zfs.dataset_") == 0)
                                   || (evClass.compare(0, 25, "sysevent.fs.zfs.snapshot_") == 0);
                } else if (evClassRelevant && isHistoryClass(evClass) && evOp.empty()
                           && s.find("history_internal_name") != std::string::npos) {
                    // Extract the operation name from the quoted value.
                    const size_t q1 = s.find('"');
                    const size_t q2 = (q1 != std::string::npos)
                                          ? s.find('"', q1 + 1)
                                          : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        evOp = s.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
            fireIfRelevant();

            pclose(fp);
            evClass.clear();
            evOp.clear();
            evClassRelevant = false;
            zedActive.store(false);
            if (!g_stop.load()) {
                zedRestarts.fetch_add(1);
                daemonLog("WARN", "ZED thread restart (zpool events exited)");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
#endif
    });

    std::thread gsaThread(runGsaSchedulerThread);

    while (!g_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(60)) {
            writeHeartbeat();
            lastHeartbeat = now;
        }
        if (zedInvalidateRequested.exchange(false)) {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            if (!rpcCache.empty()) {
                cacheInvalidations.fetch_add(1);
            }
            rpcCache.clear();
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenFd, &rfds);
        struct timeval tv {};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        const int sel = select(static_cast<int>(listenFd + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0 || !FD_ISSET(listenFd, &rfds)) {
            continue;
        }
        const NativeSock clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd == kInvalidSock) {
            continue;
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            closeSock(clientFd);
            continue;
        }
        SSL_set_fd(ssl, static_cast<int>(clientFd));
        if (SSL_accept(ssl) != 1) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            closeSock(clientFd);
            continue;
        }

        std::string reqLine;
        reqLine.reserve(1024);
        char buf[2048];
        bool gotLine = false;
        while (reqLine.size() < 1024 * 1024) {
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)));
            if (n <= 0) {
                break;
            }
            reqLine.append(buf, static_cast<std::size_t>(n));
            const std::size_t nl = reqLine.find('\n');
            if (nl != std::string::npos) {
                reqLine = reqLine.substr(0, nl);
                gotLine = true;
                break;
            }
        }

        std::string cmd;
        std::vector<std::string> rpcArgs;
        ExecResult exec;
        if (!gotLine || !parseJsonRpcRequest(reqLine, cmd, rpcArgs)) {
            exec.rc = 2;
            exec.err = "invalid rpc request\n";
            rpcFailures.fetch_add(1);
        } else {
            {
                std::lock_guard<std::mutex> lock(runtimeMutex);
                rpcCommands.insert(cmd);
            }
            if (cmd == "--heartbeat") {
                const std::string ts = utcNowIsoString();
                daemonLog("INFO", "heartbeat via RPC");
                std::ostringstream bs;
                bs << "ALIVE=yes\n";
                bs << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
                bs << "TS=" << ts << "\n";
                exec.rc = 0;
                exec.out = bs.str();
            } else if (cmd == "--dump-daemon-log") {
                long long offset = 0;
                long long maxBytes = 0;
                if (!rpcArgs.empty()) {
                    try { offset = std::stoll(rpcArgs[0]); } catch (...) {}
                }
                if (rpcArgs.size() > 1) {
                    try { maxBytes = std::stoll(rpcArgs[1]); } catch (...) {}
                }
                exec.rc = 0;
                exec.out = readDaemonLogRange(offset, maxBytes);
            } else if (cmd == "--health") {
                long long cacheEntries = 0;
                std::string commandList;
                std::string lastEvent;
                daemonLog("DEBUG", "healthcheck via RPC");
                {
                    std::lock_guard<std::mutex> lock(runtimeMutex);
                    cacheEntries = static_cast<long long>(rpcCache.size());
                    bool first = true;
                    for (const std::string& c : rpcCommands) {
                        if (!first) {
                            commandList.push_back(',');
                        }
                        commandList += c;
                        first = false;
                    }
                    lastEvent = zedLastEventUtc;
                }
                std::ostringstream hs;
                hs << "STATUS=OK\n";
                hs << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
                hs << "API=" << kApiVersion << "\n";
                hs << "SERVER=1\n";
                hs << "CACHE_ENTRIES=" << cacheEntries << "\n";
                hs << "CACHE_MAX_ENTRIES=" << cfg.cacheMaxEntries << "\n";
                hs << "CACHE_INVALIDATIONS=" << cacheInvalidations.load() << "\n";
                hs << "POOL_INVALIDATIONS=" << poolInvalidations.load() << "\n";
                hs << "RPC_FAILURES=" << rpcFailures.load() << "\n";
                hs << "RPC_COMMANDS=" << commandList << "\n";
                hs << "ZED_ACTIVE=" << (zedActive.load() ? 1 : 0) << "\n";
                hs << "ZED_RESTARTS=" << zedRestarts.load() << "\n";
                if (!lastEvent.empty()) {
                    hs << "ZED_LAST_EVENT_UTC=" << lastEvent << "\n";
                }
#ifndef _WIN32
                hs << "JOBS_SUPPORT=1\n";
#endif
                hs << "CAPS=" << agentCapabilityList() << "\n";
                exec.rc = 0;
                exec.out = hs.str();
            } else if (cmd == "--wait-for-event") {
                int timeoutSecs = 30;
                if (!rpcArgs.empty()) {
                    try { timeoutSecs = std::stoi(rpcArgs[0]); } catch (...) {}
                }
                timeoutSecs = std::max(1, std::min(timeoutSecs, 120));
                const long long initialSeq = zedEventSeq.load();
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSecs);
                while (std::chrono::steady_clock::now() < deadline && !g_stop.load()) {
                    if (zedEventSeq.load() != initialSeq) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if (zedEventSeq.load() != initialSeq) {
                    std::string lastEvent;
                    {
                        std::lock_guard<std::mutex> lock(runtimeMutex);
                        lastEvent = zedLastEventUtc;
                    }
                    std::ostringstream ws;
                    ws << "EVENT_TYPE=zed\n";
                    if (!lastEvent.empty()) {
                        ws << "EVENT_AT=" << lastEvent << "\n";
                    }
                    exec.rc = 0;
                    exec.out = ws.str();
                } else {
                    exec.rc = 1;
                    exec.out = "TIMEOUT=1\n";
                }
#ifndef _WIN32
            } else if (cmd == "--zfs-send-to-peer-async") {
                // args: snap peerHost peerPort token [baseSnap [sendFlags]]
                if (rpcArgs.size() < 4) {
                    exec.rc = 1;
                    exec.err = "usage: --zfs-send-to-peer-async <snap> <peerHost> <peerPort> <token> [<baseSnap> [<sendFlags>]]\n";
                } else {
                    DaemonJob job;
                    // Generate a 16-hex job ID from /dev/urandom
                    {
                        unsigned char rnd[8] = {};
                        int urfd = open("/dev/urandom", O_RDONLY);
                        if (urfd >= 0) { (void)read(urfd, rnd, sizeof(rnd)); close(urfd); }
                        static const char hx[] = "0123456789abcdef";
                        for (int i = 0; i < 8; ++i) {
                            job.id.push_back(hx[(rnd[i] >> 4) & 0xf]);
                            job.id.push_back(hx[rnd[i] & 0xf]);
                        }
                    }
                    job.type      = "send-to-peer";
                    job.snap      = rpcArgs[0];
                    job.peerHost  = rpcArgs[1];
                    try { job.peerPort = std::stoi(rpcArgs[2]); } catch (...) {}
                    job.token     = rpcArgs[3];
                    job.baseSnap  = (rpcArgs.size() > 4) ? rpcArgs[4] : "";
                    job.sendFlags = (rpcArgs.size() > 5) ? rpcArgs[5] : "";
                    job.state     = JobState::Running;
                    job.startedAtUtc = utcNowIsoString();
                    const std::string jobId = job.id;
                    {
                        std::lock_guard<std::mutex> jlock(g_jobsMutex);
                        g_jobs[jobId] = job;
                        persistJobsLocked();
                    }
                    daemonLog("INFO", "job " + jobId + " started type=send-to-peer snap=" + job.snap);
                    std::thread([jobId]() { runZfsSendToPeerAsync(jobId); }).detach();
                    exec.rc  = 0;
                    exec.out = "JOB_ID=" + jobId + "\nSTATE=running\n";
                }
            } else if (cmd == "--job-submit") {
                if (rpcArgs.empty() || !isAsyncSubmittableCommand(rpcArgs[0])) {
                    exec.rc = 2;
                    exec.err = "usage: --job-submit <async-submittable-mutation> [args...]\n";
                } else {
                    DaemonJob job;
                    {
                        unsigned char rnd[8] = {};
                        int urfd = open("/dev/urandom", O_RDONLY);
                        if (urfd >= 0) { (void)read(urfd, rnd, sizeof(rnd)); close(urfd); }
                        static const char hx[] = "0123456789abcdef";
                        for (int i = 0; i < 8; ++i) {
                            job.id.push_back(hx[(rnd[i] >> 4) & 0xf]);
                            job.id.push_back(hx[rnd[i] & 0xf]);
                        }
                    }
                    job.type = "mutation";
                    job.submittedCmd = rpcArgs[0];
                    job.state = JobState::Running;
                    job.startedAtUtc = utcNowIsoString();
                    const std::string jobId = job.id;
                    const std::string subCmd = rpcArgs[0];
                    const std::vector<std::string> subParams(rpcArgs.begin() + 1, rpcArgs.end());
                    {
                        std::lock_guard<std::mutex> jlock(g_jobsMutex);
                        g_jobs[jobId] = job;
                        persistJobsLocked();
                    }
                    daemonLog("INFO", "job " + jobId + " started type=mutation cmd=" + subCmd);
                    std::thread([jobId, subCmd, subParams]() {
                        // Declarar el trabajo del hilo para que la operación pueda
                        // informar de su avance con reportJobProgress().
                        t_currentJobId = jobId;
                        runSubmittedMutationJob(jobId, subCmd, subParams);
                        t_currentJobId.clear();
                    }).detach();
                    exec.rc = 0;
                    exec.out = "JOB_ID=" + jobId + "\nSTATE=running\n";
                }
            } else if (cmd == "--job-status") {
                if (rpcArgs.empty()) {
                    exec.rc = 1; exec.err = "usage: --job-status <jobId>\n";
                } else {
                    std::lock_guard<std::mutex> jlock(g_jobsMutex);
                    auto it = g_jobs.find(rpcArgs[0]);
                    if (it == g_jobs.end()) {
                        exec.rc = 1; exec.out = "ERROR=job not found\n";
                    } else {
                        const DaemonJob& j = it->second;
                        std::ostringstream js;
                        js << "JOB_ID=" << j.id << "\n"
                           << "STATE=" << jobStateName(j.state) << "\n"
                           << "BYTES=" << j.bytesTransferred << "\n"
                           << "RATE_MIB_S=" << j.rateMiBs << "\n"
                           << "ELAPSED_SECS=" << j.elapsedSecs << "\n"
                           << "STARTED_AT=" << j.startedAtUtc << "\n"
                           << "FINISHED_AT=" << j.finishedAtUtc << "\n"
                           << "ERROR=" << j.errorText << "\n";
                        if (!j.progressLines.empty())
                            js << "PROGRESS_LINE=" << j.progressLines.back() << "\n";
                        if (j.type == "mutation") {
                            js << "CMD=" << j.submittedCmd << "\n"
                               << "RC=" << j.resultRc << "\n";
                            // The transport is line based, so fold the captured output
                            // onto a single line rather than truncating it.
                            std::string flat = j.resultOut;
                            for (char& ch : flat) {
                                if (ch == '\n' || ch == '\r') {
                                    ch = ' ';
                                }
                            }
                            js << "OUT=" << trim(flat) << "\n";
                        }
                        exec.rc = 0; exec.out = js.str();
                    }
                }
            } else if (cmd == "--job-list") {
                std::lock_guard<std::mutex> jlock(g_jobsMutex);
                std::ostringstream jl;
                // Sort by startedAt descending
                std::vector<const DaemonJob*> sorted;
                sorted.reserve(g_jobs.size());
                for (const auto& kv : g_jobs) sorted.push_back(&kv.second);
                std::sort(sorted.begin(), sorted.end(), [](const DaemonJob* a, const DaemonJob* b) {
                    return a->startedAtUtc > b->startedAtUtc;
                });
                int count = 0;
                for (const DaemonJob* j : sorted) {
                    if (count >= 20) break;
                    jl << "JOB={\"id\":\"" << jsonEscape(j->id)
                       << "\",\"state\":\"" << jobStateName(j->state)
                       << "\",\"type\":\"" << jsonEscape(j->type)
                       << "\",\"snap\":\"" << jsonEscape(j->snap)
                       << "\",\"bytes\":" << j->bytesTransferred
                       << ",\"rate\":" << j->rateMiBs
                       << ",\"elapsed\":" << j->elapsedSecs
                       << ",\"started\":\"" << jsonEscape(j->startedAtUtc)
                       << "\",\"finished\":\"" << jsonEscape(j->finishedAtUtc)
                       << "\",\"error\":\"" << jsonEscape(j->errorText)
                       << "\"}\n";
                    ++count;
                }
                jl << "JOBS_COUNT=" << count << "\n";
                exec.rc = 0; exec.out = jl.str();
            } else if (cmd == "--job-cancel") {
                if (rpcArgs.empty()) {
                    exec.rc = 1; exec.err = "usage: --job-cancel <jobId>\n";
                } else {
                    std::lock_guard<std::mutex> jlock(g_jobsMutex);
                    auto it = g_jobs.find(rpcArgs[0]);
                    if (it == g_jobs.end()) {
                        exec.rc = 1; exec.out = "ERROR=job not found\n";
                    } else {
                        DaemonJob& j = it->second;
                        if (j.state != JobState::Running) {
                            exec.rc = 1; exec.out = "ERROR=job not running\n";
                        } else {
                            if (j.sendPid > 0) kill(j.sendPid, SIGTERM);
                            j.state        = JobState::Cancelled;
                            j.finishedAtUtc = utcNowIsoString();
                            persistJobsLocked();
                            daemonLog("INFO", "job " + j.id + " cancelled");
                            exec.rc  = 0;
                            exec.out = "JOB_ID=" + j.id + "\nCANCELLED=1\n";
                        }
                    }
                }
#endif // _WIN32
            } else {
            daemonLog("DEBUG", "rpc cmd=" + cmd);
            const bool dumpCmd = isDumpCommand(cmd);
            const bool mutateCmd = isMutateCommand(cmd);
            struct InvalidationProfile {
                std::vector<std::string> hints;
                std::set<std::string> dumpClasses;
            };
            const auto collectInvalidationProfile = [&](const std::string& mutateCmdName,
                                                        const std::vector<std::string>& mutateArgs) {
                InvalidationProfile p;
                if ((mutateCmdName == "--mutate-zfs-snapshot"
                     || mutateCmdName == "--mutate-zfs-destroy"
                     || mutateCmdName == "--mutate-zfs-rollback")
                    && !mutateArgs.empty()) {
                    p.dumpClasses.insert("zfs");
                    p.dumpClasses.insert("zpool");
                    std::string target = mutateArgs[0];
                    const std::size_t atPos = target.find('@');
                    if (atPos != std::string::npos) {
                        target = target.substr(0, atPos);
                    }
                    target = trim(target);
                    if (!target.empty()) {
                        p.hints.push_back(target);
                        const std::size_t slashPos = target.find('/');
                        if (slashPos != std::string::npos) {
                            const std::string pool = trim(target.substr(0, slashPos));
                            if (!pool.empty()) {
                                p.hints.push_back(pool);
                            }
                        } else {
                            p.hints.push_back(target);
                        }
                    } else {
                        p.hints.push_back("*");
                    }
                    return p;
                }
                // Para mutaciones genéricas preferimos limpieza global segura.
                p.hints.push_back("*");
                return p;
            };
            const auto cacheEntryMatchesHint = [&](const CacheEntry& entry,
                                                   const std::string& hintRaw) {
                const std::string hint = trim(hintRaw);
                if (hint.empty()) {
                    return false;
                }
                if (hint == "*") {
                    return true;
                }
                for (const std::string& aRaw : entry.args) {
                    std::string a = trim(aRaw);
                    if (a.empty()) {
                        continue;
                    }
                    const std::size_t atPos = a.find('@');
                    if (atPos != std::string::npos) {
                        a = a.substr(0, atPos);
                    }
                    if (a == hint || startsWith(a, hint + "/") || startsWith(hint, a + "/")) {
                        return true;
                    }
                }
                return false;
            };
            const auto invalidateCacheByProfile = [&](const InvalidationProfile& profile) {
                if (profile.hints.empty()) {
                    return;
                }
                const bool wipeAll = std::find(profile.hints.begin(), profile.hints.end(), std::string("*")) != profile.hints.end();
                std::lock_guard<std::mutex> lock(runtimeMutex);
                if (wipeAll) {
                    if (!rpcCache.empty()) {
                        cacheInvalidations.fetch_add(1);
                    }
                    rpcCache.clear();
                    return;
                }
                bool anyDropped = false;
                bool anyPoolHint = false;
                for (auto it = rpcCache.begin(); it != rpcCache.end();) {
                    if (!profile.dumpClasses.empty() && profile.dumpClasses.count(it->second.dumpClass) == 0) {
                        ++it;
                        continue;
                    }
                    bool drop = false;
                    for (const std::string& h : profile.hints) {
                        if (h.find('/') == std::string::npos && h != "*") {
                            anyPoolHint = true;
                        }
                        if (cacheEntryMatchesHint(it->second, h)) {
                            drop = true;
                            break;
                        }
                    }
                    if (drop) {
                        anyDropped = true;
                        it = rpcCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (anyDropped) {
                    cacheInvalidations.fetch_add(1);
                    if (anyPoolHint) {
                        poolInvalidations.fetch_add(1);
                    }
                }
            };
            if (dumpCmd && cfg.cacheTtlFastMs > 0) {
                const std::string cacheKey = makeRpcCacheKey(cmd, rpcArgs);
                {
                    std::lock_guard<std::mutex> lock(runtimeMutex);
                    const auto it = rpcCache.find(cacheKey);
                    if (it != rpcCache.end()) {
                        const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.ts).count();
                        if (ageMs <= cfg.cacheTtlFastMs) {
                            exec = it->second.result;
                        } else {
                            rpcCache.erase(it);
                        }
                    }
                }
                if (exec.out.empty() && exec.err.empty() && exec.rc == 1) {
                    exec = executeAgentCommandCapture(cmd, rpcArgs, "zfsmgr-agent");
                    if (exec.rc == 0) {
                        std::lock_guard<std::mutex> lock(runtimeMutex);
                        if (cfg.cacheMaxEntries > 0
                            && static_cast<int>(rpcCache.size()) >= cfg.cacheMaxEntries) {
                            auto victim = rpcCache.begin();
                            for (auto it = rpcCache.begin(); it != rpcCache.end(); ++it) {
                                if (it->second.ts < victim->second.ts) {
                                    victim = it;
                                }
                            }
                            if (victim != rpcCache.end()) {
                                rpcCache.erase(victim);
                            }
                        }
                        rpcCache[cacheKey] = CacheEntry{exec, now, cmd, rpcArgs, dumpClassForCommand(cmd)};
                    }
                }
            } else {
                exec = executeAgentCommandCapture(cmd, rpcArgs, "zfsmgr-agent");
            }
            if (mutateCmd && exec.rc == 0) {
                invalidateCacheByProfile(collectInvalidationProfile(cmd, rpcArgs));
            }
            if (exec.rc != 0) {
                rpcFailures.fetch_add(1);
                }
            }
        }

        const std::string response =
            std::string("{\"rc\":") + std::to_string(exec.rc)
            + ",\"stdout\":\"" + jsonEscape(exec.out) + "\""
            + ",\"stderr\":\"" + jsonEscape(exec.err) + "\"}\n";
        (void)SSL_write(ssl, response.data(), static_cast<int>(response.size()));
        SSL_shutdown(ssl);
        SSL_free(ssl);
        closeSock(clientFd);
    }
    if (zedThread.joinable()) {
        zedThread.join();
    }
    if (gsaThread.joinable()) {
        gsaThread.join();
    }
    closeSock(listenFd);
    SSL_CTX_free(ctx);
    EVP_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [--version|--api-version|--serve|--health|--dump-*|--mutate-*]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    const AgentRuntimeConfig runtimeCfg = loadRuntimeConfig();
    applyRuntimeEnvironment(runtimeCfg);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i] ? argv[i] : "");
    }

    const std::string cmd = (args.size() > 1) ? args[1] : std::string("--serve");

    if (cmd == "--version" || cmd == "version") {
        std::cout << ZFSMGR_AGENT_VERSION_STRING << '\n';
        return 0;
    }
    if (cmd == "--api-version" || cmd == "api") {
        std::cout << kApiVersion << '\n';
        return 0;
    }
    if (cmd == "--serve" || cmd == "serve") {
        return runServeLoop();
    }
    if (cmd == "--once") {
        writeHeartbeat();
        return 0;
    }
    if (cmd == "--health") {
        // This is a direct CLI invocation — not a live query to a running daemon.
        // Real ZED/cache state is only available via RPC when --serve is running.
        std::cout << "STATUS=OK\n";
        std::cout << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
        std::cout << "API=" << kApiVersion << "\n";
        std::cout << "SERVER=0\n";
        std::cout << "CACHE_ENTRIES=0\n";
        std::cout << "CACHE_MAX_ENTRIES=0\n";
        std::cout << "CACHE_INVALIDATIONS=0\n";
        std::cout << "POOL_INVALIDATIONS=0\n";
        std::cout << "RPC_FAILURES=0\n";
        std::cout << "RPC_COMMANDS=\n";
        std::cout << "ZED_ACTIVE=0\n";
#ifndef _WIN32
        std::cout << "JOBS_SUPPORT=1\n";
#endif
        std::cout << "CAPS=" << agentCapabilityList() << "\n";
        return 0;
    }

    if (cmd == "--dump-refresh-basics") {
        return runDumpRefreshBasics();
    }
    // Solo CLI a propósito: es lo que se ejecuta ANTES de que exista el canal TLS,
    // así que servirlo por RPC no tendría sentido.
    if (cmd == "--ensure-tls") {
        const ExecResult e = runEnsureTlsCapture();
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
#ifndef _WIN32
    if (cmd == "--dump-tool-availability") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        const ExecResult e = runDumpToolAvailabilityCapture(args[2]);
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
#endif
    if (cmd == "--dump-zfs-version") {
        ExecResult e = runExecCapture("zfs", {"version"});
        if (e.rc != 0 || trim(e.out).empty()) {
            e = runExecCapture("zfs", {"--version"});
        }
        if (e.rc != 0 || trim(e.out + "\n" + e.err).empty()) {
            e = runExecCapture("zpool", {"--version"});
        }
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    if (cmd == "--dump-zfs-mount") {
        return runExecStreaming("zfs", {"mount", "-j"});
    }
    if (cmd == "--dump-zpool-list") {
        return runExecStreaming("zpool", {"list", "-j"});
    }
    if (cmd == "--dump-zpool-import-probe") {
        const ExecResult a = runExecCapture("zpool", {"import"});
        const ExecResult b = runExecCapture("zpool", {"import", "-s"});
        if (!a.out.empty()) {
            std::cout << a.out;
        }
        if (!b.out.empty()) {
            std::cout << b.out;
        }
        if (!a.err.empty()) {
            std::cerr << a.err;
        }
        if (!b.err.empty()) {
            std::cerr << b.err;
        }
        return b.rc;
    }
    if (cmd == "--dump-zpool-guid-status-batch") {
        return runDumpZpoolGuidStatusBatch();
    }
    if (cmd == "--dump-zpool-guid") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zpool", {"get", "-H", "-o", "value", "guid", args[2]});
    }
    if (cmd == "--dump-zpool-status") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zpool", {"status", "-v", args[2]});
    }
    if (cmd == "--dump-zpool-status-p") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zpool", {"status", "-P", args[2]});
    }
    if (cmd == "--dump-zpool-history") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zpool", {"history", args[2]});
    }
    if (cmd == "--dump-zpool-get-all") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zpool", {"get", "-j", "all", args[2]});
    }
    if (cmd == "--dump-zfs-list-all") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"list", "-H", "-p", "-t", "filesystem,volume,snapshot", "-o",
                                         "name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount",
                                         "-r", args[2]});
    }
    if (cmd == "--dump-zfs-guid-map") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"get", "-H", "-o", "name,value", "guid", "-r", args[2]});
    }
    if (cmd == "--dump-zfs-list-children") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"list", "-H", "-o", "name", "-r", args[2]});
    }
    if (cmd == "--dump-advanced-breakdown-list") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        const ExecResult e = runDumpAdvancedBreakdownListCapture(args[2]);
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    if (cmd == "--dump-zfs-get-prop") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"get", "-H", "-o", "value", args[2], args[3]});
    }
    if (cmd == "--dump-zfs-get-all") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"get", "-j", "all", args[2]});
    }
    if (cmd == "--dump-zfs-get-json") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming("zfs", {"get", "-j", args[2], args[3]});
    }
    if (cmd == "--dump-zfs-get-gsa-raw-all-pools") {
        return runDumpGsaRawAllPools();
    }
    if (cmd == "--dump-zfs-get-gsa-raw-recursive") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runExecStreaming(
            "zfs",
            {"get", "-H", "-o", "name,property,value,source", "-r",
             "org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino",
             args[2]});
    }
    if (cmd == "--dump-gsa-connections-conf") {
        return runDumpGsaConnectionsConf();
    }
    if (cmd == "--dump-gsa-log") {
        const GsaConf conf = gsaLoadConf();
        std::ifstream f(conf.logFile);
        if (!f.is_open()) return 0;
        std::cout << f.rdbuf();
        return 0;
    }
    if (cmd == "--dump-daemon-log") {
        long long offset = 0;
        long long maxBytes = 0;
        if (args.size() > 2) {
            try { offset = std::stoll(args[2]); } catch (...) {}
        }
        if (args.size() > 3) {
            try { maxBytes = std::stoll(args[3]); } catch (...) {}
        }
        std::cout << readDaemonLogRange(offset, maxBytes);
        return 0;
    }
    if (cmd == "--heartbeat") {
        const std::string ts = utcNowIsoString();
        daemonLog("INFO", "heartbeat request (cli)");
        std::cout << "ALIVE=yes\n";
        std::cout << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
        std::cout << "TS=" << ts << "\n";
        return 0;
    }

    if (cmd == "--dump-zfs-exists") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
#ifdef HAVE_LIBZFS_CORE
        const bool exists = lzcExists(args[2]);
        std::cout << (exists ? "EXISTS=yes\n" : "EXISTS=no\n");
        return exists ? 0 : 1;
#endif
        const ExecResult sub = runExecCapture("zfs", {"list", "-H", "-o", "name", args[2]});
        std::cout << (sub.rc == 0 ? "EXISTS=yes\n" : "EXISTS=no\n");
        return sub.rc == 0 ? 0 : 1;
    }
    if (cmd == "--mutate-zfs-snapshot") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
        const bool recursive = (args[3] == "1" || toLower(args[3]) == "true" || toLower(args[3]) == "on");
#ifdef HAVE_LIBZFS_CORE
        if (!recursive) {
            const ExecResult lr = lzcSnapshotOne(args[2]);
            if (lr.rc == 0) return 0;
        }
#endif
        std::vector<std::string> a = {"snapshot"};
        if (recursive) {
            a.push_back("-r");
        }
        a.push_back(args[2]);
        return runExecStreaming("zfs", a);
    }
    if (cmd == "--mutate-zfs-destroy") {
        if (args.size() < 5) {
            printUsage(args[0].c_str());
            return 2;
        }
        const bool force = (args[3] == "1" || toLower(args[3]) == "true" || toLower(args[3]) == "on");
        const std::string& scope = args[4];
        const bool isSnap = args[2].find('@') != std::string::npos;
        const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
        if (isSnap && noScope && !force) {
            const ExecResult lr = lzcDestroyOneSnap(args[2]);
            if (lr.rc == 0) return 0;
        }
#endif
        std::vector<std::string> a = {"destroy"};
        if (force) {
            a.push_back("-f");
        }
        if (scope == "R") {
            a.push_back("-R");
        } else if (scope == "r") {
            a.push_back("-r");
        }
        a.push_back(args[2]);
        return runExecStreaming("zfs", a);
    }
    if (cmd == "--mutate-zfs-rollback") {
        if (args.size() < 5) {
            printUsage(args[0].c_str());
            return 2;
        }
        const bool force = (args[3] == "1" || toLower(args[3]) == "true" || toLower(args[3]) == "on");
        const std::string& scope = args[4];
        const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
        if (!force && noScope) {
            const auto atPos = args[2].find('@');
            if (atPos != std::string::npos) {
                const ExecResult lr = lzcRollbackTo(args[2].substr(0, atPos), args[2]);
                if (lr.rc == 0) return 0;
            }
        }
#endif
        std::vector<std::string> a = {"rollback"};
        if (force) {
            a.push_back("-f");
        }
        if (scope == "R") {
            a.push_back("-R");
        } else if (scope == "r") {
            a.push_back("-r");
        }
        a.push_back(args[2]);
        return runExecStreaming("zfs", a);
    }
    if (cmd == "--mutate-zfs-clone") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
#ifdef HAVE_LIBZFS_CORE
        const ExecResult lr = lzcClone(args[2], args[3]);
        if (lr.rc == 0) return 0;
#endif
        return runExecStreaming("zfs", {"clone", args[2], args[3]});
    }
    if (cmd == "--mutate-zfs-load-key") {
#ifndef _WIN32
        if (args.size() < 4) { printUsage(args[0].c_str()); return 2; }
        std::string dataset, passphrase;
        if (!decodeBase64(args[2], dataset) || !decodeBase64(args[3], passphrase)) {
            std::cerr << "invalid base64 argument\n"; return 2;
        }
        dataset = trim(dataset);
        if (dataset.empty()) { std::cerr << "empty dataset\n"; return 2; }
        const ExecResult e = runExecCaptureWithStdin("zfs", {"load-key", dataset}, passphrase + "\n");
        if (!e.out.empty()) std::cout << e.out;
        if (!e.err.empty()) std::cerr << e.err;
        return e.rc;
#else
        std::cerr << "not supported on Windows\n"; return 2;
#endif
    }
    if (cmd == "--mutate-zfs-change-key") {
#ifndef _WIN32
        if (args.size() < 5) { printUsage(args[0].c_str()); return 2; }
        std::string dataset, passphrase, flagsStr;
        if (!decodeBase64(args[2], dataset) || !decodeBase64(args[3], passphrase) || !decodeBase64(args[4], flagsStr)) {
            std::cerr << "invalid base64 argument\n"; return 2;
        }
        dataset = trim(dataset);
        flagsStr = trim(flagsStr);
        if (dataset.empty()) { std::cerr << "empty dataset\n"; return 2; }
        std::vector<std::string> changeArgs = {"change-key"};
        {
            std::istringstream iss(flagsStr);
            std::string tok;
            while (iss >> tok) changeArgs.push_back(tok);
        }
        changeArgs.push_back(dataset);
        const ExecResult e = runExecCaptureWithStdin("zfs", changeArgs, passphrase + "\n" + passphrase + "\n");
        if (!e.out.empty()) std::cout << e.out;
        if (!e.err.empty()) std::cerr << e.err;
        return e.rc;
#else
        std::cerr << "not supported on Windows\n"; return 2;
#endif
    }
    if (cmd == "--mutate-zfs-generic") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runGenericMutation("zfs", args[2]);
    }
    if (cmd == "--mutate-advanced-breakdown") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
        const std::vector<std::string> params(args.begin() + 2, args.end());
        const ExecResult e = runMutateAdvancedBreakdownCapture(params);
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    if (cmd == "--mutate-advanced-assemble") {
        if (args.size() < 4) {
            printUsage(args[0].c_str());
            return 2;
        }
        const std::vector<std::string> params(args.begin() + 2, args.end());
        const ExecResult e = runMutateAdvancedAssembleCapture(params);
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
#ifndef _WIN32
    if (cmd == "--mutate-advanced-todir") {
        if (args.size() < 5) {
            printUsage(args[0].c_str());
            return 2;
        }
        const std::vector<std::string> params(args.begin() + 2, args.end());
        const ExecResult e = runMutateAdvancedToDirCapture(params);
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
#endif
    if (cmd == "--mutate-zpool-generic") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runGenericMutation("zpool", args[2]);
    }
    if (cmd == "--mutate-zfs-allow-batch") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runZfsAllowBatch(args[2]);
    }
#ifndef _WIN32
    if (cmd == "--repair-alt-mountpoints") {
        const bool apply = args.size() > 2 && toLower(trim(args[2])) == "apply";
        const ExecResult e = runRepairAltMountpointsCapture(apply);
        if (!e.out.empty()) std::cout << e.out;
        if (!e.err.empty()) std::cerr << e.err;
        return e.rc;
    }
    if (cmd == "--mutate-sync-temp-tar-source") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runSyncTempTarSource(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    if (cmd == "--mutate-sync-temp-tar-dest") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runSyncTempTarDest(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    if (cmd == "--mutate-advanced-fromdir") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runMutateAdvancedFromDir(std::vector<std::string>(args.begin() + 2, args.end()));
    }
#endif
    if (cmd == "--mutate-rsync-local") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runRsyncLocal(args[2]);
    }
    if (cmd == "--mutate-shell-generic") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runMutateShellGeneric(args[2]);
    }

#ifndef _WIN32
    if (cmd == "--zfs-pipe-local") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        const ExecResult e = runZfsPipeLocalCapture(args[2]);
        if (!e.out.empty()) std::cout << e.out;
        if (!e.err.empty()) std::cerr << e.err;
        return e.rc;
    }
    if (cmd == "--zfs-send-to-peer") {
        if (args.size() < 6) {
            printUsage(args[0].c_str());
            return 2;
        }
        const std::vector<std::string> params(args.begin() + 2, args.end());
        const ExecResult e = runZfsSendToPeerCapture(params);
        if (!e.out.empty()) std::cout << e.out;
        if (!e.err.empty()) std::cerr << e.err;
        return e.rc;
    }
#endif

    printUsage(args[0].c_str());
    return 2;
}
