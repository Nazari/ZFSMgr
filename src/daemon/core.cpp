// The agent's foundations: platform shims, logging, the background job registry and
// the capability list the client reads to know which verbs this binary serves.

#include "agent_internal.h"

namespace zfsmgr::agent {

#ifdef _WIN32
#else
#endif

#ifdef _WIN32
#else
#endif
// En Windows no existe /var/lib: el daemon no tenía dónde escribir y la pestaña
// Daemon salía vacía, sin decir por qué (el ofstream fallaba y se retornaba en
// silencio). El comando --dump-daemon-log sí estaba disponible; lo que faltaba era
// el fichero.
#ifdef _WIN32
#else
#endif
#ifdef _WIN32
#else
#endif
// Rutas por defecto. En Windows no existe /etc, así que apuntar ahí dejaba al agente
// sin configuración ni certificados salvo que se le pasara ZFSMGR_AGENT_CONFIG a mano
// — comprobado en un Windows 11 real, donde --serve moría en silencio por eso.
#ifdef _WIN32
#else
#endif
#ifdef _WIN32
#else
#endif
// En Windows el separador es ';' y las rutas Unix no significan nada: con el valor
// de abajo el agente arrancaba y servía TLS, pero NINGÚN comando se encontraba.
#ifdef _WIN32
#else
#endif

// Cuándo arrancó ESTE proceso. Se toma una vez, al empezar, porque es la mitad de la
// comparación que distingue «no me han dado permiso» de «me lo dieron después de arrancar».
const std::chrono::system_clock::time_point g_arranqueDelProceso = std::chrono::system_clock::now();

// ¿Cambió la base de permisos DESPUÉS de que arrancáramos?
//
// No se puede leer su contenido —eso exige el permiso que precisamente falta— pero sí
// preguntar por su fecha: `stat` mira el directorio que la contiene, no el fichero. Si esa
// fecha es posterior a nuestro arranque, lo más probable es que el permiso se acabe de
// conceder y falte reiniciar el agente, porque macOS lo decide al arrancar el proceso.
//
// Es una PISTA, no una certeza, y por eso el estado que produce se llama «probablemente».
// Si `stat` tampoco se puede, se dice que no y el aviso sale con las dos causas.
bool permisosCambiaronTrasArranque() {
#ifdef __APPLE__
    struct ::stat st{};
    if (::stat("/Library/Application Support/com.apple.TCC/TCC.db", &st) != 0) {
        return false;
    }
    const auto cambio = std::chrono::system_clock::from_time_t(st.st_mtime);
    return cambio > g_arranqueDelProceso;
#else
    return false;
#endif
}

// Una partición ZFS que el agente no puede abrir, o vacío si puede con todas.
//
// **Se miran las particiones ZFS, no un disco cualquiera.** La regla anterior era «si se
// puede abrir uno, el permiso está concedido», y es falsa: sin «Acceso total al disco»,
// macOS deja abrir el disco INTERNO (/dev/disk0s1) y bloquea los EXTERNOS, que es donde
// suelen estar los pools. Medido en mmela el 2026-10-10: disk0s1 se abría y disk4s1 —el del
// pool— daba EPERM, así que el pool importable no aparecía y nadie lo decía. `diskutil list`
// no necesita el permiso y dice qué particiones son de tipo ZFS: son esas las que importan.
std::string discoIlegible() {
#ifdef __APPLE__
    FILE* p = ::popen("/usr/sbin/diskutil list 2>/dev/null", "r");
    if (!p) {
        return {};
    }
    std::vector<std::string> zfs;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) {
        const std::string linea(buf);
        // «   1:                        ZFS                         256.1 GB   disk4s1»
        if (linea.find(" ZFS ") == std::string::npos) {
            continue;
        }
        std::istringstream iss(linea);
        std::string tok;
        std::string ultimo;
        while (iss >> tok) {
            ultimo = tok;
        }
        if (ultimo.rfind("disk", 0) == 0) {
            zfs.push_back("/dev/" + ultimo);
        }
    }
    ::pclose(p);
    for (const std::string& ruta : zfs) {
        const int fd = ::open(ruta.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::close(fd);
            continue;
        }
        if (errno == EPERM || errno == EACCES) {
            return ruta;
        }
        // Ocupado —un pool importado tiene el disco abierto— u otra cosa: no es el permiso.
    }
    return {};
#else
    return {};
#endif
}

#include "zfsmgr/agentversion_generated.h"
#ifndef ZFSMGR_AGENT_VERSION_STRING
#define ZFSMGR_AGENT_VERSION_STRING ZFSMGR_APP_VERSION
#endif

std::atomic<bool> g_stop{false};
std::mutex g_daemonLogMutex;

// ── Background job registry ──────────────────────────────────────────────────

