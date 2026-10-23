// Scheduled snapshots (GSA): the scheduler thread, its persistent state and levelling as a
// daemon job. Also loose paths (`#path`), read-only temporary mounts and the daemon's peers.

#include "agent_internal.h"

#include <set>

namespace zfsmgr::agent {

// ── El planificador de instantáneas (GSA) ───────────────────────────────────────────
//
// Vive AQUÍ, en el daemon, y no en un guion con temporizador aparte. La razón de peso es
// la pata de «nivelar»: el guion la hacía con ssh + sshpass + su propio known_hosts y la
// contraseña por tubería, que es justo el patrón que este programa está borrando, mientras
// el daemon ya sabe mandar un flujo con mTLS, reanudación y seguimiento como trabajo. Y de
// paso: un instalador en vez de dos, un almacén de credenciales en vez de dos, y en
// Windows —donde un guion de shell no va a existir nunca— funciona igual.
//
// Ver docs/propuesta_gsa_cli.md.

// La CLAVE DEL PERIODO al que pertenece un instante, según la clase.
//
// Es la pieza que sustituye al «si son las 00:00, toca la diaria» del guion, y no es un
// detalle de estilo: aquello dependía de que el temporizador disparase EXACTAMENTE a esa
// hora. Con la máquina apagada a medianoche, la diaria de ese día no se hacía nunca y nada
// lo decía. Aquí se compara el periodo actual con el de la última instantánea de esa
// clase: si no coinciden, toca — se encendiera la máquina a la hora que se encendiera.
std::string gsaClaveDePeriodo(const std::string& clase, std::time_t cuando) {
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &cuando);
#else
    localtime_r(&cuando, &tmv);
#endif
    char buf[32] = {0};
    if (clase == "hourly") {
        std::strftime(buf, sizeof(buf), "%Y%m%d%H", &tmv);
    } else if (clase == "daily") {
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    } else if (clase == "weekly") {
        // Semana ISO: «%G%V» y no «%Y%W», que en enero mezcla dos años.
        std::strftime(buf, sizeof(buf), "%G%V", &tmv);
    } else if (clase == "monthly") {
        std::strftime(buf, sizeof(buf), "%Y%m", &tmv);
    } else {
        std::strftime(buf, sizeof(buf), "%Y", &tmv);
    }
    return std::string(buf);
}

// «GSA-daily-20260818-030000» → el instante. Devuelve false si el nombre no es de los
// nuestros, que es como se distingue una instantánea manual de una programada.
bool gsaInstanteDeNombre(const std::string& snap, const std::string& clase, std::time_t& out) {
    const std::string prefijo = "GSA-" + clase + "-";
    if (snap.rfind(prefijo, 0) != 0) {
        return false;
    }
    const std::string sello = snap.substr(prefijo.size());
    if (sello.size() != 15 || sello[8] != '-') {
        return false;
    }
    std::tm tmv{};
    try {
        tmv.tm_year = std::stoi(sello.substr(0, 4)) - 1900;
        tmv.tm_mon = std::stoi(sello.substr(4, 2)) - 1;
        tmv.tm_mday = std::stoi(sello.substr(6, 2));
        tmv.tm_hour = std::stoi(sello.substr(9, 2));
        tmv.tm_min = std::stoi(sello.substr(11, 2));
        tmv.tm_sec = std::stoi(sello.substr(13, 2));
    } catch (...) {
        return false;
    }
    tmv.tm_isdst = -1;
    out = std::mktime(&tmv);
    return out != static_cast<std::time_t>(-1);
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
             "org.zfsmgr:schedule-enabled,org.zfsmgr:schedule-level,org.zfsmgr:schedule-target",
             pool});
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

// ── GSA scheduler ────────────────────────────────────────────────────────────

bool gsaBoolOn(const std::string& s) {
    std::string lo = s;
    for (char& c : lo) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lo == "on" || lo == "yes" || lo == "true" || lo == "1";
}

int gsaIntVal(const std::string& s) {
    if (s.empty())
        return 0;
    for (unsigned char c : s) {
        if (!std::isdigit(c))
            return 0;
    }
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
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
    if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0)
        return "00000000-000000";
    return buf;
}

// GsaTimeFields/gsaLocalTimeFields vivían aquí: la hora, el día de la semana, el del mes y
// el mes, para decidir qué clase de instantánea tocaba. Ya no se usan — desde que la
// decisión no la toma el reloj sino los NOMBRES de las instantáneas que ya hay
// (gsaDueClasses), que es lo que hace que una máquina apagada a medianoche no pierda su
// diaria.

// ¿Cae esta ruta dentro de algún punto de montaje de ZFS?
//
// Se comparan rutas CANONIZADAS: sin eso, «/mnt/datos/../../etc/shadow» pasaría el filtro
// por empezar con un punto de montaje. `weakly_canonical` resuelve «..» aunque el último
// tramo no exista.
bool rutaDentroDeUnMountpoint(const std::string& ruta) {
    std::error_code ec;
    const std::filesystem::path pedida = std::filesystem::weakly_canonical(ruta, ec);
    if (ec) {
        return false;
    }
    // DÓNDE está montado cada uno se le pregunta a `zfs mount`, no a la propiedad.
    //
    // Es el mismo error que ya se corrigió en Desglosar y en el árbol de la interfaz, y aquí
    // dejaba la comprobación IMPOSIBLE DE PASAR EN WINDOWS: la propiedad dice «/winpool/sa»
    // —una ruta que allí no existe— y el montaje real es «Z:/sa/». Comprobado contra OldLau.
    // O sea que `--dump-dir-list` y `--dump-file` estaban muertos en Windows: contestaban
    // «la ruta no está dentro de ningún punto de montaje» a cualquier ruta, incluida la
    // buena. Afectaba al `ls #content` del intérprete.
    //
    // La propiedad se conserva de reserva: si `zfs mount` no contesta —o el dataset no
    // aparece en su lista— es mejor eso que nada.
    std::vector<std::string> puntos;
    {
        const ExecResult mnt = runExecCapture("zfs", {"mount"});
        if (mnt.rc == 0) {
            for (const std::string& linea : splitLines(mnt.out)) {
                // «<dataset><espacios><ruta>»: la ruta es lo que va detrás del primer bloque
                // de espacios, y puede llevar espacios dentro.
                const std::string l = trim(linea);
                const std::size_t sep = l.find_first_of(" \t");
                if (sep == std::string::npos) {
                    continue;
                }
                const std::string ruta = trim(l.substr(sep));
                if (!ruta.empty()) {
                    puntos.push_back(ruta);
                }
            }
        }
    }
    const ExecResult r = runExecCapture("zfs", {"list", "-H", "-o", "mountpoint"});
    if (r.rc == 0) {
        for (const std::string& linea : splitLines(r.out)) {
            puntos.push_back(linea);
        }
    }
    if (puntos.empty()) {
        return false;
    }
    for (const std::string& linea : puntos) {
        const std::string mp = trim(linea);
        if (mp.empty() || mp == "-" || mp == "legacy" || mp == "none") {
            continue;
        }
        std::error_code ec2;
        const std::filesystem::path base = std::filesystem::weakly_canonical(mp, ec2);
        if (ec2) {
            continue;
        }
        // Prefijo POR COMPONENTES, no por texto: «/mnt/datos2» empieza por «/mnt/datos» y
        // no está dentro de él.
        auto itB = base.begin();
        auto itP = pedida.begin();
        bool dentro = true;
        for (; itB != base.end(); ++itB, ++itP) {
            if (itP == pedida.end() || *itB != *itP) {
                dentro = false;
                break;
            }
        }
        if (dentro) {
            return true;
        }
    }
    return false;
}