// El error de un trabajo: su código y sus argumentos. La frase inglesa queda solo para el
// registro del daemon; al cliente le llega el código. Lo que no es del agente —la salida de
// `zfs`— va con el código «text».
void ponErrorDeTrabajo(DaemonJob& j, const std::string& codigo, std::vector<std::string> args) {
    if (codigo.empty()) {
        j.errorCode = "text";
        j.errorArgs = {args.empty() ? std::string() : args.front()};
    } else {
        j.errorCode = codigo;
        j.errorArgs = std::move(args);
    }
    j.errorText = zfsmgr::base::agentmsg::inEnglish(j.errorCode, j.errorArgs);
}

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

// ¿Le han pedido a ESTE trabajo que pare? Los trabajos de copia (desglosar, ensamblar,
// hacia dir) corren en un hilo dentro del daemon, no en un proceso aparte, así que el
// SIGTERM de --job-cancel no llega a ninguna parte: el trabajo quedaba marcado como
// cancelado y seguía hasta el final. Comprobado en vivo: un Desglosar "cancelado" creó
// igualmente sus 262 datasets. Hay que mirar la bandera y salir por las buenas.
bool jobCancelRequested() {
    if (t_currentJobId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    const auto it = g_jobs.find(t_currentJobId);
    return it != g_jobs.end() && it->second.state == JobState::Cancelled;
}

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

// Los CONTADORES del trabajo, que son cosa aparte de la línea de progreso.
//
// Un trabajo de mutación —breakdown, assemble, todir, rsync— dejaba `bytesTransferred` y
// `rateMiBs` en CERO de principio a fin. El muestreador de copia calculaba las dos cifras
// cada dos segundos, las pegaba dentro de una frase —«5,7 GiB copiados a 17,6 MiB/s»— y
// tiraba los números. Así que `jobs` enseñaba «0B» y «0.00» mientras la máquina movía
// gigabytes, que es exactamente el síntoma que ya nos costó una tarde con `send-to-peer`:
// un contador a cero no se lee como «no lo sé», se lee como «no está pasando nada».
//
// Que los datos estuvieran en la frase no lo salvaba: una frase no se ordena, no se suma y
// no sale en `--format json` como número. Los campos existen desde el principio y esto es
// lo único que faltaba, rellenarlos.
void reportJobCounters(uint64_t bytes, double rateMiBs, long elapsedSecs) {
    if (t_currentJobId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(t_currentJobId);
    if (it == g_jobs.end()) {
        return;
    }
    it->second.bytesTransferred = bytes;
    it->second.rateMiBs = rateMiBs;
    it->second.elapsedSecs = elapsedSecs;
}

// Un fallo DEL PROPIO AGENTE, como código del catálogo (`base/agentmsg`) con sus argumentos.
// En `err` va además la frase en inglés: es lo que leen los clientes anteriores a los
// códigos y lo que queda en el registro. Ver agentmsg.h.
// Solo el código y su frase, sin tocar el `rc` que ya ponga quien llama.
void ponCodigo(ExecResult& r, const std::string& codigo, std::vector<std::string> args) {
    r.code = codigo;
    r.err = zfsmgr::base::agentmsg::inEnglish(codigo, args) + "\n";
    r.args = std::move(args);
}

// Lo mismo sobre un resultado que ya lleva otras cosas (la salida, por ejemplo).
void ponFallo(ExecResult& r, int rc, const std::string& codigo, std::vector<std::string> args) {
    r.rc = rc;
    r.code = codigo;
    r.err = zfsmgr::base::agentmsg::inEnglish(codigo, args) + "\n";
    r.args = std::move(args);
}

ExecResult fallo(int rc, const std::string& codigo, std::vector<std::string> args) {
    ExecResult r;
    r.rc = rc;
    r.code = codigo;
    r.err = zfsmgr::base::agentmsg::inEnglish(codigo, args) + "\n";
    r.args = std::move(args);
    return r;
}

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
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
    // DÓNDE está montado se le pregunta a `zfs mount`, no a la propiedad `mountpoint`.
    //
    // La propiedad dice dónde montaría; `zfs mount` dice dónde ESTÁ. En Windows la
    // diferencia no es sutil: la propiedad devuelve una ruta de estilo Unix
    // —/winpool/desglose— que allí no existe, mientras el montaje real es Z:\desglose.
    // Desglosar construía la ruta con la propiedad y descartaba todos los directorios
    // con «skip_not_a_directory», así que la operación no hacía nada y decía que sí.
    //
    // Es el mismo error que ya se corrigió en el árbol de la interfaz con driveletter:
    // deducir la ruta de una propiedad heredable en vez de consultar los montajes de
    // verdad. Si el dataset no aparece en la lista se recurre a la propiedad, que es
    // mejor que nada.
    {
        const ExecResult mnt = runExecCapture("zfs", {"mount"});
        if (mnt.rc == 0) {
            for (const std::string& line : splitLines(mnt.out)) {
                if (line.rfind(dataset, 0) != 0) {
                    continue;
                }
                const std::string rest = line.substr(dataset.size());
                if (rest.empty() || (rest[0] != ' ' && rest[0] != '\t')) {
                    continue;  // otro dataset cuyo nombre empieza igual
                }
                std::string path = trim(rest);
                // Windows devuelve «Z:/desglose/», con barra final; concatenar sobre eso
                // deja separadores dobles, que unas veces se toleran y otras no.
                while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
                    const std::string without = path.substr(0, path.size() - 1);
                    if (!without.empty() && without.back() == ':') {
                        break;  // la raíz de una unidad sí lleva separador
                    }
                    path = without;
                }
                if (!path.empty()) {
                    ExecResult ok;
                    ok.rc = 0;
                    ok.out = path;
                    return ok;
                }
            }
        }
    }
    ExecResult mp = getZfsPropertyCapture(dataset, "mountpoint");
    if (mp.rc != 0) {
        return mp;
    }
    const std::string mountpoint = trim(mp.out);
    if (mountpoint.empty() || mountpoint == "-" || mountpoint == "none" || mountpoint == "legacy") {
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
        // Los holds: leerlos y soltarlos. Se declaran para que un cliente nuevo hablando
        // con un agente viejo NO ofrezca la función y falle al pulsarla; sabrá que no está.
        "--dump-zfs-holds",
        "--mutate-zfs-hold",
        "--mutate-zfs-rename",
        "--mutate-zfs-release",
    };
    caps.push_back("--dump-tool-availability");
    // Transferencias y trabajos en segundo plano, ya en las dos plataformas (fases 1 a 5).
    //
    // Esto es lo que de verdad enciende la función: lo que el agente DECLARA manda sobre
    // la tabla estática del cliente. Dejarlos aquí dentro del #ifndef habría dejado
    // Send y Nivelar apagadas en Windows por mucho que la tabla dijera lo contrario.
    caps.push_back("--job-submit");
    caps.push_back("--zfs-send-to-peer");
    caps.push_back("--dump-zfs-driveletters");
    caps.push_back("--mutate-advanced-fromdir-prepare");
    caps.push_back("--mutate-prepare-dir");
    caps.push_back("--tree-recv-listen");
    caps.push_back("--tree-send-to-peer");
    // El emisor acepta `--as <nombre>` para un fichero suelto. Un agente anterior IGNORA lo
    // que no conoce y lo mandaría con su nombre: el cliente mira esto antes de pedirlo.
    caps.push_back("--tree-send-as");
    caps.push_back("--mutate-set-peers");
    caps.push_back("--dump-peers");
    caps.push_back("--mutate-set-bind");
    caps.push_back("--mutate-zfs-recv-abort");
    caps.push_back("--dump-resolve-devices");
    caps.push_back("--mutate-zpool-labelclear");
    caps.push_back("--dump-path-list");
    caps.push_back("--dump-path-info");
    caps.push_back("--file-recv-listen");
    // Escribe el fichero comprimido (tercer parámetro) y lee los comprimidos al enviarlos. Un
    // agente anterior IGNORA el tercer parámetro y lo dejaría sin comprimir: el cliente mira
    // esto antes de pedirlo.
    caps.push_back("--file-stream-compression");
    caps.push_back("--file-send-to-peer-async");
    // El estado del planificador y una pasada a demanda.
    caps.push_back("--dump-gsa-status");
    caps.push_back("--mutate-gsa-run-once");
#if !defined(_WIN32) && !defined(__APPLE__)
    caps.push_back("--mutate-mount-ro-temp");
    caps.push_back("--mutate-unmount-ro-temp");
#endif
#ifndef _WIN32
    caps.push_back("--repair-alt-mountpoints");
    caps.push_back("--mutate-advanced-todir");
    caps.push_back("--mutate-copy-local");
#endif
    // Desglosar y Ensamblar, en las dos plataformas. El comentario que había aquí decía
    // que en Windows habían dependido de makeTempDir y que «esa parte ya está resuelta»;
    // era cierto a medias y por eso contradecía a la tabla del cliente durante semanas.
    // makeTempDir sí estaba portado, pero lo que las rompía era otra cosa: la ruta se
    // deducía de la propiedad `mountpoint`, que allí vale «/pool/ds» y no existe. Ahora
    // se consultan los montajes reales y ambas están comprobadas contra una máquina.
    // Ensamblar va en las dos plataformas —verificado contra una máquina Windows: los
    // datasets vuelven a ser directorios en su sitio—. Desglosar NO: allí los descendientes
    // se montan planos bajo la letra del pool, así que el dataset nuevo no quedaría donde
    // estaba el directorio. Ver `runMutateAdvancedBreakdownCapture`.
#ifndef _WIN32
    caps.push_back("--mutate-advanced-breakdown");
#endif
    caps.push_back("--mutate-advanced-assemble");
    std::string out;
    for (size_t i = 0; i < caps.size(); ++i) {
        if (i)
            out += ",";
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
    std::snprintf(suffix, sizeof(suffix), "%08lX%04X%04X", static_cast<unsigned long>(guid.Data1),
                  guid.Data2, guid.Data3);
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
    std::snprintf(suffix, sizeof(suffix), "%08lX%04X%04X", static_cast<unsigned long>(guid.Data1),
                  guid.Data2, guid.Data3);
    const std::string dir = std::string(tempRoot) + patternPrefix + suffix;
    if (!CreateDirectoryA(dir.c_str(), nullptr)) {
        return {};
    }
    return dir;
#endif
}

// Bytes que ocupa un dataset, para poder medir cuánto lleva copiado sin recorrer el
// árbol: ZFS ya lleva la cuenta.
long long datasetUsedBytes(const std::string& ds) {
    const ExecResult r = runExecCapture("zfs", {"list", "-Hp", "-o", "used", ds});
    if (r.rc != 0) {
        return -1;
    }
    try {
        return std::stoll(trim(r.out));
    } catch (...) {
        return -1;
    }
}

std::string humanBytes(long long n) {
    static const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (i == 0 ? "%.0f %s" : "%.1f %s"), v, u[i]);
    return buf;
}