// Un directorio, en JSON: nombre, tipo y tamaño.
//
// JSON y no TSV a propósito: un nombre de fichero puede llevar un tabulador o un salto de
// línea dentro, y con TSV eso corre las columnas o parte la entrada en dos.
ExecResult listaDirectorio(const std::string& ruta) {
    ExecResult r;
    std::error_code ec;
    zfsmgr::base::json::Array entradas;
    for (const auto& e : std::filesystem::directory_iterator(
             ruta, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        const bool dir = e.is_directory(ec2);
        zfsmgr::base::json::Value v;
        v.set("name", zfsmgr::base::json::Value(e.path().filename().string()));
        v.set("type", zfsmgr::base::json::Value(std::string(dir ? "d" : "f")));
        const std::uintmax_t tam = dir ? 0 : e.file_size(ec2);
        v.set("size", zfsmgr::base::json::Value(static_cast<long long>(ec2 ? 0 : tam)));
        entradas.push_back(v);
    }
    if (ec) {
        r.rc = 1;
        ponCodigo(r, "cannot_read_dir", {ec.message()});
        return r;
    }
    zfsmgr::base::json::Value raiz;
    raiz.set("entries", zfsmgr::base::json::Value(entradas));
    r.rc = 0;
    r.out = zfsmgr::base::json::toCompact(raiz) + "\n";
    return r;
}

// --- Rutas sueltas: `zfsm://maq#path/...`.
//
// Lo que NO es contenido de un dataset: cualquier directorio de la máquina. A diferencia de
// `--dump-dir-list`, que se queda dentro de los montajes de ZFS, estos dos verbos aceptan
// cualquier ruta, y es una decisión tomada a propósito. Solo devuelven NOMBRES y `stat`,
// nunca contenido (`--dump-file` sigue limitado). Los verbos de copia
// (`--mutate-copy-local`, `--tree-send-to-peer`) ya leían cualquier ruta como root, así
// que el límite del listado no protegía nada y sí impedía navegar.

std::filesystem::path canonizaSinBarra(std::string ruta) {
    while (ruta.size() > 1 && (ruta.back() == '/' || ruta.back() == '\\')
           && !(ruta.size() == 3 && ruta[1] == ':')) {
        ruta.pop_back();
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(ruta, ec);
    if (ec) {
        p = std::filesystem::path(ruta);
    }
    // weakly_canonical deja la barra final si la había en lo que no existe: fuera, para que
    // la comparación por componentes no vea un último tramo vacío.
    std::string t = p.string();
    while (t.size() > 1 && (t.back() == '/' || t.back() == '\\')
           && !(t.size() == 3 && t[1] == ':')) {
        t.pop_back();
    }
    return std::filesystem::path(t);
}

// Cada dataset montado y dónde, según `zfs mount` —no la propiedad: en Windows la
// propiedad dice «/winpool/sa» y el montaje real es «Z:/sa/», ver rutaDentroDeUnMountpoint—.
std::vector<MontajeZfs> montajesZfs() {
    std::vector<MontajeZfs> out;
    const ExecResult mnt = runExecCapture("zfs", {"mount"});
    if (mnt.rc != 0) {
        return out;
    }
    for (const std::string& linea : splitLines(mnt.out)) {
        const std::string l = trim(linea);
        const std::size_t sep = l.find_first_of(" \t");
        if (sep == std::string::npos) {
            continue;
        }
        const std::string ruta = trim(l.substr(sep));
        if (!ruta.empty()) {
            out.push_back({l.substr(0, sep), canonizaSinBarra(ruta)});
        }
    }
    return out;
}

char letraDeTipo(const std::filesystem::file_status& st) {
    using std::filesystem::file_type;
    switch (st.type()) {
        case file_type::directory:
            return 'd';
        case file_type::regular:
            return 'f';
        case file_type::symlink:
            return 'l';
        case file_type::block:
            return 'b';
        case file_type::character:
            return 'c';
        case file_type::fifo:
            return 'p';
        case file_type::socket:
            return 's';
        default:
            return 'o';
    }
}

zfsmgr::base::json::Value entradaDeRuta(const std::filesystem::path& p) {
    namespace J = zfsmgr::base::json;
    J::Value v;
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(p, ec);
    zfsmgr::remotetree::Entry en;
    std::string err;
    zfsmgr::remotetree::describePath(p.string(), en, err);
    std::string nombre = p.filename().string();
    if (nombre.empty()) {
        nombre = p.string();
    }
    v.set("name", J::Value(nombre));
    v.set("type", J::Value(std::string(1, ec ? 'o' : letraDeTipo(st))));
    v.set("size", J::Value(static_cast<long long>(en.size)));
    v.set("mtime", J::Value(static_cast<long long>(en.mtime)));
    v.set("mode", J::Value(static_cast<long long>(en.mode)));
    v.set("owner", J::Value(en.owner));
    v.set("group", J::Value(en.group));
    if (!en.target.empty()) {
        v.set("target", J::Value(en.target));
    }
    return v;
}

// `--dump-path-list <ruta>`: lo que hay en un directorio cualquiera. En Windows, «/» son
// las unidades: no hay una raíz común por encima de C: y D:.
ExecResult listaRuta(const std::string& ruta) {
    namespace J = zfsmgr::base::json;
    ExecResult r;
    J::Array entradas;
#ifdef _WIN32
    if (ruta.empty() || ruta == "/" || ruta == "\\") {
        const DWORD mascara = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (mascara & (1u << i)) {
                J::Value v;
                v.set("name", J::Value(std::string(1, static_cast<char>('A' + i)) + ":"));
                v.set("type", J::Value(std::string("d")));
                v.set("size", J::Value(0LL));
                entradas.push_back(v);
            }
        }
        J::Value raiz;
        raiz.set("entries", J::Value(entradas));
        r.rc = 0;
        r.out = J::toCompact(raiz) + "\n";
        return r;
    }
#endif
    std::error_code ec;
    if (!std::filesystem::is_directory(ruta, ec)) {
        r.rc = std::filesystem::exists(ruta, ec) ? 20 : 2;
        r.err = (r.rc == 20 ? "not a directory: " : "no such file or directory: ") + ruta + "\n";
        return r;
    }
    for (const auto& e : std::filesystem::directory_iterator(
             ruta, std::filesystem::directory_options::skip_permission_denied, ec)) {
        entradas.push_back(entradaDeRuta(e.path()));
    }
    if (ec) {
        r.rc = 1;
        ponCodigo(r, "cannot_read_dir", {ec.message()});
        return r;
    }
    J::Value raiz;
    raiz.set("entries", J::Value(entradas));
    r.rc = 0;
    r.out = J::toCompact(raiz) + "\n";
    return r;
}

ExecResult montaSoloLecturaTemporal(const std::string& dataset) {
    ExecResult r;
#if defined(_WIN32)
    (void)dataset;
    r.rc = 2;
    ponCodigo(r, "ro_mount_windows", {});
    return r;
#elif defined(__APPLE__)
    // En macOS NO: su `mount_zfs` ignora `-o ro` y el montaje queda en lectura-escritura
    // (medido en mbp: se escribió un fichero en un montaje pedido «ro»); `readonly` lo
    // rechaza y `mount -u -r` falla. Un montaje de solo lectura que escribe es peor que no
    // tenerlo.
    (void)dataset;
    r.rc = 2;
    ponCodigo(r, "ro_mount_macos", {});
    return r;
#else
    if (trim(dataset).empty() || dataset.find('@') != std::string::npos) {
        r.rc = 2;
        ponCodigo(r, "need_filesystem", {});
        return r;
    }
    const ExecResult tipo = getZfsPropertyCapture(dataset, "type");
    if (tipo.rc != 0) {
        return tipo;
    }
    if (trim(tipo.out) != "filesystem") {
        r.rc = 2;
        r.err = dataset + " is a " + trim(tipo.out) + ", not a filesystem\n";
        return r;
    }
    // Montado ya —por ZFS o por nosotros—: se dice dónde, y no se monta otra vez.
    if (datasetIsMounted(dataset)) {
        const ExecResult dondeYa = getDatasetMountpointCapture(dataset);
        r.rc = 0;
        r.out = "MOUNTPOINT=" + trim(dondeYa.out) + "\nTEMPORARY=0\n";
        return r;
    }
    std::string plantilla = std::string(kPrefijoMontajeRo) + "XXXXXX";
    std::vector<char> buf(plantilla.begin(), plantilla.end());
    buf.push_back('\0');
    const char* dir = mkdtemp(buf.data());
    if (!dir) {
        r.rc = 1;
        ponCodigo(r, "temp_dir_failed_errno", {std::strerror(errno)});
        return r;
    }
    const std::string directorio = dir;
#if defined(__linux__)
    const std::vector<std::string> args = {"-t", "zfs", "-o", "ro,zfsutil", dataset, directorio};
    const std::string programa = "mount";
#else
    const std::vector<std::string> args = {"-t", "zfs", "-o", "ro", dataset, directorio};
    const std::string programa = "mount";
#endif
    const ExecResult m = runExecCapture(programa, args);
    if (m.rc != 0) {
        rmdir(directorio.c_str());
        r.rc = m.rc;
        ponCodigo(r, "ro_mount_failed", {dataset, trim(m.err.empty() ? m.out : m.err)});
        return r;
    }
    daemonLog("INFO", "read-only temporary mount of " + dataset + " at " + directorio);
    r.rc = 0;
    r.out = "MOUNTPOINT=" + directorio + "\nTEMPORARY=1\n";
    return r;
#endif
}

ExecResult desmontaSoloLecturaTemporal(const std::string& directorio) {
    ExecResult r;
#ifdef _WIN32
    (void)directorio;
    r.rc = 2;
    ponCodigo(r, "ro_mount_windows", {});
    return r;
#else
    // Solo lo que montó el verbo de arriba: con esto no se puede desmontar cualquier cosa.
    if (directorio.rfind(kPrefijoMontajeRo, 0) != 0 || directorio.find("..") != std::string::npos
        || directorio.find('/', std::strlen(kPrefijoMontajeRo)) != std::string::npos) {
        r.rc = 2;
        r.err = "not a temporary read-only mount of ours: " + directorio + "\n";
        return r;
    }
    const ExecResult u = runExecCapture("umount", {directorio});
    if (u.rc != 0) {
        // Ocupado —un trabajo de copia que todavía lee de ahí—: se dice, y el directorio se
        // queda; quien llamó puede reintentar.
        r.rc = u.rc;
        r.err = "cannot unmount " + directorio + ": " + trim(u.err.empty() ? u.out : u.err) + "\n";
        return r;
    }
    rmdir(directorio.c_str());
    daemonLog("INFO", "read-only temporary mount at " + directorio + " released");
    r.rc = 0;
    r.out = "UNMOUNTED=" + directorio + "\n";
    return r;
#endif
}

ExecResult describeRuta(const std::string& ruta) {
    namespace J = zfsmgr::base::json;
    ExecResult r;
    J::Value v;
    v.set("path", J::Value(ruta));
    std::error_code ec;
    const bool existe = std::filesystem::exists(std::filesystem::symlink_status(ruta, ec));
    v.set("exists", J::Value(existe && !ec));
    // Siguiendo enlaces: un enlace a un directorio es un sitio al que se puede entrar.
    std::error_code ecDir;
    v.set("is_dir", J::Value(std::filesystem::is_directory(ruta, ecDir) && !ecDir));
    if (existe && !ec) {
        J::Value e = entradaDeRuta(std::filesystem::path(ruta));
        for (const char* k : {"type", "size", "mtime", "mode", "owner", "group", "target"}) {
            if (e.contains(k)) {
                v.set(k, e[k]);
            }
        }
    }
    // El montaje más largo que la contiene, comparando POR COMPONENTES: «/mnt/datos2» no
    // está dentro de «/mnt/datos».
    const std::filesystem::path pedida = canonizaSinBarra(ruta);
    const MontajeZfs* mejor = nullptr;
    std::size_t mejorLargo = 0;
    std::vector<MontajeZfs> montajes = montajesZfs();
    for (const MontajeZfs& m : montajes) {
        auto itB = m.ruta.begin();
        auto itP = pedida.begin();
        std::size_t largo = 0;
        bool dentro = true;
        for (; itB != m.ruta.end(); ++itB, ++itP, ++largo) {
            if (itP == pedida.end() || *itB != *itP) {
                dentro = false;
                break;
            }
        }
        if (dentro && (!mejor || largo > mejorLargo)) {
            mejor = &m;
            mejorLargo = largo;
        }
    }
    if (mejor) {
        std::vector<std::string> resto;
        auto itP = pedida.begin();
        for (std::size_t i = 0; i < mejorLargo && itP != pedida.end(); ++i) {
            ++itP;
        }
        for (; itP != pedida.end(); ++itP) {
            if (!itP->empty()) {
                resto.push_back(itP->string());
            }
        }
        // Dentro de `.zfs/snapshot/<s>`, lo que hay es la instantánea, no el dataset.
        std::string instantanea;
        if (resto.size() >= 3 && resto[0] == ".zfs" && resto[1] == "snapshot") {
            instantanea = resto[2];
            resto.erase(resto.begin(), resto.begin() + 3);
        }
        std::string relativa;
        for (const std::string& t : resto) {
            relativa += (relativa.empty() ? "" : "/") + t;
        }
        v.set("dataset", J::Value(mejor->dataset));
        v.set("mountpoint", J::Value(mejor->ruta.generic_string()));
        v.set("relative", J::Value(relativa));
        v.set("snapshot", J::Value(instantanea));
        v.set("exact_mountpoint", J::Value(relativa.empty()));
    }
    r.rc = 0;
    r.out = J::toCompact(v) + "\n";
    return r;
}

// Un fichero, en base64 y con tope.
//
// En base64 porque la respuesta del RPC viaja como LÍNEAS recortadas: unos bytes crudos
// llegarían mutilados. Y con tope porque esto se lee entero en memoria: lo que no quepa
// tiene el camino de transfer, que para eso existe.
// Un TROZO del fichero, o el fichero entero si no se pide trozo.
//
// El tope de 8 MiB seguía en pie y era el motivo de que WebDAV no sirviera para nada: una
// imagen de 726 MB —o de 50 GB, que también las hay— no cabía, así que el explorador y el
// navegador no podían abrir un solo fichero de verdad. Un protocolo de ficheros que no
// sirve ficheros no es un protocolo de ficheros.
//
// Con `desde` y `cuanto` se lee solo esa parte, así que ni el daemon ni quien lo llama
// tienen que sostener el fichero entero en memoria. La respuesta empieza por `SIZE=<total>`
// para que quien pide el primer trozo sepa ya cuántos le faltan: sin eso haría falta una
// llamada aparte solo para preguntar el tamaño.
ExecResult leeFichero(const std::string& ruta, std::uintmax_t desde, std::uintmax_t cuanto) {
    ExecResult r;
    constexpr std::uintmax_t kTope = 8ull * 1024 * 1024;
    std::error_code ec;
    const std::uintmax_t tam = std::filesystem::file_size(ruta, ec);
    if (ec) {
        r.rc = 1;
        ponCodigo(r, "cannot_stat", {ec.message()});
        return r;
    }
    if (cuanto > 0) {
        if (cuanto > kTope) {
            r.rc = 2;
            ponCodigo(r, "chunk_too_big", {});
            return r;
        }
        std::ifstream f(ruta, std::ios::binary);
        if (!f) {
            r.rc = 1;
            ponCodigo(r, "cannot_open_file", {});
            return r;
        }
        f.seekg(static_cast<std::streamoff>(desde));
        std::string buf;
        buf.resize(static_cast<std::size_t>(cuanto));
        f.read(&buf[0], static_cast<std::streamsize>(cuanto));
        buf.resize(static_cast<std::size_t>(f.gcount()));
        r.rc = 0;
        r.out = "SIZE=" + std::to_string(tam) + "\n" + zfsmgr::base::base64Encode(buf) + "\n";
        return r;
    }
    if (tam > kTope) {
        r.rc = 27;
        ponCodigo(r, "file_too_big", {});
        return r;
    }
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        r.rc = 1;
        ponCodigo(r, "cannot_open_file", {});
        return r;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    r.rc = 0;
    r.out = zfsmgr::base::base64Encode(ss.str()) + "\n";
    return r;
}

// Returns the snapshot classes that are due given the retention counts and time.
// Qué clases TOCAN ahora para este dataset.
//
// Antes se decidía por la hora del reloj: la diaria «si son las 00», la semanal «si son
// las 00 del domingo». Eso obliga a que el planificador dispare EXACTAMENTE en esa hora, y
// si la máquina estaba apagada a medianoche —o el daemon se reinició a las 00:05— la
// diaria de ese día no se hacía nunca y nada lo decía.
//
// Ahora se compara el PERIODO: si no hay ninguna instantánea de esa clase dentro del
// periodo en curso, toca. Con eso, una máquina que se enciende a las 09:00 recupera su
// diaria al arrancar, y una que lleva días encendida no hace ninguna de más. El estado son
// las propias instantáneas, así que no hay fichero que se corrompa ni que se desincronice.
std::vector<std::string> gsaDueClasses(const std::string& ds, int hourly, int daily, int weekly,
                                       int monthly, int yearly, std::time_t ahora) {
    const std::pair<const char*, int> clases[] = {
        {"hourly", hourly},   {"daily", daily},   {"weekly", weekly},
        {"monthly", monthly}, {"yearly", yearly},
    };
    std::vector<std::string> due;
    for (const auto& c : clases) {
        if (c.second <= 0) {
            continue;
        }
        const std::string periodo = gsaClaveDePeriodo(c.first, ahora);
        const ExecResult r =
            runExecCapture("zfs", {"list", "-H", "-t", "snapshot", "-o", "name", "-d", "1", ds});
        bool yaHay = false;
        if (r.rc == 0) {
            const std::string prefijo = ds + "@";
            for (const std::string& l : splitLines(r.out)) {
                const std::string s = trim(l);
                if (s.rfind(prefijo, 0) != 0) {
                    continue;
                }
                std::time_t cuando = 0;
                if (gsaInstanteDeNombre(s.substr(prefijo.size()), c.first, cuando)
                    && gsaClaveDePeriodo(c.first, cuando) == periodo) {
                    yaHay = true;
                    break;
                }
            }
        }
        if (!yaHay) {
            due.push_back(c.first);
        }
    }
    return due;
}

std::string gsaPropValue(const std::string& ds, const std::string& prop, bool localOnly) {
    std::vector<std::string> args = {"get", "-H", "-o", "value"};
    if (localOnly) {
        args.push_back("-s");
        args.push_back("local");
    }
    args.push_back(prop);
    args.push_back(ds);
    const ExecResult r = runExecCapture("zfs", args);
    if (r.rc != 0)
        return {};
    const std::string val = trim(r.out);
    return (val == "-") ? std::string() : val;
}

thread_local GsaFallo t_gsaFalloPar;

// Los snapshots de UN dataset (sin descendientes), del más viejo al más nuevo, con su guid.
//
// `-s createtxg` y no `creation`: dos snapshots del mismo segundo —una horaria y una diaria
// a medianoche— tienen el mismo `creation`, y el orden entre ellos decide cuál es la base.
bool gsaSnapshotsDe(const std::string& ds, std::vector<zfsmgr::base::gsa::SnapRef>& out,
                    std::string& error) {
    out.clear();
    const ExecResult r = runExecCapture("zfs", {"list", "-H", "-p", "-t", "snapshot", "-o",
                                                "name,guid", "-s", "createtxg", "-d", "1", ds});
    if (r.rc != 0) {
        error = trim(r.err).empty() ? "zfs list failed" : trim(r.err);
        return false;
    }
    out = zfsmgr::base::gsa::parseSnapRefs(r.out, ds);
    return true;
}

std::vector<std::string> gsaNombres(const std::vector<zfsmgr::base::gsa::SnapRef>& s) {
    std::vector<std::string> n;
    n.reserve(s.size());
    for (const auto& x : s) {
        n.push_back(x.name);
    }
    return n;
}

// ¿Ha cambiado algo desde `previo`? `written@<previo>` es lo escrito desde entonces, y ZFS
// ya lo lleva contado: 0 en el dataset —y en una recursiva, en TODOS sus descendientes— es
// que un snapshot nuevo guardaría exactamente lo mismo. Ante cualquier duda (un hijo sin
// ese snapshot da «-», un error, un valor que no se entiende) la respuesta es «sí ha
// cambiado»: lo malo de equivocarse por ese lado es un snapshot de más.
bool gsaSinCambiosDesde(const std::string& ds, const std::string& previo, bool recursive) {
    std::vector<std::string> args = {"get", "-H", "-p", "-o", "value"};
    if (recursive) {
        args.push_back("-r");
        args.push_back("-t");
        args.push_back("filesystem,volume");
    }
    args.push_back("written@" + previo);
    args.push_back(ds);
    const ExecResult r = runExecCapture("zfs", args);
    if (r.rc != 0) {
        return false;
    }
    bool alguno = false;
    for (const std::string& l : splitLines(r.out)) {
        const std::string v = trim(l);
        if (v.empty()) {
            continue;
        }
        if (v != "0") {
            return false;
        }
        alguno = true;
    }
    return alguno;
}

// El snapshot de la clase que toca.
//
// The snapshots of `ds` another machine holds as the base of the copy it pulls (holds tagged
// gsa::kPullHoldPrefix). They are neither pruned nor renamed: that machine may be down for
// longer than the retention, or reading one right now. `userrefs` first, so only the few
// snapshots with any hold are asked about. If it cannot be asked, the answer is «none», which
// is today's behaviour — and ZFS itself refuses to destroy a held snapshot anyway.
std::set<std::string> gsaRetenidosPorPares(const std::string& ds, bool recursive) {
    std::vector<std::string> args = {"get", "-H", "-o", "name,value", "-t", "snapshot"};
    if (recursive) {
        args.push_back("-r");
    } else {
        args.insert(args.end(), {"-d", "1"});
    }
    args.push_back("userrefs");
    args.push_back(ds);
    const ExecResult refs = runExecCapture("zfs", args);
    if (refs.rc != 0) {
        return {};
    }
    std::vector<std::string> conHold = {"holds", "-H"};
    for (const std::string& l : splitLines(refs.out)) {
        const std::size_t tab = l.find('\t');
        if (tab != std::string::npos && trim(l.substr(tab + 1)) != "0") {
            conHold.push_back(trim(l.substr(0, tab)));
        }
    }
    if (conHold.size() == 2) {
        return {};
    }
    const ExecResult holds = runExecCapture("zfs", conHold);
    return zfsmgr::base::gsa::heldByPullers(holds.out, ds, recursive);
}

// Si el dataset no ha cambiado desde el último de ESA MISMA clase, no se hace uno nuevo: se
// RENOMBRA aquel al nombre que tocaría. El periodo queda cubierto —que es lo que mira
// `gsaDueClasses`—, la retención no gasta una plaza en algo que no aporta nada y el snapshot
// sigue guardando los mismos datos. La misma clase y no «el anterior» a secas: renombrar la
// horaria de las 00:00 para hacer la diaria se la quitaría a las horarias.
//
// En el destino de la nivelación el renombrado se repite solo, porque la base se busca por
// guid (`planLevel`).
GsaPaso gsaHazSnapshot(const std::string& ds, const std::string& klass, bool recursive,
                       bool permitirRenombrar, std::string& nombre, const GsaLog& log) {
    GsaPaso paso;
    paso.hecho = true;
    nombre = "GSA-" + klass + "-" + gsaLocalTimestamp();
    std::vector<zfsmgr::base::gsa::SnapRef> snaps;
    std::string err;
    if (!gsaSnapshotsDe(ds, snaps, err)) {
        paso.falla("gsa_list_snapshots_failed", {ds, err});
        log("ERROR", "GSA snapshot error for " + ds + ": " + paso.error);
        return paso;
    }
    for (const auto& s : snaps) {
        if (s.name == nombre) {
            log("INFO", "GSA snapshot skip for " + ds + ": " + nombre + " already exists");
            return paso;
        }
    }
    const std::string previo = zfsmgr::base::gsa::newestOfClass(gsaNombres(snaps), klass);
    // Con una nivelación en marcha no se renombra: `zfs send -I` puede estar leyendo ese
    // snapshot. Se hace uno normal, que no estorba a nadie.
    // Nor when another machine holds it: it is that machine's base, and it finds it by guid
    // but releases its hold by name.
    if (permitirRenombrar && !previo.empty()
        && gsaRetenidosPorPares(ds, recursive).count(previo) == 0
        && gsaSinCambiosDesde(ds, previo, recursive)) {
        std::vector<std::string> args = {"rename"};
        if (recursive) {
            args.push_back("-r");
        }
        args.push_back(ds + "@" + previo);
        args.push_back(ds + "@" + nombre);
        const ExecResult r = runExecCapture("zfs", args);
        if (r.rc == 0) {
            log("INFO", "GSA snapshot unchanged for " + ds + ": " + previo + " -> " + nombre);
            return paso;
        }
        // Si no se pudo renombrar, se hace el snapshot de siempre: mejor uno de más que
        // un periodo sin cubrir.
        log("WARN", "GSA snapshot rename failed for " + ds + "@" + previo + ": " + trim(r.err)
                        + "; a new snapshot is taken instead");
    }
    std::vector<std::string> args = {"snapshot"};
    if (recursive) {
        args.push_back("-r");
    }
    args.push_back(ds + "@" + nombre);
    const ExecResult r = runExecCapture("zfs", args);
    if (r.rc != 0) {
        paso.falla("gsa_snapshot_failed",
                   {ds, trim(r.err).empty() ? "exit " + std::to_string(r.rc) : trim(r.err)});
        log("ERROR", "GSA snapshot error for " + ds + ": " + paso.error);
        nombre.clear();
        return paso;
    }
    log("INFO", "GSA snapshot created for " + ds + ": " + nombre);
    return paso;
}

// La poda de una clase en el ORIGEN. La base de la nivelación —su guid lo guarda el estado
// tras cada nivelación buena— no se borra aunque quede fuera de la retención: con el destino
// caído más tiempo que la retención, borrarla rompería para siempre la cadena incremental.
GsaPaso gsaPoda(const std::string& ds, const std::string& klass, int keep, bool recursive,
                const std::string& guidProtegido, int& borrados, bool& baseConservada,
                const GsaLog& log) {
    GsaPaso paso;
    if (keep <= 0) {
        return paso;
    }
    std::vector<zfsmgr::base::gsa::SnapRef> snaps;
    std::string err;
    if (!gsaSnapshotsDe(ds, snaps, err)) {
        paso.hecho = true;
        paso.falla("gsa_list_snapshots_failed", {ds, err});
        log("ERROR", "GSA prune error for " + ds + ": " + paso.error);
        return paso;
    }
    std::string protegido;
    for (const auto& s : snaps) {
        if (!guidProtegido.empty() && s.guid == guidProtegido) {
            protegido = s.name;
        }
    }
    // Protected: this machine's own levelling base, and every base another machine holds.
    std::set<std::string> protegidos = gsaRetenidosPorPares(ds, recursive);
    if (!protegido.empty()) {
        protegidos.insert(protegido);
    }
    const zfsmgr::base::gsa::PrunePlan plan =
        zfsmgr::base::gsa::planPrune(gsaNombres(snaps), klass, keep, protegidos);
    for (const std::string& n : plan.kept) {
        baseConservada = true;
        log("WARN", "GSA prune of " + ds + ": " + n + " is beyond the retention but kept, "
                        + (n == protegido ? std::string("it is the base of the levelling")
                                          : std::string("another machine holds it as its base")));
    }
    for (const std::string& n : plan.destroy) {
        paso.hecho = true;
        std::vector<std::string> args = {"destroy"};
        if (recursive) {
            args.push_back("-r");
        }
        args.push_back(ds + "@" + n);
        const ExecResult r = runExecCapture("zfs", args);
        if (r.rc != 0) {
            paso.falla("gsa_destroy_failed", {ds + "@" + n, trim(r.err)});
            log("ERROR", "GSA prune error for " + ds + "@" + n + ": " + trim(r.err));
        } else {
            ++borrados;
            log("INFO", "GSA pruned " + ds + "@" + n);
        }
    }
    return paso;
}

// Run a command on the SSH target, return combined exit code.

// ── Los PARES del daemon ────────────────────────────────────────────────────────────
//
// Para nivelar contra otra máquina hay que llegar hasta su daemon, y eso pide credenciales
// que el daemon no tiene por sí mismo: cada uno verifica a sus clientes contra SU propio
// `client.crt`, así que para hablar con la máquina B hay que presentar el certificado de
// cliente de B. Es lo mismo que hace la aplicación, que guarda esa terna por conexión en
// su almacén de confianza.
//
// Esto sustituye a `gsa-connections.conf`, que guardaba usuario, host, clave SSH y
// contraseña en claro, y con lo que se abría un `ssh` por cada operación.
//
// El fichero lo escribe el cliente con `--mutate-set-peers`. Va con permisos 600 y de root
// porque lleva claves privadas dentro; el mismo trato que el material TLS de al lado.
#ifdef _WIN32
#else
#endif

// El nombre de conexión con el que ESTA máquina se conoce a sí misma, según el cliente.
//
// Sustituye a `SELF_CONNECTION` de `gsa-connections.conf`. Sirve para lo único que hacía
// falta: distinguir «nivela contra otra máquina» de «nivela contra un dataset de aquí».
// Sin él, un nombre de conexión mal escrito se tomaría por local y la nivelación acabaría
// en un dataset de esta máquina en vez de fallar, que es lo que tiene que hacer.
// ¿El destino de una nivelación soy YO? Por identificador de MÁQUINA, no por etiqueta.
//
// Antes se comparaba el nombre escrito en la propiedad del dataset con el nombre que un
// cliente dijo que tenía esta máquina. Dos etiquetas, ninguna identidad: `local` significa
// una máquina distinta según quién pregunte —fc16 se llama «local» a sí misma y mmela
// también— y la misma máquina aparece como «unib» en una lista y «unibody» en otra. Dos
// clientes distintos podían dejar el sistema creyendo que el destino es otro y mandando por
// la red un envío que no tenía que salir de aquí.
//
// Ahora: se busca el nombre entre los pares, se saca SU uid, y se compara con el que esta
// máquina lee de sí misma. La identidad no es lo que me contaron, es lo que soy.
//
// El camino viejo se conserva como RESPALDO y a propósito: un `peers.json` escrito por un
// cliente anterior no lleva uid, y entonces la comparación por nombre es lo único que hay.
// Quitarlo convertiría una actualización del cliente en una nivelación que deja de ocurrir
// sin decir nada.
bool gsaDestinoSoyYo(const std::string& connId, const std::string& selfPorNombre) {
    // La REGLA no está aquí: está en `base::peers::destinationIsSelf`, con pruebas. Aquí
    // solo se leen los datos del fichero y se le pasan. De esa regla depende que un envío
    // salga por la red o se quede en casa, y enterrarla en esta función —que ya tiene mil
    // líneas— es donde se esconden los fallos que nadie ve.
    zfsmgr::base::peers::SelfMatch m;
    m.selfName = selfPorNombre;
    m.realUid = trim(detectMachineUuid());
    std::ifstream f(kPeersPath);
    if (f.is_open()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        zfsmgr::base::json::Value raiz;
        std::string err;
        if (zfsmgr::base::json::parse(ss.str(), raiz, &err)) {
            m.selfUid = trim(raiz["self_uid"].toString());
            if (m.selfName.empty()) {
                m.selfName = trim(raiz["self"].toString());
            }
            for (const auto& v : raiz["peers"].toArray()) {
                zfsmgr::base::peers::Peer par;
                par.id = trim(v["id"].toString());
                par.uid = trim(v["uid"].toString());
                m.peers.push_back(par);
            }
        }
    }
    return zfsmgr::base::peers::destinationIsSelf(connId, m);
}

std::string gsaYoMismo() {
    std::ifstream f(kPeersPath);
    if (!f.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    zfsmgr::base::json::Value raiz;
    std::string err;
    if (!zfsmgr::base::json::parse(ss.str(), raiz, &err)) {
        return {};
    }
    return trim(raiz["self"].toString());
}

bool gsaCargaPeer(const std::string& id, GsaPeer& out) {
    std::ifstream f(kPeersPath);
    if (!f.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    zfsmgr::base::json::Value raiz;
    std::string err;
    if (!zfsmgr::base::json::parse(ss.str(), raiz, &err)) {
        return false;
    }
    for (const auto& v : raiz["peers"].toArray()) {
        if (toLower(trim(v["id"].toString())) != toLower(trim(id))) {
            continue;
        }
        out.id = trim(v["id"].toString());
        out.host = trim(v["host"].toString());
        out.port = static_cast<int>(v["port"].toInt(0));
        out.serverCertPem = v["server_cert_pem"].toString();
        out.clientCertPem = v["client_cert_pem"].toString();
        out.clientKeyPem = v["client_key_pem"].toString();
        return !out.host.empty() && out.port > 0 && !out.serverCertPem.empty()
               && !out.clientCertPem.empty() && !out.clientKeyPem.empty();
    }
    return false;
}

GsaRpc gsaPeerRpc(const GsaPeer& peer, const std::string& cmd, const std::vector<std::string>& args,
                  std::string& salida, std::string& error) {
    zfsmgr::base::TlsClientConfig cfg;
    cfg.host = peer.host;
    cfg.port = peer.port;
    cfg.serverCertPem = peer.serverCertPem;
    cfg.clientCertPem = peer.clientCertPem;
    cfg.clientKeyPem = peer.clientKeyPem;
    zfsmgr::base::json::Value req;
    req.set("cmd", zfsmgr::base::json::Value(cmd));
    zfsmgr::base::json::Array ja;
    for (const std::string& a : args) {
        ja.push_back(zfsmgr::base::json::Value(a));
    }
    req.set("args", zfsmgr::base::json::Value(std::move(ja)));
    std::string respuesta;
    std::string errTls;
    if (!zfsmgr::base::tlsRequestLine(cfg, zfsmgr::base::json::toCompact(req), respuesta, errTls)) {
        t_gsaFalloPar = {"peer_unreachable",
                         {peer.id, peer.host + ":" + std::to_string(peer.port), trim(errTls)}};
        error = zfsmgr::base::agentmsg::inEnglish(t_gsaFalloPar.codigo, t_gsaFalloPar.args);
        return GsaRpc::NoLlega;
    }
    zfsmgr::base::json::Value resp;
    std::string errJson;
    if (!zfsmgr::base::json::parse(respuesta, resp, &errJson)) {
        t_gsaFalloPar = {"peer_bad_answer", {peer.id, errJson}};
        error = zfsmgr::base::agentmsg::inEnglish(t_gsaFalloPar.codigo, t_gsaFalloPar.args);
        return GsaRpc::NoLlega;
    }
    const int rc = static_cast<int>(resp["rc"].toInt(1));
    salida = resp["stdout"].toString();
    if (rc != 0) {
        error = trim(resp["stderr"].toString());
        // Si el par contesta con código (agentmsg), se conserva tal cual.
        if (!resp["code"].toString().empty()) {
            std::vector<std::string> a;
            for (const auto& x : resp["args"].toArray())
                a.push_back(x.toString());
            t_gsaFalloPar = {resp["code"].toString(), a};
            error = zfsmgr::base::agentmsg::inEnglish(t_gsaFalloPar.codigo, t_gsaFalloPar.args);
        } else if (!error.empty()) {
            t_gsaFalloPar = {"text", {error}};
        } else {
            t_gsaFalloPar = {"peer_refused", {peer.id, std::to_string(rc)}};
            error = zfsmgr::base::agentmsg::inEnglish(t_gsaFalloPar.codigo, t_gsaFalloPar.args);
        }
        return GsaRpc::Rechaza;
    }
    return GsaRpc::Ok;
}

// `zfs send | zfs recv` en la misma máquina, pero con el daemon EN MEDIO: lee de uno, cuenta y
// escribe en el otro. Es lo que necesita la nivelación del planificador para ser un trabajo
// —bytes, velocidad, cancelación—; `ejecutaTuberiaZfsLocal` los une directamente y no ve
// nada de lo que pasa.
//
// `recv` sigue leyendo de una TUBERÍA, que es lo que exige macOS (de un fichero o de un
// socket se porta mal). Cancelar es dejar de bombear: se termina `send` y se cierra la
// entrada de `recv`, que falla —y con `-s` deja el testigo para reanudar—.
#ifndef _WIN32
ExecResult ejecutaTuberiaZfsLocalConAvance(const std::vector<std::string>& sendArgv,
                                           const std::vector<std::string>& recvArgv,
                                           const TransferProgressFn& avance) {
    ExecResult r;
    // Las tres tuberías, sin herencia desde que nacen: otro hilo puede hacer fork entre
    // crearlas y marcarlas, y un hijo ajeno que se queda con un extremo impide el EOF.
    int deSend[2] = {-1, -1};
    int aRecv[2] = {-1, -1};
    int errores[2] = {-1, -1};
    const auto creaTubo = [](int t[2]) {
#if defined(__linux__) || defined(__FreeBSD__)
        return pipe2(t, O_CLOEXEC) == 0;
#else
        if (pipe(t) != 0)
            return false;
        fcntl(t[0], F_SETFD, FD_CLOEXEC);
        fcntl(t[1], F_SETFD, FD_CLOEXEC);
        return true;
#endif
    };
    const auto cierra = [](int& fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };
    if (!creaTubo(deSend) || !creaTubo(aRecv) || !creaTubo(errores)) {
        cierra(deSend[0]);
        cierra(deSend[1]);
        cierra(aRecv[0]);
        cierra(aRecv[1]);
        cierra(errores[0]);
        cierra(errores[1]);
        r.rc = 125;
        ponCodigo(r, "internal", {"pipe failed"});
        return r;
    }
    const auto argvDe = [](const std::vector<std::string>& v) {
        std::vector<char*> a;
        a.push_back(const_cast<char*>("zfs"));
        for (const std::string& x : v)
            a.push_back(const_cast<char*>(x.c_str()));
        a.push_back(nullptr);
        return a;
    };
    std::vector<char*> sendA = argvDe(sendArgv);
    std::vector<char*> recvA = argvDe(recvArgv);
    // En el hijo, dup2 quita el CLOEXEC del descriptor nuevo: sus 0/1/2 sobreviven al exec y
    // lo demás se cierra solo.
    const pid_t sendPid = fork();
    if (sendPid == 0) {
        dup2(deSend[1], STDOUT_FILENO);
        dup2(errores[1], STDERR_FILENO);
        execvp("zfs", sendA.data());
        _exit(127);
    }
    const pid_t recvPid = sendPid > 0 ? fork() : -1;
    if (recvPid == 0) {
        dup2(aRecv[0], STDIN_FILENO);
        dup2(errores[1], STDERR_FILENO);
        execvp("zfs", recvA.data());
        _exit(127);
    }
    cierra(deSend[1]);
    cierra(aRecv[0]);
    cierra(errores[1]);
    if (sendPid < 0 || recvPid < 0) {
        if (sendPid > 0) {
            kill(sendPid, SIGTERM);
            waitpid(sendPid, nullptr, 0);
        }
        cierra(deSend[0]);
        cierra(aRecv[1]);
        cierra(errores[0]);
        r.rc = 125;
        ponCodigo(r, "internal", {"fork failed"});
        return r;
    }

    std::string errAcum;
    std::vector<char> buf(256 * 1024);
    std::uint64_t total = 0;
    bool cancelado = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto ultimo = t0;
    while (deSend[0] >= 0 || errores[0] >= 0) {
        pollfd p[2];
        int n = 0;
        int iDatos = -1;
        int iErr = -1;
        if (deSend[0] >= 0) {
            p[n] = {deSend[0], POLLIN, 0};
            iDatos = n++;
        }
        if (errores[0] >= 0) {
            p[n] = {errores[0], POLLIN, 0};
            iErr = n++;
        }
        if (::poll(p, static_cast<nfds_t>(n), 1000) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (iErr >= 0 && (p[iErr].revents & (POLLIN | POLLHUP | POLLERR))) {
            char e[4096];
            const ssize_t k = read(errores[0], e, sizeof(e));
            if (k > 0)
                errAcum.append(e, static_cast<std::size_t>(k));
            else if (k == 0 || errno != EINTR)
                cierra(errores[0]);
        }
        if (iDatos >= 0 && (p[iDatos].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t k = read(deSend[0], buf.data(), buf.size());
            if (k > 0) {
                std::size_t hecho = 0;
                while (hecho < static_cast<std::size_t>(k)) {
                    const ssize_t w =
                        write(aRecv[1], buf.data() + hecho, static_cast<std::size_t>(k) - hecho);
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        break;  // recv murió: se ve en su código de salida
                    }
                    hecho += static_cast<std::size_t>(w);
                }
                total += hecho;
                if (hecho < static_cast<std::size_t>(k)) {
                    cierra(deSend[0]);
                    cierra(aRecv[1]);
                }
            } else if (k == 0 || errno != EINTR) {
                cierra(deSend[0]);
                cierra(aRecv[1]);  // EOF para recv: el flujo ha terminado
            }
        }
        const auto ahora = std::chrono::steady_clock::now();
        if (avance && (ahora - ultimo >= std::chrono::seconds(1) || deSend[0] < 0)) {
            ultimo = ahora;
            const double segs = std::chrono::duration<double>(ahora - t0).count();
            const double mibs =
                segs > 0 ? (static_cast<double>(total) / (1024.0 * 1024.0)) / segs : 0.0;
            if (!avance(total, static_cast<long>(segs), mibs) && deSend[0] >= 0) {
                cancelado = true;
                kill(sendPid, SIGTERM);
                cierra(deSend[0]);
                cierra(aRecv[1]);
            }
        }
    }
    cierra(deSend[0]);
    cierra(aRecv[1]);
    cierra(errores[0]);
    int stSend = 0;
    int stRecv = 0;
    waitpid(sendPid, &stSend, 0);
    waitpid(recvPid, &stRecv, 0);
    const int rcSend = decodeWaitStatus(stSend);
    const int rcRecv = decodeWaitStatus(stRecv);
    r.err = errAcum;
    r.rc = cancelado ? 130 : (rcRecv != 0 ? rcRecv : rcSend);
    if (cancelado) {
        r.err = "cancelled\n" + r.err;
    }
    return r;
}
#else
ExecResult ejecutaTuberiaZfsLocalConAvance(const std::vector<std::string>&,
                                           const std::vector<std::string>&,
                                           const TransferProgressFn&) {
    ExecResult r;
    r.rc = 2;
    return fallo(2, "local_pipe_windows", {});
}
#endif

// ¿Hay que dejarlo? El trabajo que corre en este hilo se canceló, o el daemon se para.
//
// Se pregunta DESPUÉS de cada envío porque cortar por la función de avance no siempre sale
// como error: el relé remoto lo da por terminado con 0, y sin esto un envío cancelado se
// anotaba «done» y seguía hasta podar el destino.
bool gsaParado() {
    return g_stop.load() || jobCancelRequested();
}

// Descartar una recepción a medias en el destino (`zfs recv -A`).
bool enDestinoAborta(bool isLocal, const GsaPeer& peer, const std::string& dataset,
                     std::string& error) {
    if (isLocal) {
        const ExecResult r = runExecCapture("zfs", {"recv", "-A", dataset});
        error = trim(r.err);
        return r.rc == 0;
    }
    std::string salida;
    return gsaPeerRpc(peer, "--mutate-zfs-recv-abort", {dataset}, salida, error) == GsaRpc::Ok;
}

// Un flujo de `zfs send` al destino, en la misma máquina o en la otra, con el avance y la
// cancelación de `avance`. `sendArgv` es el argv de `zfs send` sin el «zfs»: completo, o
// «send -t <testigo>» para continuar uno cortado.
//
// El receptor recibe SIEMPRE con `-s`: un corte deja un testigo para continuar en vez de
// obligar a empezar de cero.
ExecResult gsaEnviaFlujo(bool isLocal, const GsaPeer& peer, const std::string& conexion,
                         const std::vector<std::string>& sendArgv, const std::string& recvEn,
                         bool incremental, const std::string& testigoReanudar,
                         const TransferProgressFn& avance) {
    if (isLocal) {
        std::vector<std::string> recvArgv = {"recv", "-u", "-s"};
        if (incremental) {
            recvArgv.push_back("-F");
        }
        // The source's schedule does not travel to the target (gsa::scheduleProperties).
        for (const std::string& prop : zfsmgr::base::gsa::scheduleProperties()) {
            recvArgv.push_back("-x");
            recvArgv.push_back(prop);
        }
        recvArgv.push_back(recvEn);
        return ejecutaTuberiaZfsLocalConAvance(sendArgv, recvArgv, avance);
    }
    ExecResult r;
    std::string respuesta;
    std::string errPeer;
    if (gsaPeerRpc(peer, "--zfs-recv-listen", {recvEn, incremental ? "1" : "0", "noschedule"},
                   respuesta, errPeer)
        != GsaRpc::Ok) {
        return fallo(1, "peer_cannot_listen", {conexion, errPeer});
    }
    std::string puerto;
    std::string testigo;
    for (const std::string& l : splitLines(respuesta)) {
        const std::string x = trim(l);
        if (x.rfind("PORT=", 0) == 0)
            puerto = x.substr(5);
        if (x.rfind("TOKEN=", 0) == 0)
            testigo = x.substr(6);
    }
    if (puerto.empty() || testigo.empty()) {
        return fallo(1, "peer_no_port", {conexion});
    }
    // runZfsSendToPeerCapture recibe: snap host puerto testigo [base [banderas [reanudar]]].
    // Se le da el argv ya partido: el snapshot es el último, y lo de en medio son banderas
    // y la base de «-I».
    if (!testigoReanudar.empty()) {
        return runZfsSendToPeerCapture({std::string(), peer.host, puerto, testigo, std::string(),
                                        std::string(), testigoReanudar},
                                       avance);
    }
    std::string banderas;
    std::string base;
    for (std::size_t i = 1; i + 1 < sendArgv.size(); ++i) {
        if (sendArgv[i] == "-I" && i + 2 < sendArgv.size()) {
            base = sendArgv[++i];
            continue;
        }
        banderas += (banderas.empty() ? "" : " ") + sendArgv[i];
    }
    return runZfsSendToPeerCapture({sendArgv.back(), peer.host, puerto, testigo, base, banderas},
                                   avance);
}

// Nivelar: dejar el destino con los mismos snapshots que el origen.
//
// Se decide con `planLevel` (capa base, con pruebas), comparando los snapshots de los dos
// lados POR GUID:
//   - renombra en el destino los que el origen renombró (periodos sin cambios);
//   - manda con `-I` desde la base común hasta el más nuevo del origen, con lo que viajan
//     también los intermedios —antes `-i` dejaba fuera los que no llegaron en su día—;
//   - poda en el destino los `GSA-` que el origen ya no tiene, a la vez que el origen, y nunca
//     uno que no sea nuestro;
//   - y si el destino tiene, por detrás de la base, snapshots que el origen no tiene, NO manda
//     nada: `recv -F` los destruiría. Se dice cuáles.
GsaNivelado gsaNivela(const std::string& ds, bool recursive, const std::string& dstSpec,
                      const GsaTargetConn& target, const TransferProgressFn& avance,
                      const GsaLog& log) {
    namespace G = zfsmgr::base::gsa;
    GsaNivelado res;
    res.paso.hecho = true;
    const auto falla = [&](const std::string& codigo, std::vector<std::string> args) {
        res.paso.falla(codigo, std::move(args));
        log("ERROR", "GSA level error for " + ds + ": " + res.paso.error);
        return res;
    };
    // Un envío que falló: con su propio código si lo trae (no escuchar, sin puerto…), y si
    // no, envuelto en «falló el envío a …» con lo que dijo.
    const auto fallaEnvio = [&](const char* codigoEnvoltorio, const std::string& donde,
                                const ExecResult& r) {
        if (!r.code.empty()) {
            return falla(r.code, r.args);
        }
        return falla(codigoEnvoltorio,
                     {donde, trim(r.err).empty() ? "exit " + std::to_string(r.rc) : trim(r.err)});
    };
    std::string conexionDestino;
    std::string dstDataset;
    if (!G::splitTarget(dstSpec, conexionDestino, dstDataset)) {
        return falla("gsa_bad_destination", {dstSpec});
    }
    const std::string& dstUrl = dstSpec;
    const bool isLocal = (target.mode == "local");

    std::vector<G::SnapRef> origen;
    std::string err;
    if (!gsaSnapshotsDe(ds, origen, err)) {
        return falla("gsa_list_snapshots_failed", {ds, err});
    }
    if (origen.empty()) {
        return falla("gsa_source_no_snapshots", {});
    }

    GsaPeer peer;
    if (!isLocal && !gsaCargaPeer(conexionDestino, peer)) {
        // Que falte el par NO es «saltarse» esta nivelación: es un fallo, y se dice qué hacer.
        return falla("gsa_no_peer_credentials", {conexionDestino, std::string(kPeersPath)});
    }

    // ── Lo que quedó A MEDIAS en el destino, primero.
    //
    // El receptor recibe con `-s`, así que un corte —la red, una cancelación, un reinicio—
    // deja allí un testigo, y mientras exista el destino no acepta ningún otro flujo. Se
    // continúa con `zfs send -t`. Si el testigo ya no sirve —la poda borró el snapshot que
    // se estaba mandando—, se descarta lo recibido y se empieza de cero, diciéndolo.
    {
        const zfsmgr::base::transfer::AgentCall llama =
            [&](const zfsmgr::base::ConnectionProfile&, const std::vector<std::string>& args, int,
                std::string& out, std::string& errOut, int& rc) {
                if (args.empty())
                    return false;
                if (isLocal) {
                    const ExecResult x = executeAgentCommandCapture(
                        args[0], std::vector<std::string>(args.begin() + 1, args.end()),
                        "zfsmgr-agent");
                    out = x.out;
                    errOut = x.err;
                    rc = x.rc;
                    return true;
                }
                std::string e;
                const GsaRpc g = gsaPeerRpc(
                    peer, args[0], std::vector<std::string>(args.begin() + 1, args.end()), out, e);
                errOut = e;
                rc = g == GsaRpc::Ok ? 0 : 1;
                return g != GsaRpc::NoLlega;
            };
        const zfsmgr::base::transfer::Resume aMedias = zfsmgr::base::transfer::findResumeToken(
            llama, zfsmgr::base::ConnectionProfile{}, dstDataset);
        if (aMedias.any()) {
            const ExecResult prueba = runExecCapture("zfs", {"send", "-n", "-t", aMedias.token});
            if (prueba.rc != 0) {
                log("WARN", "GSA level of " + ds + ": the partial receive on " + aMedias.heldBy
                                + " cannot be resumed (" + trim(prueba.err)
                                + "); it is discarded and the levelling starts over");
                std::string e;
                if (!enDestinoAborta(isLocal, peer, aMedias.heldBy, e)) {
                    return falla("gsa_cannot_discard_partial", {aMedias.heldBy, e});
                }
            } else {
                log("INFO", "GSA level resuming " + ds + " -> " + dstUrl + " on " + aMedias.heldBy);
                const ExecResult rr = gsaEnviaFlujo(isLocal, peer, conexionDestino,
                                                    {"send", "-t", aMedias.token}, aMedias.heldBy,
                                                    /*incremental=*/false, aMedias.token, avance);
                if (gsaParado()) {
                    return falla(g_stop.load() ? "daemon_stopping" : "cancelled", {});
                }
                if (rr.rc != 0) {
                    return fallaEnvio("gsa_resume_failed", dstUrl, rr);
                }
                log("INFO", "GSA level resumed " + ds + " -> " + dstUrl);
            }
        }
    }

    // ── El destino: si existe y qué snapshots tiene.
    bool destinoExiste = false;
    std::vector<G::SnapRef> destino;
    if (isLocal) {
        const ExecResult chk = runExecCapture("zfs", {"list", "-H", "-o", "name", dstDataset});
        destinoExiste = (chk.rc == 0);
        if (!destinoExiste && trim(chk.err).find("does not exist") == std::string::npos) {
            return falla("gsa_cannot_look_at", {dstUrl, trim(chk.err)});
        }
        if (destinoExiste && !gsaSnapshotsDe(dstDataset, destino, err)) {
            return falla("gsa_list_snapshots_failed", {dstUrl, err});
        }
    } else {
        std::string salida;
        std::string errPeer;
        switch (gsaPeerRpc(peer, "--dump-zfs-list-all", {dstDataset}, salida, errPeer)) {
            case GsaRpc::NoLlega:
                return falla(t_gsaFalloPar.codigo, t_gsaFalloPar.args);
            case GsaRpc::Rechaza:
                // Solo «no existe» es la primera nivelación; cualquier otra negativa es un fallo.
                if (errPeer.find("does not exist") == std::string::npos) {
                    return falla("gsa_cannot_look_at", {dstUrl, errPeer});
                }
                destinoExiste = false;
                break;
            case GsaRpc::Ok: {
                destinoExiste = true;
                // name, guid, used, compressratio, encryption, creation… Con -r salen también
                // los descendientes: solo cuentan los del propio dataset. Orden por creación,
                // estable para respetar el de `zfs list` dentro del mismo segundo.
                std::vector<std::pair<long long, G::SnapRef>> conFecha;
                const std::string prefijo = dstDataset + "@";
                for (const std::string& l : splitLines(salida)) {
                    const std::vector<std::string> c = zfsmgr::base::split(l, "\t", false);
                    if (c.size() < 6 || c[0].rfind(prefijo, 0) != 0) {
                        continue;
                    }
                    long long creado = 0;
                    try {
                        creado = std::stoll(c[5]);
                    } catch (...) {
                    }
                    conFecha.push_back({creado, {c[0].substr(prefijo.size()), trim(c[1])}});
                }
                std::stable_sort(conFecha.begin(), conFecha.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                for (auto& x : conFecha) {
                    destino.push_back(x.second);
                }
                break;
            }
        }
    }

    const G::LevelPlan plan = G::planLevel(origen, destino, destinoExiste);
    if (plan.kind == G::LevelPlan::Kind::Conflict) {
        if (plan.conflicting.empty()) {
            return falla("gsa_target_unrelated", {dstUrl});
        }
        std::string lista;
        for (const std::string& n : plan.conflicting) {
            lista += (lista.empty() ? "" : ", ") + n;
        }
        return falla("gsa_target_has_own", {dstUrl, lista});
    }

    // Acciones sobre el destino, en la máquina que toque.
    const auto enDestino = [&](const std::string& verbo, const std::vector<std::string>& local,
                               const std::vector<std::string>& remoto, std::string& error) {
        if (isLocal) {
            const ExecResult r = runExecCapture("zfs", local);
            error = trim(r.err);
            return r.rc == 0;
        }
        std::string salida;
        return gsaPeerRpc(peer, verbo, remoto, salida, error) == GsaRpc::Ok;
    };

    // 1) Los renombrados. Si uno falla no se para: el envío por guid funciona igual, y se
    //    vuelve a intentar en la siguiente pasada.
    for (const auto& rn : plan.renameOnTarget) {
        std::vector<std::string> local = {"rename"};
        if (recursive) {
            local.push_back("-r");
        }
        local.push_back(dstDataset + "@" + rn.first);
        local.push_back(dstDataset + "@" + rn.second);
        std::vector<std::string> remoto = {dstDataset + "@" + rn.first,
                                           dstDataset + "@" + rn.second};
        if (recursive) {
            remoto.push_back("r");
        }
        std::string e;
        if (enDestino("--mutate-zfs-rename", local, remoto, e)) {
            log("INFO", "GSA level renamed " + dstDataset + "@" + rn.first + " -> " + rn.second);
        } else {
            log("WARN", "GSA level cannot rename " + dstDataset + "@" + rn.first + " -> "
                            + rn.second + ": " + e);
        }
    }

    // 2) El envío.
    const std::string& ultimo = origen.back().name;
    if (plan.kind == G::LevelPlan::Kind::UpToDate) {
        res.enviado = ultimo;
        res.guidBase = origen.back().guid;
    } else {
        const bool incremental = (plan.kind == G::LevelPlan::Kind::Incremental);
        // «-e» minúscula: bloques embebidos («-E» no es una bandera de `zfs send`).
        const std::string sendOpts = recursive ? "-wLecR" : "-wLec";
        log("INFO", "GSA level start for " + ds + " -> " + dstUrl
                        + (incremental ? " from @" + plan.base : " (full)") + " to @" + ultimo);
        std::vector<std::string> sendArgv = {"send", sendOpts};
        if (incremental) {
            sendArgv.push_back("-I");
            sendArgv.push_back("@" + plan.base);
        }
        sendArgv.push_back(ds + "@" + ultimo);
        const ExecResult r = gsaEnviaFlujo(isLocal, peer, conexionDestino, sendArgv, dstDataset,
                                           incremental, std::string(), avance);
        if (gsaParado()) {
            return falla(g_stop.load() ? "daemon_stopping" : "cancelled", {});
        }
        if (r.rc != 0) {
            return fallaEnvio("gsa_send_failed", dstUrl, r);
        }
        res.enviado = ultimo;
        res.guidBase = origen.back().guid;
        log("INFO", "GSA level done for " + ds + " -> " + dstUrl + " @" + ultimo);
    }

    // 3) La poda del destino, over what is LEFT after receiving: a recursive levelling
    //    receives with `recv -F` and a replication stream already destroys there what the
    //    source no longer has (see G::stillToPrune). Read again; if that read fails, prune as
    //    planned, which is what was done before.
    std::vector<std::string> aPodar = plan.pruneOnTarget;
    if (!aPodar.empty() && plan.kind != G::LevelPlan::Kind::UpToDate) {
        std::vector<std::string> presentes;
        bool leido = false;
        if (isLocal) {
            std::vector<G::SnapRef> lista;
            std::string e;
            if (gsaSnapshotsDe(dstDataset, lista, e)) {
                leido = true;
                for (const G::SnapRef& x : lista) {
                    presentes.push_back(x.name);
                }
            }
        } else {
            std::string salida;
            std::string errPeer;
            if (gsaPeerRpc(peer, "--dump-zfs-list-all", {dstDataset}, salida, errPeer)
                == GsaRpc::Ok) {
                leido = true;
                const std::string prefijo = dstDataset + "@";
                for (const std::string& l : splitLines(salida)) {
                    const std::string nombre = l.substr(0, l.find('\t'));
                    if (nombre.rfind(prefijo, 0) == 0) {
                        presentes.push_back(nombre.substr(prefijo.size()));
                    }
                }
            }
        }
        if (leido) {
            const std::vector<std::string> quedan = G::stillToPrune(aPodar, presentes);
            for (const std::string& n : aPodar) {
                if (std::find(quedan.begin(), quedan.end(), n) == quedan.end()) {
                    log("INFO", "GSA level " + dstDataset + "@" + n
                                    + " was already removed by the receive");
                }
            }
            aPodar = quedan;
        }
    }
    for (const std::string& n : aPodar) {
        res.podaDestino.hecho = true;
        std::vector<std::string> local = {"destroy"};
        if (recursive) {
            local.push_back("-r");
        }
        local.push_back(dstDataset + "@" + n);
        std::string e;
        if (enDestino("--mutate-zfs-destroy", local,
                      {dstDataset + "@" + n, "0", recursive ? "r" : "-"}, e)) {
            ++res.borradosEnDestino;
            log("INFO", "GSA level pruned " + dstDataset + "@" + n);
        } else {
            res.podaDestino.falla("gsa_target_destroy_failed",
                                  {dstDataset + "@" + n, conexionDestino, e});
            log("ERROR", "GSA level prune error: " + res.podaDestino.error);
        }
    }
    return res;
}

// ── El estado del planificador ───────────────────────────────────────────────────────
//
// Lo que pasó en la última pasada de cada programación: el snapshot, la poda, la
// nivelación y la poda del destino, cada uno con si fue bien y por qué no. Hasta ahora el
// único estado eran los propios snapshots, que dicen lo que salió pero no lo que falló: un
// fallo solo se veía leyendo el registro, y nadie lo leía. El CLI lo pide con
// `--dump-gsa-status` (al arrancar y en `schedule status`).
#ifdef _WIN32
#else
#endif

// Una pasada cada vez: el hilo y `--mutate-gsa-run-once` no pueden pisarse.
std::mutex g_gsaPasada;
// Y el fichero, que se lee y escribe desde los dos y desde `--dump-gsa-status`.
std::mutex g_gsaEstado;

zfsmgr::base::json::Value gsaLeeEstadoSinBloqueo() {
    zfsmgr::base::json::Value raiz;
    std::ifstream f(kGsaStatePath);
    if (!f.is_open()) {
        return raiz;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!zfsmgr::base::json::parse(ss.str(), raiz, &err) || !raiz.isObject()) {
        return zfsmgr::base::json::Value();
    }
    return raiz;
}

// Escritura atómica: un temporal al lado y un renombrado. Un daemon que muere a mitad de
// escribir deja el fichero anterior entero, no uno a medias que ya no se puede leer.
void gsaGuardaEstadoSinBloqueo(const zfsmgr::base::json::Value& raiz) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(kGsaStatePath).parent_path(), ec);
    const std::string tmp = std::string(kGsaStatePath) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) {
            daemonLog("ERROR", std::string("GSA cannot write ") + tmp);
            return;
        }
        f << zfsmgr::base::json::toIndented(raiz);
    }
    std::filesystem::rename(tmp, kGsaStatePath, ec);
    if (ec) {
        daemonLog("ERROR", std::string("GSA cannot write ") + kGsaStatePath + ": " + ec.message());
    }
}

zfsmgr::base::json::Value gsaEntradaDe(const std::string& ds) {
    std::lock_guard<std::mutex> lock(g_gsaEstado);
    return gsaLeeEstadoSinBloqueo()["datasets"][ds];
}

// Cambiar la entrada de un dataset LEYÉNDOLA Y ESCRIBIÉNDOLA de una vez, con el cerrojo
// puesto. La pasada y el trabajo de nivelación escriben la misma entrada desde hilos
// distintos, cada uno sus campos; con «leer al principio, escribir al final» uno pisaba lo
// que el otro había escrito entre medias.
void gsaActualizaEntrada(const std::string& ds,
                         const std::function<void(zfsmgr::base::json::Value&)>& cambia) {
    std::lock_guard<std::mutex> lock(g_gsaEstado);
    zfsmgr::base::json::Value raiz = gsaLeeEstadoSinBloqueo();
    zfsmgr::base::json::Value datasets = raiz["datasets"];
    zfsmgr::base::json::Value entrada = datasets[ds];
    cambia(entrada);
    datasets.set(ds, entrada);
    raiz.set("datasets", datasets);
    gsaGuardaEstadoSinBloqueo(raiz);
}

// El error de un paso en el estado: su código y sus argumentos, que el CLI dice en su idioma.
void gsaPonError(zfsmgr::base::json::Value& v, const std::string& codigo,
                 const std::vector<std::string>& args) {
    namespace J = zfsmgr::base::json;
    v.set("error_code", J::Value(codigo));
    J::Array a;
    for (const auto& x : args)
        a.push_back(J::Value(x));
    v.set("error_args", J::Value(std::move(a)));
}

zfsmgr::base::json::Value gsaRegistroDe(const GsaPaso& p, long long cuando) {
    zfsmgr::base::json::Value v;
    v.set("time", zfsmgr::base::json::Value(cuando));
    v.set("ok", zfsmgr::base::json::Value(p.ok));
    if (!p.ok) {
        gsaPonError(v, p.codigo, p.args);
    }
    return v;
}

// ── Una programación: lo que toca hacer con un dataset ───────────────────────────────

GsaProgramacion gsaLeeProgramacion(const std::string& ds) {
    GsaProgramacion p;
    p.recursive = gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-recursive", false));
    p.hourly = gsaIntVal(gsaPropValue(ds, "org.zfsmgr:schedule-hourly", false));
    p.daily = gsaIntVal(gsaPropValue(ds, "org.zfsmgr:schedule-daily", false));
    p.weekly = gsaIntVal(gsaPropValue(ds, "org.zfsmgr:schedule-weekly", false));
    p.monthly = gsaIntVal(gsaPropValue(ds, "org.zfsmgr:schedule-monthly", false));
    p.yearly = gsaIntVal(gsaPropValue(ds, "org.zfsmgr:schedule-yearly", false));
    p.levelOn = gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-level", false));
    p.dstSpec = gsaPropValue(ds, "org.zfsmgr:schedule-target", false);
    return p;
}

// El ancestro con programación RECURSIVA activada que cubre a `ds`, o vacío.
std::string gsaAncestroRecursivo(const std::string& ds) {
    std::string probe = ds;
    while (true) {
        const std::size_t slash = probe.rfind('/');
        if (slash == std::string::npos) {
            return {};
        }
        probe = probe.substr(0, slash);
        if (gsaBoolOn(gsaPropValue(probe, "org.zfsmgr:schedule-enabled", /*local=*/true))
            && gsaBoolOn(gsaPropValue(probe, "org.zfsmgr:schedule-recursive", /*local=*/false))) {
            return probe;
        }
    }
}

GsaLog gsaLogDelDaemon() {
    // Al registro del DAEMON, no a uno aparte, y con su nivel: un fallo es ERROR, no INFO.
    return [](const char* nivel, const std::string& linea) { daemonLog(nivel, linea); };
}

// ── La nivelación como TRABAJO del daemon ───────────────────────────────────────────
//
// Antes se hacía dentro de la pasada: no salía en `jobs`, no se veía su avance, `job cancel`
// no la paraba, y mientras duraba —una primera nivelación son horas— el resto de los
// datasets se quedaba sin snapshots. Ahora la pasada solo decide (`decideLevel`) y lanza un
// trabajo «gsa-level» en otro hilo, que al acabar deja su resultado en el estado.
//
// Uno por dataset y dos a la vez como mucho; los demás esperan «en cola».
std::mutex g_gsaNivMutex;
std::condition_variable g_gsaNivCv;
int g_gsaNivEnMarcha = 0;
std::map<std::string, std::string> g_gsaNivPorDataset;  // dataset -> id del trabajo

bool gsaNivelandose(const std::string& ds) {
    std::lock_guard<std::mutex> lock(g_gsaNivMutex);
    return g_gsaNivPorDataset.count(ds) > 0;
}

bool gsaTrabajoCancelado(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    const auto it = g_jobs.find(id);
    return it != g_jobs.end() && it->second.state == JobState::Cancelled;
}

std::string gsaLanzaNivelacion(const std::string& ds, bool recursive, const std::string& dstSpec,
                               const GsaTargetConn& target, const GsaLog& log) {
    const std::string id = generateJobId();
    if (id.empty()) {
        log("ERROR", "GSA level error for " + ds + ": could not create a job id");
        return {};
    }
    {
        DaemonJob j;
        j.id = id;
        j.type = "gsa-level";
        j.state = JobState::Queued;
        j.snap = ds;
        j.dstDataset = dstSpec;
        j.startedAtUtc = utcNowIsoString();
        j.description = "gsa level " + ds + " -> " + dstSpec;
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_jobs[id] = j;
        persistJobsLocked();
    }
    {
        std::lock_guard<std::mutex> lock(g_gsaNivMutex);
        g_gsaNivPorDataset[ds] = id;
    }
    gsaActualizaEntrada(ds, [&](zfsmgr::base::json::Value& e) {
        zfsmgr::base::json::Value t;
        t.set("id", zfsmgr::base::json::Value(id));
        e.set("level_job", t);
    });
    log("INFO", "GSA level job " + id + " queued for " + ds);

    std::thread([id, ds, recursive, dstSpec, target]() {
        t_currentJobId = id;
        const GsaLog log = gsaLogDelDaemon();
        // Esperar turno.
        {
            std::unique_lock<std::mutex> lock(g_gsaNivMutex);
            while (g_gsaNivEnMarcha >= kGsaNivMax && !g_stop.load() && !gsaTrabajoCancelado(id)) {
                g_gsaNivCv.wait_for(lock, std::chrono::seconds(2));
            }
            if (!g_stop.load() && !gsaTrabajoCancelado(id)) {
                ++g_gsaNivEnMarcha;
            }
        }
        const bool tuvoTurno = !g_stop.load() && !gsaTrabajoCancelado(id);
        GsaNivelado n;
        if (tuvoTurno) {
            {
                std::lock_guard<std::mutex> lock(g_jobsMutex);
                auto it = g_jobs.find(id);
                if (it != g_jobs.end()) {
                    it->second.state = JobState::Running;
                    persistJobsLocked();
                }
            }
            const TransferProgressFn avance = [&id](std::uint64_t bytes, long segs, double mibs) {
                reportJobCounters(bytes, mibs, segs);
                char linea[160];
                std::snprintf(linea, sizeof(linea), "%.1f MiB @ %.1f MiB/s, %lds",
                              static_cast<double>(bytes) / (1024.0 * 1024.0), mibs, segs);
                reportJobProgress(linea);
                return !g_stop.load() && !gsaTrabajoCancelado(id);
            };
            n = gsaNivela(ds, recursive, dstSpec, target, avance, log);
            {
                std::lock_guard<std::mutex> lock(g_gsaNivMutex);
                --g_gsaNivEnMarcha;
            }
            g_gsaNivCv.notify_all();
        }
        const bool cancelado = gsaTrabajoCancelado(id);
        const bool parado = g_stop.load() && !cancelado;
        const long long ahora = static_cast<long long>(std::time(nullptr));
        {
            std::lock_guard<std::mutex> lock(g_jobsMutex);
            auto it = g_jobs.find(id);
            if (it != g_jobs.end()) {
                if (!cancelado) {
                    it->second.state = n.paso.ok && tuvoTurno ? JobState::Done : JobState::Failed;
                    if (parado) {
                        ponErrorDeTrabajo(it->second, "daemon_stopping", {});
                    } else if (!n.paso.ok) {
                        ponErrorDeTrabajo(it->second, n.paso.codigo, n.paso.args);
                    }
                }
                it->second.finishedAtUtc = utcNowIsoString();
                persistJobsLocked();
            }
        }
        gsaActualizaEntrada(ds, [&](zfsmgr::base::json::Value& e) {
            namespace J = zfsmgr::base::json;
            const J::Value previo = e["level_result"];
            J::Value reg;
            reg.set("time", J::Value(ahora));
            reg.set("job", J::Value(id));
            const bool ok = tuvoTurno && n.paso.ok && !cancelado;
            reg.set("ok", J::Value(ok));
            if (ok) {
                reg.set("sent", J::Value(n.enviado));
                reg.set("base_guid", J::Value(n.guidBase));
            } else {
                std::string codigo = n.paso.codigo;
                std::vector<std::string> args = n.paso.args;
                if (cancelado) {
                    codigo = "gsa_cancelled_by_user";
                    args = {id};
                    reg.set("cancelled", J::Value(true));
                } else if (parado) {
                    codigo = "gsa_daemon_stopped";
                    args = {};
                }
                gsaPonError(reg, codigo, args);
                // La base sigue siendo la de la última vez que fue bien: es la que hay que
                // seguir protegiendo de la poda.
                reg.set("base_guid", J::Value(previo["base_guid"].toString()));
                reg.set("sent", J::Value(previo["sent"].toString()));
            }
            e.set("level_result", reg);
            if (n.podaDestino.hecho || ok) {
                J::Value rp = gsaRegistroDe(n.podaDestino, ahora);
                rp.set("destroyed", J::Value(n.borradosEnDestino));
                e.set("target_prune", rp);
            }
            e.remove("level_job");
        });
        {
            std::lock_guard<std::mutex> lock(g_gsaNivMutex);
            g_gsaNivPorDataset.erase(ds);
        }
        if (cancelado) {
            log("WARN", "GSA level job " + id + " for " + ds + " cancelled by the user");
        }
    }).detach();
    return id;
}

// Lo que se hace con un dataset en cada pasada. Devuelve el id del trabajo de nivelación que
// se haya lanzado, si se lanzó alguno.
std::string gsaProcesaDataset(const std::string& ds, const std::string& selfConn, bool forzado,
                              const GsaLog& log) {
    namespace J = zfsmgr::base::json;
    namespace G = zfsmgr::base::gsa;
    const GsaProgramacion p = gsaLeeProgramacion(ds);
    const long long ahora = static_cast<long long>(std::time(nullptr));
    const J::Value previo = gsaEntradaDe(ds);
    const J::Value previoNivel = previo["level_result"];
    // Con una nivelación en marcha, ni se poda el origen ni se renombra: `zfs send -I` está
    // leyendo esos snapshots. Se recupera en la primera pasada después de acabar.
    const bool nivelandose = gsaNivelandose(ds);

    const std::vector<std::string> due =
        gsaDueClasses(ds, p.hourly, p.daily, p.weekly, p.monthly, p.yearly, std::time(nullptr));
    {
        std::string dueStr;
        for (const std::string& c : due) {
            if (!dueStr.empty())
                dueStr += ',';
            dueStr += c;
        }
        log("INFO",
            "GSA evaluate " + ds + ": recursive=" + (p.recursive ? "yes" : "no")
                + " hourly=" + std::to_string(p.hourly) + " daily=" + std::to_string(p.daily)
                + " weekly=" + std::to_string(p.weekly) + " monthly=" + std::to_string(p.monthly)
                + " yearly=" + std::to_string(p.yearly) + " due=" + dueStr
                + (nivelandose ? " (levelling in progress)" : ""));
    }

    // 1) Los snapshots que tocan. Si fallan dos clases se queda el primer fallo, y uno que
    //    va bien detrás no lo tapa.
    bool hayNuevo = false;
    bool algunSnapshot = false;
    GsaPaso pasoSnapshot;
    std::string nombreSnapshot;
    for (const std::string& klass : due) {
        if (g_stop.load())
            break;
        std::string nombre;
        const GsaPaso paso = gsaHazSnapshot(ds, klass, p.recursive, !nivelandose, nombre, log);
        algunSnapshot = true;
        if (!nombre.empty()) {
            nombreSnapshot = nombre;
            hayNuevo = true;
        }
        if (!paso.ok && pasoSnapshot.ok) {
            pasoSnapshot = paso;
        }
    }

    // 2) La poda del origen, en cada pasada —su disparador no es que toque un snapshot nuevo
    //    sino que sobren viejos—, salvo con una nivelación en marcha. La base de la
    //    nivelación queda protegida.
    const std::string guidBase = p.levelOn ? previoNivel["base_guid"].toString() : std::string();
    int borrados = 0;
    bool baseConservada = false;
    GsaPaso poda;
    if (!nivelandose) {
        for (const char* klass : {"hourly", "daily", "weekly", "monthly", "yearly"}) {
            if (g_stop.load())
                break;
            const int keep = (std::string(klass) == "hourly")    ? p.hourly
                             : (std::string(klass) == "daily")   ? p.daily
                             : (std::string(klass) == "weekly")  ? p.weekly
                             : (std::string(klass) == "monthly") ? p.monthly
                                                                 : p.yearly;
            const GsaPaso uno =
                gsaPoda(ds, klass, keep, p.recursive, guidBase, borrados, baseConservada, log);
            if (!uno.ok && poda.ok) {
                poda = uno;
            }
        }
    }

    gsaActualizaEntrada(ds, [&](J::Value& e) {
        e.set("last_eval", J::Value(ahora));
        e.set("level", J::Value(p.levelOn));
        e.set("destination", J::Value(p.dstSpec));
        e.remove("pending");
        if (algunSnapshot) {
            J::Value reg = gsaRegistroDe(pasoSnapshot, ahora);
            if (!nombreSnapshot.empty()) {
                reg.set("name", J::Value(nombreSnapshot));
            }
            e.set("snapshot", reg);
        }
        if (!nivelandose) {
            J::Value reg = gsaRegistroDe(poda, ahora);
            reg.set("destroyed", J::Value(borrados));
            reg.set("kept_base", J::Value(baseConservada));
            e.set("prune", reg);
        }
        if (!p.levelOn) {
            e.remove("level_result");
            e.remove("target_prune");
        }
    });

    // 3) ¿Nivelar? Lo decide la capa base; aquí solo se lanza.
    G::LevelTrigger t;
    t.levelOn = p.levelOn;
    t.jobActive = nivelandose;
    t.newSnapshot = hayNuevo;
    t.prunedSource = borrados > 0;
    t.forced = forzado;
    t.hasPrevious = previoNivel.isObject();
    t.previousOk = previoNivel["ok"].toBool(false);
    t.previousCancelled = previoNivel["cancelled"].toBool(false);
    if (g_stop.load() || G::decideLevel(t) != G::LevelAction::Start) {
        return {};
    }
    GsaTargetConn target;
    std::string conexionDestino;
    std::string datasetDestino;
    if (G::splitTarget(p.dstSpec, conexionDestino, datasetDestino)
        && gsaDestinoSoyYo(conexionDestino, selfConn)) {
        target.mode = "local";
    }
    return gsaLanzaNivelacion(ds, p.recursive, p.dstSpec, target, log);
}

void gsaRunOnce(const GsaLog& log) {
    // NO se exige `/etc/zfsmgr/gsa.conf`, que ya no existe: la programación está en las
    // propiedades del dataset, y lo único que necesita credenciales —nivelar contra otra
    // máquina— lo comprueba la nivelación, diciéndolo como fallo.
    std::lock_guard<std::mutex> pasada(g_gsaPasada);
    // Con quién se identifica esta máquina, para saber si un destino es ELLA.
    const std::string selfConn = gsaYoMismo();

    const ExecResult dsResult =
        runExecCapture("zfs", {"list", "-H", "-o", "name", "-t", "filesystem"});
    std::set<std::string> programados;
    if (dsResult.rc == 0) {
        std::vector<std::string> raicesRecursivas;
        const auto desciende = [](const std::string& ds, const std::string& root) {
            return ds.size() > root.size() + 1 && ds.compare(0, root.size(), root) == 0
                   && ds[root.size()] == '/';
        };
        for (const std::string& rawLine : splitLines(dsResult.out)) {
            if (g_stop.load())
                break;
            const std::string ds = trim(rawLine);
            if (ds.empty())
                continue;
            if (!gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-enabled", /*local=*/true)))
                continue;
            bool cubierto = false;
            for (const std::string& root : raicesRecursivas) {
                if (desciende(ds, root)) {
                    cubierto = true;
                    break;
                }
            }
            if (cubierto || !gsaAncestroRecursivo(ds).empty())
                continue;
            programados.insert(ds);
            gsaProcesaDataset(ds, selfConn, /*forzado=*/false, log);
            if (gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-recursive", false))) {
                raicesRecursivas.push_back(ds);
            }
        }
    }
    // La pasada, y fuera lo que ya no está programado: un dataset desprogramado no puede
    // seguir saliendo en rojo por un fallo de hace un mes.
    if (!g_stop.load()) {
        std::lock_guard<std::mutex> lock(g_gsaEstado);
        zfsmgr::base::json::Value raiz = gsaLeeEstadoSinBloqueo();
        zfsmgr::base::json::Value datasets;
        for (const auto& kv : raiz["datasets"].toObject()) {
            if (programados.count(kv.first)) {
                datasets.set(kv.first, kv.second);
            }
        }
        raiz.set("datasets", datasets);
        raiz.set("last_pass",
                 zfsmgr::base::json::Value(static_cast<long long>(std::time(nullptr))));
        gsaGuardaEstadoSinBloqueo(raiz);
    }
}

// Una pasada para UN dataset, ya. Para comprobar una programación tras arreglar lo que
// fallaba sin esperar cinco minutos, y para probarla. Nunca recorre todos los datasets.
ExecResult runGsaRunOnceCapture(const std::string& ds) {
    ExecResult r;
    if (trim(ds).empty() || ds.find('@') != std::string::npos) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-gsa-run-once <dataset>"});
        return r;
    }
    if (!gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-enabled", /*local=*/true))) {
        return fallo(1, "gsa_no_schedule", {ds});
    }
    const std::string ancestro = gsaAncestroRecursivo(ds);
    if (!ancestro.empty()) {
        return fallo(1, "gsa_covered_by_recursive", {ds, ancestro});
    }
    std::vector<std::string> lineas;
    const GsaLog log = [&lineas](const char* nivel, const std::string& linea) {
        daemonLog(nivel, linea);
        lineas.push_back(std::string(nivel) + " " + linea);
    };
    std::string trabajo;
    {
        std::lock_guard<std::mutex> pasada(g_gsaPasada);
        trabajo = gsaProcesaDataset(ds, gsaYoMismo(), /*forzado=*/true, log);
    }
    for (const std::string& l : lineas) {
        r.out += l + "\n";
    }
    // La nivelación va aparte, como trabajo: aquí solo se dice cuál. Quien quiera esperarla
    // sigue ese trabajo (`schedule run --wait`).
    if (!trabajo.empty()) {
        r.out += "JOB_ID=" + trabajo + "\n";
    } else if (gsaNivelandose(ds)) {
        std::lock_guard<std::mutex> lock(g_gsaNivMutex);
        r.out += "JOB_ID=" + g_gsaNivPorDataset[ds] + "\nALREADY_RUNNING=1\n";
    }
    const zfsmgr::base::json::Value e = gsaEntradaDe(ds);
    const bool mal = !e["snapshot"]["ok"].toBool(true) || !e["prune"]["ok"].toBool(true);
    r.rc = mal ? 1 : 0;
    return r;
}

// El estado para el CLI: lo guardado, más las programaciones activadas que todavía no han
// pasado por ninguna pasada (`pending`), y el reloj del daemon para que quien lo lea pueda
// decir si el hilo está vivo sin fiarse del suyo.
// When this schedule's next snapshot is due. A class already due (a failed snapshot is retried
// on every pass) is taken on the next pass; otherwise, the soonest start of a next period.
long long gsaProximaVez(const std::string& ds, long long ultimaPasada, long long ahora) {
    const GsaProgramacion p = gsaLeeProgramacion(ds);
    if (!gsaDueClasses(ds, p.hourly, p.daily, p.weekly, p.monthly, p.yearly,
                       static_cast<std::time_t>(ahora))
             .empty()) {
        return std::max(ahora, ultimaPasada > 0 ? ultimaPasada + kGsaPasadaSegs : ahora);
    }
    long long mejor = 0;
    const std::pair<const char*, int> clases[] = {{"hourly", p.hourly},
                                                  {"daily", p.daily},
                                                  {"weekly", p.weekly},
                                                  {"monthly", p.monthly},
                                                  {"yearly", p.yearly}};
    for (const auto& c : clases) {
        if (c.second <= 0) {
            continue;
        }
        const long long t = zfsmgr::base::gsa::nextPeriodStart(c.first, ahora);
        if (mejor == 0 || t < mejor) {
            mejor = t;
        }
    }
    return mejor;
}

ExecResult runDumpGsaStatusCapture() {
    namespace J = zfsmgr::base::json;
    ExecResult r;
    J::Value raiz;
    {
        std::lock_guard<std::mutex> lock(g_gsaEstado);
        raiz = gsaLeeEstadoSinBloqueo();
    }
    J::Value datasets;
    const long long ultimaPasada = raiz["last_pass"].toInt(0);
    const long long ahora = static_cast<long long>(std::time(nullptr));
    const ExecResult pools = runExecCapture("zpool", {"list", "-H", "-o", "name"});
    if (pools.rc == 0) {
        for (const std::string& rawPool : splitLines(pools.out)) {
            const std::string pool = trim(rawPool);
            if (pool.empty())
                continue;
            const ExecResult g =
                runExecCapture("zfs", {"get", "-H", "-r", "-s", "local", "-t", "filesystem", "-o",
                                       "name,value", "org.zfsmgr:schedule-enabled", pool});
            if (g.rc != 0)
                continue;
            for (const std::string& l : splitLines(g.out)) {
                const std::size_t tab = l.find('\t');
                if (tab == std::string::npos)
                    continue;
                const std::string ds = trim(l.substr(0, tab));
                if (!gsaBoolOn(trim(l.substr(tab + 1))))
                    continue;
                if (raiz["datasets"].contains(ds)) {
                    J::Value e = raiz["datasets"][ds];
                    // El trabajo de nivelación en curso, con lo que lleva: eso no está en el
                    // fichero, está en la lista de trabajos.
                    const std::string id = e["level_job"]["id"].toString();
                    if (!id.empty()) {
                        J::Value t = e["level_job"];
                        std::lock_guard<std::mutex> lock(g_jobsMutex);
                        const auto it = g_jobs.find(id);
                        if (it != g_jobs.end()) {
                            t.set("state",
                                  J::Value(it->second.state == JobState::Queued ? "queued"
                                                                                : "running"));
                            t.set("bytes",
                                  J::Value(static_cast<long long>(it->second.bytesTransferred)));
                        }
                        e.set("level_job", t);
                    }
                    e.set("next", J::Value(gsaProximaVez(ds, ultimaPasada, ahora)));
                    datasets.set(ds, e);
                    continue;
                }
                // Los cubiertos por una recursiva no tienen entrada propia, y no la tendrán.
                if (!gsaAncestroRecursivo(ds).empty())
                    continue;
                J::Value e;
                e.set("pending", J::Value(true));
                e.set("level",
                      J::Value(gsaBoolOn(gsaPropValue(ds, "org.zfsmgr:schedule-level", false))));
                e.set("destination",
                      J::Value(gsaPropValue(ds, "org.zfsmgr:schedule-target", false)));
                e.set("next", J::Value(gsaProximaVez(ds, ultimaPasada, ahora)));
                datasets.set(ds, e);
            }
        }
    }
    J::Value out;
    out.set("last_pass", J::Value(raiz["last_pass"].toInt(0)));
    out.set("now", J::Value(ahora));
    if (!datasets.isObject()) {
        datasets = J::Value(J::Object{});
    }
    out.set("datasets", datasets);
    r.rc = 0;
    r.out = J::toCompact(out) + "\n";
    return r;
}

// Al arrancar el daemon: una nivelación que constaba EN CURSO no lo está —el proceso que la
// llevaba ha muerto—. Se anota como fallida, y la siguiente pasada la reintenta continuando
// lo recibido a medias.
void gsaCierraNivelacionesHuerfanas() {
    namespace J = zfsmgr::base::json;
    std::lock_guard<std::mutex> lock(g_gsaEstado);
    J::Value raiz = gsaLeeEstadoSinBloqueo();
    J::Value datasets;
    bool cambio = false;
    const long long ahora = static_cast<long long>(std::time(nullptr));
    for (const auto& kv : raiz["datasets"].toObject()) {
        J::Value e = kv.second;
        if (e.contains("level_job")) {
            const J::Value previo = e["level_result"];
            J::Value reg;
            reg.set("time", J::Value(ahora));
            reg.set("job", J::Value(e["level_job"]["id"].toString()));
            reg.set("ok", J::Value(false));
            gsaPonError(reg, "gsa_daemon_restarted", {});
            reg.set("base_guid", J::Value(previo["base_guid"].toString()));
            reg.set("sent", J::Value(previo["sent"].toString()));
            e.set("level_result", reg);
            e.remove("level_job");
            cambio = true;
        }
        datasets.set(kv.first, e);
    }
    if (cambio) {
        raiz.set("datasets", datasets);
        gsaGuardaEstadoSinBloqueo(raiz);
    }
}

void runGsaSchedulerThread() {
    gsaCierraNivelacionesHuerfanas();
    // Cada cinco minutos, y la primera pasada a los treinta segundos de arrancar.
    //
    // Despertar a menudo es seguro desde que lo que toca se decide por PERIODO y no por la
    // hora del reloj: repetir la pasada no repite nada. Los treinta segundos de gracia son
    // para que una máquina que estuvo apagada recupere lo que le tocaba nada más encenderse.
    bool primera = true;
    while (!g_stop.load()) {
        const int espera = primera ? 30 : static_cast<int>(kGsaPasadaSegs);
        primera = false;
        for (int i = 0; i < espera && !g_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (g_stop.load())
            break;
        try {
            gsaRunOnce(gsaLogDelDaemon());
        } catch (...) {
            daemonLog("ERROR", "gsa: uncaught exception in the pass");
        }
    }
}

// Testigo de 32 bytes en hexadecimal, para que solo el emisor esperado pueda entregar
// un flujo al puerto que se acaba de abrir.
//
// Con RAND_bytes de OpenSSL, que ya está enlazado, en vez de leer /dev/urandom: había
// un respaldo a std::rand() que en caso de fallo generaba un testigo adivinable, y era
// la única fuente de aleatoriedad de todo el mecanismo. Además /dev/urandom no existe
// en Windows, así que una sola implementación sirve para las dos plataformas.
std::string generateTransferToken() {
    unsigned char buf[32] = {};
    if (RAND_bytes(buf, static_cast<int>(sizeof(buf))) != 1) {
        return std::string();  // sin aleatoriedad no se abre el puerto: el llamante aborta
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char c : buf) {
        out.push_back(hex[(c >> 4) & 0xf]);
        out.push_back(hex[c & 0xf]);
    }
    return out;
}

// Identificador de trabajo: 8 bytes en hexadecimal.
//
// Estaba escrito dos veces leyendo /dev/urandom, que en Windows no existe. Se unifica
// aprovechando el generador de OpenSSL que ya usa el testigo de transfer.
std::string generateJobId() {
    unsigned char buf[8] = {};
    if (RAND_bytes(buf, static_cast<int>(sizeof(buf))) != 1) {
        return std::string();
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (unsigned char c : buf) {
        out.push_back(hex[(c >> 4) & 0xf]);
        out.push_back(hex[c & 0xf]);
    }
    return out;
}

#ifdef _WIN32
// Winsock se inicializa una vez por proceso. runServeLoop ya lo hace, pero este verbo
// también se puede invocar por línea de comandos, donde no ha pasado por ahí.
void ensureWinsock() {
    static bool done = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (done) {
        return;
    }
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    done = true;
}
#endif

#ifdef _WIN32
#else
#endif

void closeTransferSocket(TransferSocket s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
#else
    if (s >= 0) {
        close(s);
    }
#endif
}

// Cierra abortando la conexión (RST): al otro lado, `recv` devuelve un ERROR y no un fin de
// datos. Es la forma de decirle al receptor «lo que te ha llegado no está entero».
void cierraConRechazo(TransferSocket s) {
    if (s == kSocketInvalido) {
        return;
    }
    struct linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    (void)setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
    closeTransferSocket(s);
}

}  // namespace zfsmgr::agent