// ¿Está montado? Se pregunta mucho, y siempre por el mismo motivo: ZFS remonta solo al
// cambiar el mountpoint o al renombrar, así que un `zfs mount` posterior falla con
// "filesystem already mounted" aunque todo esté bien. Fallar o avisar por eso es ruido.
bool datasetIsMounted(const std::string& name) {
    const ExecResult r = getZfsPropertyCapture(name, "mounted");
    return r.rc == 0 && trim(r.out) == "yes";
}

// Send un árbol. Ya NO usa rsync.
//
// Conserva el nombre y la firma a propósito: cambia quién copia, no la disciplina de
// los llamantes —Desglosar, Ensamblar y Hacia Dir—, que copian, verifican y solo
// entonces borran el origen. Tocar las dos cosas a la vez en operaciones destructivas
// era pedir problemas.
//
// El porqué de quitar rsync está en docs/diseno_tecnico_copia_nativa_sin_rsync.md: no
// existe en Windows, y las alternativas —cwRsync— reintroducirían la capa Unix que se
// quitó con MSYS2 y emularían las ACL de NTFS en vez de copiarlas.
// Copia local con el motor del proyecto. Se llamaba `runRsyncCopyMoveCapture` y NO llama a
// rsync: llama a `copytree::copyTree`. El nombre era un resto de cuando sí lo hacía, y
// mandaba a buscar una dependencia que no está.
// Lo que macOS crea en la raíz de CADA volumen montado, y por tanto de cada dataset: el
// índice de Spotlight, el registro de eventos, la papelera y el icono que pone OpenZFS. No
// son datos del usuario y se regeneran solos. `.Spotlight-V100` ni siquiera se puede leer
// —ni como root con Acceso total al disco—, así que Ensamblar o Hacia Dir de un dataset que
// Spotlight había indexado fallaba con «Operation not permitted» (medido en mbp).
// Solo en la raíz de lo que se copia, que es donde los pone el sistema.
std::vector<std::string> conExclusionesDelSistema(std::vector<std::string> excludes) {
#ifdef __APPLE__
    for (const char* n : {".Spotlight-V100", ".fseventsd", ".Trashes", ".TemporaryItems",
                          ".DocumentRevisions-V100", ".VolumeIcon.icns"}) {
        excludes.emplace_back(n);
    }
#endif
    return excludes;
}

ExecResult runCopyTreeCapture(const std::string& srcDir, const std::string& dstDir,
                              const std::vector<std::string>& excludes, bool oneFileSystem) {
    ExecResult r;
    zfsmgr::copytree::Options opt;
    opt.oneFileSystem = oneFileSystem;
    opt.excludes = conExclusionesDelSistema(excludes);
    const zfsmgr::copytree::Result cr = zfsmgr::copytree::copyTree(srcDir, dstDir, opt);
    if (!cr.ok) {
        r.rc = 1;
        r.err = cr.error + "\n";
        return r;
    }
    r.rc = 0;
    r.out = "FILES=" + std::to_string(cr.filesCopied) + " DIRS=" + std::to_string(cr.dirsCreated)
            + " SYMLINKS=" + std::to_string(cr.symlinksCopied)
            + " HARDLINKS=" + std::to_string(cr.hardLinksRecreated)
            + " BYTES=" + std::to_string(cr.bytesWritten) + "\n";
    return r;
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

}  // namespace zfsmgr::agent
