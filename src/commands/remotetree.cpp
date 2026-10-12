#include "remotetree.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <openssl/evp.h>
#include <openssl/sha.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // std::min/std::max, no las macros de windows.h
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#endif

namespace fs = std::filesystem;

namespace zfsmgr::remotetree {

bool sameMtime(std::int64_t s1, std::uint32_t ns1, std::int64_t s2, std::uint32_t ns2) {
    if (s1 != s2) {
        return false;
    }
    if (ns1 == ns2) {
        return true;
    }
    // Uno de los dos no guarda fracción de segundo: se compara al segundo.
    if (ns1 == 0 || ns2 == 0) {
        return true;
    }
    // NTFS guarda intervalos de 100 ns: si uno de los dos es múltiplo de 100, se compara a
    // esa resolución.
    if ((ns1 % 100 == 0 || ns2 % 100 == 0) && ns1 / 100 == ns2 / 100) {
        return true;
    }
    return false;
}

namespace {

std::string conBarras(const std::string& s) {
    std::string r = s;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

char letraDe(EntryKind t) {
    switch (t) {
        case EntryKind::Directory: return 'd';
        case EntryKind::File: return 'f';
        case EntryKind::Symlink: return 'l';
        case EntryKind::HardLink: return 'h';
        case EntryKind::Special: return 'n';
    }
    return 'f';
}

bool tipoDe(char c, EntryKind& out) {
    switch (c) {
        case 'd': out = EntryKind::Directory; return true;
        case 'f': out = EntryKind::File; return true;
        case 'l': out = EntryKind::Symlink; return true;
        case 'h': out = EntryKind::HardLink; return true;
        case 'n': out = EntryKind::Special; return true;
        default: return false;
    }
}

char letraDe(Action a) {
    switch (a) {
        case Action::MakeDirectory: return 'D';
        case Action::Send: return 'F';
        case Action::Symlink_: return 'L';
        case Action::HardLink_: return 'H';
        case Action::Special_: return 'N';
        case Action::Metadata_: return 'M';
        case Action::Delete: return 'X';
    }
    return 'F';
}

bool accionDe(char c, Action& out) {
    switch (c) {
        case 'D': out = Action::MakeDirectory; return true;
        case 'F': out = Action::Send; return true;
        case 'L': out = Action::Symlink_; return true;
        case 'H': out = Action::HardLink_; return true;
        case 'N': out = Action::Special_; return true;
        case 'M': out = Action::Metadata_; return true;
        case 'X': out = Action::Delete; return true;
        default: return false;
    }
}

// Lo que NO forma parte del árbol aunque esté dentro, y solo en el primer nivel.
//
// `$RECYCLE.BIN` y `System Volume Information` son del VOLUMEN, no de quien lo usa: Windows
// los pone en la raíz de cada unidad. Sincronizar la raíz de un volumen con borrado se los
// llevaba por delante —visto en una pasada en seco contra una unidad real, que proponía
// borrar la papelera entera— y traerlos al otro extremo tampoco tiene sentido.
//
// Anclados al primer nivel a propósito: un directorio del usuario que se llame igual más
// abajo sí es suyo, y no se toca. Es la misma regla que las exclusiones de `copytree`.
bool esDelVolumen(const std::string& rutaRelativa) {
    if (rutaRelativa.find('/') != std::string::npos) {
        return false;  // no está en el primer nivel
    }
    std::string b;
    b.reserve(rutaRelativa.size());
    for (char c : rutaRelativa) {
        b.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    return b == "$recycle.bin" || b == "system volume information" || b == ".fseventsd"
           || b == ".spotlight-v100" || b == ".trashes";
}

// Cuántos niveles tiene una ruta. Sirve para borrar de dentro hacia fuera.
std::size_t hondura(const std::string& ruta) {
    return static_cast<std::size_t>(std::count(ruta.begin(), ruta.end(), '/'));
}

bool dentroDe(const std::string& ruta, const std::string& dir) {
    return ruta.size() > dir.size() && ruta.compare(0, dir.size(), dir) == 0
           && ruta[dir.size()] == '/';
}

// Identidad de una entrada con varios nombres, para los enlaces duros.
struct Identidad {
    std::uint64_t dev{0};
    std::uint64_t ino{0};
    bool operator<(const Identidad& o) const { return dev != o.dev ? dev < o.dev : ino < o.ino; }
};

#ifndef _WIN32
// El nombre se busca una vez y se recuerda: un árbol de cien mil ficheros de un mismo usuario
// haría cien mil `getpwuid`, y esa llamada puede ir a la red si hay LDAP o NIS detrás.
std::string nombreUsuario(std::uint32_t id) {
    static std::map<std::uint32_t, std::string> cache;
    const auto it = cache.find(id);
    if (it != cache.end()) {
        return it->second;
    }
    const struct passwd* pw = ::getpwuid(static_cast<uid_t>(id));
    const std::string n = pw && pw->pw_name ? pw->pw_name : std::string();
    cache[id] = n;
    return n;
}

std::string nombreGrupo(std::uint32_t id) {
    static std::map<std::uint32_t, std::string> cache;
    const auto it = cache.find(id);
    if (it != cache.end()) {
        return it->second;
    }
    const struct group* gr = ::getgrgid(static_cast<gid_t>(id));
    const std::string n = gr && gr->gr_name ? gr->gr_name : std::string();
    cache[id] = n;
    return n;
}

std::uint32_t nsecDe(const struct stat& st) {
#if defined(__APPLE__)
    return static_cast<std::uint32_t>(st.st_mtimespec.tv_nsec);
#else
    return static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
#endif
}

// Todo lo de una entrada, con `lstat`: de un enlace simbólico interesa el enlace, no aquello
// a lo que apunta —seguirlo daría el dueño del destino, que puede estar fuera del árbol—.
// False si no se puede leer o es de un tipo que no se sabe copiar.
bool describeUna(const fs::path& p, Entry& en, Identidad* id, std::uint32_t* enlaces) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return false;
    }
    en.mtime = static_cast<std::int64_t>(st.st_mtime);
    en.mtimeNsec = nsecDe(st);
    en.mode = static_cast<std::uint32_t>(st.st_mode & 07777);
    en.uid = static_cast<std::uint32_t>(st.st_uid);
    en.gid = static_cast<std::uint32_t>(st.st_gid);
    en.owner = nombreUsuario(en.uid);
    en.group = nombreGrupo(en.gid);
    switch (st.st_mode & S_IFMT) {
        case S_IFDIR: en.kind = EntryKind::Directory; break;
        case S_IFREG:
            en.kind = EntryKind::File;
            en.size = static_cast<std::uint64_t>(st.st_size);
            break;
        case S_IFLNK: {
            en.kind = EntryKind::Symlink;
            std::error_code ec;
            en.target = conBarras(fs::read_symlink(p, ec).generic_string());
            break;
        }
        case S_IFIFO: en.kind = EntryKind::Special; en.special = 'p'; break;
        case S_IFSOCK: en.kind = EntryKind::Special; en.special = 's'; break;
        case S_IFCHR:
        case S_IFBLK:
            en.kind = EntryKind::Special;
            en.special = S_ISCHR(st.st_mode) ? 'c' : 'b';
            en.devMajor = static_cast<std::uint32_t>(major(st.st_rdev));
            en.devMinor = static_cast<std::uint32_t>(minor(st.st_rdev));
            break;
        default:
            return false;
    }
    en.meta = fsmeta::read(p.string());
    if (id != nullptr) {
        id->dev = static_cast<std::uint64_t>(st.st_dev);
        id->ino = static_cast<std::uint64_t>(st.st_ino);
    }
    if (enlaces != nullptr) {
        *enlaces = static_cast<std::uint32_t>(st.st_nlink);
    }
    return true;
}
#else
bool describeUna(const fs::path& p, Entry& en, Identidad* id, std::uint32_t* enlaces) {
    if (enlaces != nullptr) {
        *enlaces = 1;
    }
    // La identidad para los enlaces duros: número de serie del volumen e índice del fichero.
    // Sin ella, un enlace duro creado aquí se describía como un fichero suelto y el emisor lo
    // volvía a enlazar en cada pasada.
    if (id != nullptr || enlaces != nullptr) {
        const HANDLE h = CreateFileW(p.wstring().c_str(), 0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(h, &info)) {
                if (id != nullptr) {
                    id->dev = info.dwVolumeSerialNumber;
                    id->ino = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32)
                              | info.nFileIndexLow;
                }
                if (enlaces != nullptr) {
                    *enlaces = info.nNumberOfLinks;
                }
            }
            CloseHandle(h);
        }
    }
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return false;
    }
    if (fs::is_symlink(st)) {
        en.kind = EntryKind::Symlink;
        std::error_code e2;
        en.target = conBarras(fs::read_symlink(p, e2).generic_string());
    } else if (fs::is_directory(st)) {
        en.kind = EntryKind::Directory;
    } else if (fs::is_regular_file(st)) {
        en.kind = EntryKind::File;
    } else {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d)) {
        ULARGE_INTEGER t;
        t.LowPart = d.ftLastWriteTime.dwLowDateTime;
        t.HighPart = d.ftLastWriteTime.dwHighDateTime;
        // FILETIME cuenta intervalos de 100 ns desde 1601; el epoch de Unix está 11644473600
        // segundos después.
        en.mtime = static_cast<std::int64_t>(t.QuadPart / 10000000ULL) - 11644473600LL;
        en.mtimeNsec = static_cast<std::uint32_t>((t.QuadPart % 10000000ULL) * 100ULL);
        if (en.kind == EntryKind::File) {
            en.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        }
    }
    return true;
}
#endif

}  // namespace

bool walk(const std::string& raiz, std::vector<Entry>& salida, std::string& error,
          bool unSoloSistema, std::vector<std::string>* ilegibles) {
    salida.clear();
    error.clear();
    std::error_code ec;
    const fs::path base = fs::path(raiz);
    if (!fs::is_directory(base, ec) || ec) {
        error = "not a directory: " + raiz;
        return false;
    }
    Identidad raizId;
    {
        Entry tmp;
        std::uint32_t n = 0;
        (void)describeUna(base, tmp, &raizId, &n);
    }
    // Identidad de cada entrada con varios nombres. Se rellena durante el recorrido pero NO
    // se decide aquí cuál es el original: eso se hace al final, con la lista ordenada.
    std::map<std::string, Identidad> identidad;

    // Recursión a mano, y no `recursive_directory_iterator` con skip_permission_denied: aquel
    // daba un directorio ilegible por VACÍO, y con borrado eso vaciaba su copia. Aquí un
    // directorio que no se puede leer se apunta, y el borrado no entra en él.
    std::vector<std::pair<fs::path, std::string>> pendientes = {{base, std::string()}};
    bool primera = true;
    while (!pendientes.empty()) {
        const auto [dir, relDir] = pendientes.back();
        pendientes.pop_back();
        std::error_code e1;
        fs::directory_iterator it(dir, e1);
        if (e1) {
            if (primera) {
                error = "could not walk " + raiz + ": " + e1.message();
                return false;
            }
            if (ilegibles != nullptr) {
                ilegibles->push_back(relDir);
            }
            continue;
        }
        primera = false;
        for (; it != fs::directory_iterator(); it.increment(e1)) {
            if (e1) {
                break;
            }
            const fs::path p = it->path();
            const std::string nombre = conBarras(p.filename().generic_string());
            const std::string rel = relDir.empty() ? nombre : relDir + "/" + nombre;
            if (relDir.empty() && esDelVolumen(nombre)) {
                continue;
            }
            Entry en;
            Identidad id;
            std::uint32_t enlaces = 1;
            if (!describeUna(p, en, &id, &enlaces)) {
                continue;
            }
            en.path = rel;
            if (en.kind == EntryKind::Directory) {
                // Otro sistema de ficheros —en ZFS, otro dataset montado dentro—: ni se baja
                // ni se anota, porque no forma parte de este árbol.
                if (unSoloSistema && id.dev != raizId.dev) {
                    continue;
                }
                salida.push_back(std::move(en));
                pendientes.emplace_back(p, rel);
                continue;
            }
            if (enlaces > 1) {
                identidad.emplace(rel, id);
            }
            salida.push_back(std::move(en));
        }
        if (e1 && ilegibles != nullptr) {
            ilegibles->push_back(relDir);
        }
    }

    std::sort(salida.begin(), salida.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });

    // Los enlaces duros se resuelven AHORA, sobre la lista ya ordenada: el original es siempre
    // el primero por orden alfabético, y eso coincide en las dos máquinas. Decidirlo durante
    // el recorrido dependía del orden en que el sistema devuelve el directorio, y los dos
    // extremos describían el mismo árbol de forma distinta.
    std::map<Identidad, std::string> primero;
    for (Entry& e : salida) {
        const auto it = identidad.find(e.path);
        if (it == identidad.end()) {
            continue;
        }
        const auto ya = primero.find(it->second);
        if (ya == primero.end()) {
            primero.emplace(it->second, e.path);
            continue;
        }
        e.kind = EntryKind::HardLink;
        e.target = ya->second;
        e.size = 0;
    }
    return true;
}

bool walkSingleFile(const std::string& fichero, Entry& salida, std::string& error) {
    error.clear();
    salida = Entry{};
    const fs::path p(fichero);
    if (!describeUna(p, salida, nullptr, nullptr) || salida.kind != EntryKind::File) {
        error = "not a regular file: " + fichero;
        return false;
    }
    salida.path = conBarras(p.filename().generic_string());
    return !salida.path.empty();
}

bool describePath(const std::string& ruta, Entry& salida, std::string& error) {
    error.clear();
    salida = Entry{};
    const fs::path p(ruta);
    if (!describeUna(p, salida, nullptr, nullptr)) {
        error = "no such file or directory: " + ruta;
        return false;
    }
    salida.path = conBarras(p.filename().generic_string());
    return true;
}

// ── Dueños ───────────────────────────────────────────────────────────────────────────────

void namesUsed(const std::vector<Entry>& entradas, std::vector<std::string>& usuarios,
               std::vector<std::string>& grupos) {
    std::set<std::string> u;
    std::set<std::string> g;
    for (const Entry& e : entradas) {
        if (!e.owner.empty()) {
            u.insert(e.owner);
        }
        if (!e.group.empty()) {
            g.insert(e.group);
        }
    }
    usuarios.assign(u.begin(), u.end());
    grupos.assign(g.begin(), g.end());
}

OwnerMap resolveHere(const std::vector<std::string>& usuarios,
                     const std::vector<std::string>& grupos) {
    OwnerMap m;
#ifndef _WIN32
    // Un nombre cuyo número es −1 cuenta como inexistente: `chown` lee −1 como «no lo
    // cambies». macOS tiene así a «nogroup», y ponerlo dejaba el grupo como estaba.
    for (const std::string& n : usuarios) {
        const struct passwd* pw = ::getpwnam(n.c_str());
        if (pw != nullptr && pw->pw_uid != static_cast<uid_t>(-1)) {
            m.users[n] = static_cast<std::uint32_t>(pw->pw_uid);
        }
    }
    for (const std::string& n : grupos) {
        const struct group* gr = ::getgrnam(n.c_str());
        if (gr != nullptr && gr->gr_gid != static_cast<gid_t>(-1)) {
            m.groups[n] = static_cast<std::uint32_t>(gr->gr_gid);
        }
    }
#else
    (void)usuarios;
    (void)grupos;
#endif
    return m;
}

void expectAtReceiver(std::vector<Entry>& entradas, const OwnerMap& m) {
    for (Entry& e : entradas) {
        const auto u = m.users.find(e.owner);
        const auto g = m.groups.find(e.group);
        e.expectedUid = u != m.users.end() ? u->second : e.uid;
        e.expectedGid = g != m.groups.end() ? g->second : e.gid;
        e.expectedKnown = true;
    }
}

// ── Manifiesto ──────────────────────────────────────────────────────────────────────────
//
// `<tipo> <tamaño> <s> <ns> <modo> <uid> <gid> <especial> <mayor> <menor>
//  <lRuta> <lDestino> <lDueño> <lGrupo> <lHuella>\n` + ruta + destino + dueño + grupo + huella

namespace {

const std::string& huellaDe(const Entry& e, std::string& buf) {
    if (!e.metaDigest.empty() || e.meta.empty()) {
        return e.metaDigest;
    }
    buf = fsmeta::digestOf(e.meta);
    return buf;
}

}  // namespace

std::string serializeManifest(const std::vector<Entry>& entradas) {
    std::string out;
    std::string buf;
    for (const Entry& e : entradas) {
        const std::string& h = huellaDe(e, buf);
        out += letraDe(e.kind);
        for (const unsigned long long n :
             {static_cast<unsigned long long>(e.size), static_cast<unsigned long long>(e.mtime),
              static_cast<unsigned long long>(e.mtimeNsec), static_cast<unsigned long long>(e.mode),
              static_cast<unsigned long long>(e.uid), static_cast<unsigned long long>(e.gid)}) {
            out += ' ';
            out += std::to_string(n);
        }
        out += ' ';
        out += e.special;
        out += ' ' + std::to_string(e.devMajor) + ' ' + std::to_string(e.devMinor);
        out += ' ' + std::to_string(e.path.size()) + ' ' + std::to_string(e.target.size()) + ' '
               + std::to_string(e.owner.size()) + ' ' + std::to_string(e.group.size()) + ' '
               + std::to_string(h.size()) + '\n';
        out += e.path;
        out += e.target;
        out += e.owner;
        out += e.group;
        out += h;
    }
    out += "E\n";
    return out;
}

namespace {

struct LineaManifiesto {
    char letra{0};
    std::uint64_t tam{0};
    std::int64_t s{0};
    std::uint32_t ns{0};
    std::uint32_t modo{0};
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    char especial{'-'};
    std::uint32_t mayor{0};
    std::uint32_t menor{0};
    std::size_t lr{0}, ld{0}, lu{0}, lg{0}, lh{0};
};

bool leeLineaManifiesto(const std::string& linea, LineaManifiesto& l) {
    std::istringstream iss(linea);
    return static_cast<bool>(iss >> l.letra >> l.tam >> l.s >> l.ns >> l.modo >> l.uid >> l.gid
                             >> l.especial >> l.mayor >> l.menor >> l.lr >> l.ld >> l.lu >> l.lg
                             >> l.lh);
}

}  // namespace

bool manifestNamesLength(const std::string& linea, std::size_t& largo) {
    LineaManifiesto l;
    if (!leeLineaManifiesto(linea, l)) {
        largo = 0;
        return false;
    }
    largo = l.lr + l.ld + l.lu + l.lg + l.lh;
    return true;
}

bool parseManifest(const std::string& texto, std::vector<Entry>& salida, std::string& error) {
    salida.clear();
    error.clear();
    std::size_t i = 0;
    while (i < texto.size()) {
        const std::size_t fin = texto.find('\n', i);
        if (fin == std::string::npos) {
            error = "manifest truncated";
            return false;
        }
        const std::string linea = texto.substr(i, fin - i);
        i = fin + 1;
        if (linea == "E") {
            return true;
        }
        LineaManifiesto l;
        if (!leeLineaManifiesto(linea, l)) {
            error = "unreadable manifest line: " + linea;
            return false;
        }
        Entry e;
        if (!tipoDe(l.letra, e.kind)) {
            error = "unknown type in the manifest: " + std::string(1, l.letra);
            return false;
        }
        const std::size_t total = l.lr + l.ld + l.lu + l.lg + l.lh;
        if (i + total > texto.size()) {
            error = "manifest truncated in the names";
            return false;
        }
        e.size = l.tam;
        e.mtime = l.s;
        e.mtimeNsec = l.ns;
        e.mode = l.modo;
        e.uid = l.uid;
        e.gid = l.gid;
        e.special = l.especial;
        e.devMajor = l.mayor;
        e.devMinor = l.menor;
        e.path = texto.substr(i, l.lr);
        e.target = texto.substr(i + l.lr, l.ld);
        e.owner = texto.substr(i + l.lr + l.ld, l.lu);
        e.group = texto.substr(i + l.lr + l.ld + l.lu, l.lg);
        e.metaDigest = texto.substr(i + l.lr + l.ld + l.lu + l.lg, l.lh);
        i += total;
        salida.push_back(std::move(e));
    }
    error = "the manifest does not end in «E»";
    return false;
}

// ── Plan ────────────────────────────────────────────────────────────────────────────────

namespace {

// Un origen sin modos ni dueños de Unix (Windows) manda el modo a 0 y los nombres vacíos. Un
// `chmod 000` de verdad lleva nombres: no se confunden.
bool sinModosDeUnix(const Entry& e) {
    return e.mode == 0 && e.owner.empty() && e.group.empty();
}

bool sinDuenos(const Entry& e) {
    return e.owner.empty() && e.group.empty() && e.uid == 0 && e.gid == 0;
}

// Por los NÚMEROS que el receptor pondrá (expectAtReceiver), que es lo que de verdad queda.
// Un origen sin dueños (Windows) no pide nada, y un destino sin dueños no puede guardarlos.
bool mismoDueno(const Entry& a, const Entry& b) {
    if (sinDuenos(a) || sinDuenos(b)) {
        return true;
    }
    const std::uint32_t u = a.expectedKnown ? a.expectedUid : a.uid;
    const std::uint32_t g = a.expectedKnown ? a.expectedGid : a.gid;
    return u == b.uid && g == b.gid;
}

bool metadatosDistintos(const Entry& a, const Entry& b) {
    if (!mismoDueno(a, b)) {
        return true;
    }
    if (a.kind != EntryKind::Symlink && !sinModosDeUnix(a) && !sinModosDeUnix(b)
        && (a.mode & 07777) != (b.mode & 07777)) {
        return true;
    }
    if (!sameMtime(a.mtime, a.mtimeNsec, b.mtime, b.mtimeNsec)) {
        return true;
    }
    return a.metaDigest != b.metaDigest;
}

bool mismoEspecial(const Entry& a, const Entry& b) {
    if (a.special != b.special) {
        return false;
    }
    return (a.special != 'c' && a.special != 'b')
           || (a.devMajor == b.devMajor && a.devMinor == b.devMinor);
}

Action accionPara(EntryKind k) {
    switch (k) {
        case EntryKind::Directory: return Action::MakeDirectory;
        case EntryKind::File: return Action::Send;
        case EntryKind::Symlink: return Action::Symlink_;
        case EntryKind::HardLink: return Action::HardLink_;
        case EntryKind::Special: return Action::Special_;
    }
    return Action::Send;
}

std::string padreDe(const std::string& ruta) {
    const std::size_t b = ruta.rfind('/');
    return b == std::string::npos ? std::string(".") : ruta.substr(0, b);
}

}  // namespace

Plan makePlan(const std::vector<Entry>& origen, const std::vector<Entry>& destino,
              bool borraLoQueSobra, const std::vector<std::string>& ilegibles) {
    Plan plan;
    std::map<std::string, const Entry*> enDestino;
    for (const Entry& e : destino) {
        enDestino.emplace(e.path, &e);
    }
    std::map<std::string, const Entry*> enOrigen;
    for (const Entry& e : origen) {
        enOrigen.emplace(e.path, &e);
    }
    const auto tieneHijosEnDestino = [&](const std::string& dir) {
        const auto it = enDestino.upper_bound(dir + "/");
        return (it != enDestino.end() && dentroDe(it->first, dir))
               || enDestino.count(dir + "/") > 0;
    };

    // Directorios del destino que se sustituyen por otra cosa: lo que tenían dentro se va con
    // ellos, y no se planea borrarlo otra vez.
    std::vector<std::string> sustituidos;
    for (const Entry& e : origen) {
        const auto it = enDestino.find(e.path);
        const Entry* alli = (it == enDestino.end()) ? nullptr : it->second;
        if (alli == nullptr) {
            plan.operations.push_back({accionPara(e.kind), e, Change::Created, false});
            plan.bytes += e.kind == EntryKind::File ? e.size : 0;
            continue;
        }
        const bool mismoTipo = alli->kind == e.kind
                               && (e.kind != EntryKind::Special || mismoEspecial(e, *alli));
        if (!mismoTipo) {
            // Un fichero y un enlace duro al mismo contenido son el mismo «tipo» a la vista:
            // eso es un cambio de contenido, no de tipo.
            const bool ficheros = (alli->kind == EntryKind::File || alli->kind == EntryKind::HardLink)
                                  && (e.kind == EntryKind::File || e.kind == EntryKind::HardLink);
            if (alli->kind == EntryKind::Directory) {
                if (!borraLoQueSobra && tieneHijosEnDestino(e.path)) {
                    plan.refused.push_back(e.path);
                    continue;
                }
                sustituidos.push_back(e.path);
            }
            plan.operations.push_back(
                {accionPara(e.kind), e, ficheros ? Change::Content : Change::Replaced, false});
            plan.bytes += e.kind == EntryKind::File ? e.size : 0;
            continue;
        }
        switch (e.kind) {
            case EntryKind::Directory:
            case EntryKind::Special:
                if (metadatosDistintos(e, *alli)) {
                    plan.operations.push_back({Action::Metadata_, e, Change::Metadata, false});
                } else {
                    ++plan.unchanged;
                }
                break;
            case EntryKind::Symlink:
                if (alli->target != e.target) {
                    plan.operations.push_back({Action::Symlink_, e, Change::Content, false});
                } else if (metadatosDistintos(e, *alli)) {
                    plan.operations.push_back({Action::Metadata_, e, Change::Metadata, false});
                } else {
                    ++plan.unchanged;
                }
                break;
            case EntryKind::HardLink:
                // Los metadatos son del inodo: van con el primer nombre, no con este.
                if (alli->target != e.target) {
                    plan.operations.push_back({Action::HardLink_, e, Change::Content, false});
                } else {
                    ++plan.unchanged;
                }
                break;
            case EntryKind::File:
                // Tamaño y fecha, que es el criterio de rsync; la fecha con `sameMtime`.
                if (alli->size == e.size
                    && sameMtime(e.mtime, e.mtimeNsec, alli->mtime, alli->mtimeNsec)) {
                    if (metadatosDistintos(e, *alli)) {
                        plan.operations.push_back({Action::Metadata_, e, Change::Metadata, false});
                    } else {
                        ++plan.unchanged;
                    }
                } else {
                    plan.operations.push_back({Action::Send, e, Change::Content, false});
                    plan.bytes += e.size;
                }
                break;
        }
    }

    if (borraLoQueSobra) {
        std::vector<const Entry*> sobran;
        for (const Entry& e : destino) {
            if (enOrigen.count(e.path) > 0) {
                continue;
            }
            const auto bajo = [&](const std::vector<std::string>& dirs) {
                return std::any_of(dirs.begin(), dirs.end(), [&](const std::string& d) {
                    return d.empty() || dentroDe(e.path, d);
                });
            };
            // Ni lo que se va con un directorio sustituido, ni lo que cuelga de un directorio
            // del origen que no se pudo leer: de ese no se sabe qué tiene.
            if (bajo(sustituidos) || bajo(ilegibles)) {
                continue;
            }
            sobran.push_back(&e);
        }
        // De más hondo a menos hondo: un directorio se borra DESPUÉS de lo que tiene dentro.
        std::sort(sobran.begin(), sobran.end(), [](const Entry* a, const Entry* b) {
            const std::size_t ha = hondura(a->path);
            const std::size_t hb = hondura(b->path);
            return ha != hb ? ha > hb : a->path > b->path;
        });
        for (const Entry* e : sobran) {
            plan.operations.push_back({Action::Delete, *e, Change::Deleted, false});
        }
    }

    // El receptor escribe dentro de directorios cuya fecha, en el origen, no ha cambiado:
    // crear, renombrar o borrar dentro se la cambia. Se le devuelve con una operación
    // silenciosa de metadatos, o la pasada siguiente los vería distintos. Es lo que hace
    // copytree al terminar cada directorio.
    std::set<std::string> yaTienen;
    for (const Operation& o : plan.operations) {
        if (o.entry.kind == EntryKind::Directory && o.action != Action::Delete) {
            yaTienen.insert(o.entry.path);
        }
    }
    std::vector<Operation> silenciosas;
    for (const Operation& o : plan.operations) {
        const std::string padre = padreDe(o.entry.path);
        if (yaTienen.count(padre) > 0) {
            continue;
        }
        const auto it = enOrigen.find(padre);
        if (it == enOrigen.end() || it->second->kind != EntryKind::Directory) {
            continue;
        }
        yaTienen.insert(padre);
        silenciosas.push_back({Action::Metadata_, *it->second, Change::Metadata, true});
    }
    plan.operations.insert(plan.operations.end(), silenciosas.begin(), silenciosas.end());
    return plan;
}

std::string describe(const Operation& o) {
    if (o.silent) {
        return {};
    }
    const char* marca = "+ ";
    switch (o.change) {
        case Change::Created: marca = "+ "; break;
        case Change::Replaced: marca = "! "; break;
        case Change::Content: marca = "> "; break;
        case Change::Metadata: marca = ". "; break;
        case Change::Deleted: marca = "- "; break;
    }
    return marca + o.entry.path + (o.entry.kind == EntryKind::Directory ? "/" : "");
}

// ── Cabecera de cada operación ──────────────────────────────────────────────────────────
//
// `<acción> <modo> <s> <ns> <tamaño> <uid> <gid> <especial> <mayor> <menor>
//  <lRuta> <lDestino> <lDueño> <lGrupo> <lMeta>\n` + los cinco, seguidos.

std::string headerOf(const Operation& o) {
    const Entry& e = o.entry;
    const std::string meta = fsmeta::serialize(e.meta);
    std::string h;
    h += letraDe(o.action);
    h += ' ' + std::to_string(e.mode) + ' ' + std::to_string(e.mtime) + ' '
         + std::to_string(e.mtimeNsec) + ' ' + std::to_string(e.size) + ' '
         + std::to_string(e.uid) + ' ' + std::to_string(e.gid) + ' ';
    h += e.special;
    h += ' ' + std::to_string(e.devMajor) + ' ' + std::to_string(e.devMinor) + ' '
         + std::to_string(e.path.size()) + ' ' + std::to_string(e.target.size()) + ' '
         + std::to_string(e.owner.size()) + ' ' + std::to_string(e.group.size()) + ' '
         + std::to_string(meta.size()) + '\n';
    h += e.path;
    h += e.target;
    h += e.owner;
    h += e.group;
    h += meta;
    return h;
}

bool parseHeader(const std::string& linea, Operation& salida, HeaderLengths& l,
                 std::string& error) {
    error.clear();
    std::istringstream iss(linea);
    char letra = 0;
    Entry& e = salida.entry;
    e = Entry{};
    if (!(iss >> letra >> e.mode >> e.mtime >> e.mtimeNsec >> e.size >> e.uid >> e.gid
          >> e.special >> e.devMajor >> e.devMinor >> l.path >> l.target >> l.owner >> l.group
          >> l.meta)) {
        error = "unreadable header: " + linea;
        return false;
    }
    if (!accionDe(letra, salida.action)) {
        error = "unknown action: " + std::string(1, letra);
        return false;
    }
    return true;
}

bool takeNames(const std::string& n, const HeaderLengths& l, Operation& o, std::string& error) {
    if (n.size() != l.total()) {
        error = "names of the wrong length";
        return false;
    }
    std::size_t i = 0;
    o.entry.path = n.substr(i, l.path);
    i += l.path;
    o.entry.target = n.substr(i, l.target);
    i += l.target;
    o.entry.owner = n.substr(i, l.owner);
    i += l.owner;
    o.entry.group = n.substr(i, l.group);
    i += l.group;
    return fsmeta::parse(n.substr(i, l.meta), o.entry.meta, error);
}

// ── Contenido con huecos ────────────────────────────────────────────────────────────────

namespace {

constexpr std::size_t kBloqueCeros = 4096;

bool sonCeros(const char* p, std::size_t n) {
    static const char ceros[kBloqueCeros] = {};
    return std::memcmp(p, ceros, n) == 0;
}

}  // namespace

bool readStretches(const std::string& ruta, std::uint64_t tam,
                   const std::function<bool(const char*, std::size_t)>& emite,
                   std::string& error) {
    constexpr std::size_t kBuf = 256 * 1024;
    std::vector<char> buf(kBuf);
    std::string datos;          // datos pendientes de emitir
    std::uint64_t hueco = 0;    // hueco pendiente de emitir
    const auto vacia = [&]() {
        if (!datos.empty()) {
            if (!emite(datos.data(), datos.size())) {
                return false;
            }
            datos.clear();
        }
        if (hueco > 0) {
            while (hueco > 0) {
                const std::size_t n =
                    static_cast<std::size_t>(std::min<std::uint64_t>(hueco, 1ULL << 30));
                if (!emite(nullptr, n)) {
                    return false;
                }
                hueco -= n;
            }
        }
        return true;
    };
    // Un trozo leído: por bloques de 4 KiB, los de ceros van al hueco.
    const auto trozo = [&](const char* p, std::size_t n) {
        for (std::size_t off = 0; off < n; off += kBloqueCeros) {
            const std::size_t len = std::min(kBloqueCeros, n - off);
            if (sonCeros(p + off, len)) {
                if (!datos.empty() && !vacia()) {
                    return false;
                }
                hueco += len;
            } else {
                if (hueco > 0 && !vacia()) {
                    return false;
                }
                datos.append(p + off, len);
                if (datos.size() >= kBuf && !vacia()) {
                    return false;
                }
            }
        }
        return true;
    };
#ifdef _WIN32
    std::FILE* f = _wfopen(fs::path(ruta).wstring().c_str(), L"rb");
    if (f == nullptr) {
        error = "could not read " + ruta;
        return false;
    }
    std::uint64_t leido = 0;
    while (leido < tam) {
        const std::size_t quiero =
            static_cast<std::size_t>(std::min<std::uint64_t>(kBuf, tam - leido));
        const std::size_t n = std::fread(buf.data(), 1, quiero, f);
        if (n == 0) {
            break;
        }
        if (!trozo(buf.data(), n)) {
            std::fclose(f);
            error = "the connection dropped";
            return false;
        }
        leido += n;
    }
    std::fclose(f);
    if (leido != tam) {
        error = "«" + ruta + "» changed size while it was being sent";
        return false;
    }
#else
    const int fd = ::open(ruta.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "could not read " + ruta + ": " + std::strerror(errno);
        return false;
    }
    const off_t total = static_cast<off_t>(tam);
    off_t pos = 0;
    bool bien = true;
    while (bien && pos < total) {
        off_t ini = pos;
        off_t fin = total;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
        // Los huecos que el sistema de ficheros conoce ni se leen.
        const off_t d = ::lseek(fd, pos, SEEK_DATA);
        if (d < 0 && errno == ENXIO) {
            ini = total;
        } else if (d >= 0) {
            ini = std::min(d, total);
            const off_t h = ::lseek(fd, ini, SEEK_HOLE);
            fin = h < 0 ? total : std::min(h, total);
        }
#endif
        hueco += static_cast<std::uint64_t>(ini - pos);
        for (off_t at = ini; bien && at < fin;) {
            const std::size_t quiero =
                static_cast<std::size_t>(std::min<off_t>(static_cast<off_t>(kBuf), fin - at));
            const ssize_t n = ::pread(fd, buf.data(), quiero, at);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                // Se anunció un tamaño en la cabecera y el otro extremo cuenta con él.
                bien = false;
                error = "«" + ruta + "» changed size while it was being sent";
                break;
            }
            if (!trozo(buf.data(), static_cast<std::size_t>(n))) {
                bien = false;
                error = "the connection dropped";
                break;
            }
            at += n;
        }
        pos = fin;
    }
    ::close(fd);
    if (!bien) {
        return false;
    }
#endif
    return vacia();
}

std::string tempPathNextTo(const std::string& ruta) {
    static std::atomic<unsigned> contador{0};
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return (fs::path(ruta).parent_path()
            / (".zfsmgr-" + std::to_string(pid) + "-" + std::to_string(contador.fetch_add(1))
               + ".tmp"))
        .string();
}

SparseWriter::~SparseWriter() {
#ifdef _WIN32
    if (file_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(file_));
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
}

bool SparseWriter::open(const std::string& ruta, std::string& error) {
    path_ = ruta;
    pos_ = 0;
#ifdef _WIN32
    const std::wstring w = fs::path(ruta).wstring();
    std::FILE* f = _wfopen(w.c_str(), L"wb");
    if (f == nullptr) {
        error = "could not create " + ruta;
        return false;
    }
    std::fclose(f);
    // Disperso ANTES de escribir: sin esto, los huecos se materializan como ceros.
    const HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD ret = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
        CloseHandle(h);
    }
    file_ = _wfopen(w.c_str(), L"r+b");
    if (file_ == nullptr) {
        error = "could not open for writing " + ruta;
        return false;
    }
#else
    // 0666 menos la umask, como cualquier fichero nuevo: si el origen no tiene modos de Unix
    // (Windows) no se le pone otro, y con 0600 se quedaba cerrado para todos menos root.
    fd_ = ::open(ruta.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd_ < 0) {
        error = "could not create " + ruta + ": " + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

bool SparseWriter::write(const char* p, std::size_t n) {
    for (std::size_t off = 0; off < n; off += kBloqueCeros) {
        const std::size_t len = std::min(kBloqueCeros, n - off);
        if (sonCeros(p + off, len)) {
            pos_ += len;
            continue;
        }
#ifdef _WIN32
        std::FILE* f = static_cast<std::FILE*>(file_);
        if (_fseeki64(f, static_cast<long long>(pos_), SEEK_SET) != 0
            || std::fwrite(p + off, 1, len, f) != len) {
            return false;
        }
#else
        std::size_t hecho = 0;
        while (hecho < len) {
            const ssize_t w =
                ::pwrite(fd_, p + off + hecho, len - hecho, static_cast<off_t>(pos_ + hecho));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return false;
            }
            hecho += static_cast<std::size_t>(w);
        }
#endif
        pos_ += len;
    }
    return true;
}

bool SparseWriter::finish(std::string& error) {
    // El tamaño final, siempre: un fichero que TERMINA en hueco no se alarga solo. Con
    // truncate y no escribiendo un byte al final, que materializaría un bloque.
#ifdef _WIN32
    if (file_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(file_));
        file_ = nullptr;
    }
    std::error_code ec;
    fs::resize_file(fs::path(path_), pos_, ec);
    if (ec) {
        error = "could not set the size of " + path_ + ": " + ec.message();
        return false;
    }
#else
    const bool ok = ::ftruncate(fd_, static_cast<off_t>(pos_)) == 0;
    const int e1 = errno;
    const bool cerrado = ::close(fd_) == 0;
    fd_ = -1;
    if (!ok || !cerrado) {
        error = "could not write " + path_ + ": " + std::strerror(ok ? errno : e1);
        return false;
    }
#endif
    return true;
}

// ── Aplicar en el destino ───────────────────────────────────────────────────────────────

int applyMetadata(const std::string& ruta, const Entry& e) {
    int fallos = 0;
#ifdef _WIN32
    if (e.kind != EntryKind::Symlink && !setMtime(ruta, e.mtime, e.mtimeNsec)) {
        ++fallos;
    }
#else
    struct stat st {};
    const bool esEnlace = ::lstat(ruta.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
    // El orden, el de copytree: dueño (cambiarlo borra setuid), atributos (la ACL POSIX de
    // Linux reescribe la máscara), modo, la ACL de FreeBSD y macOS (ahí es el modo el que la
    // reescribiría) y la fecha la última, porque todo lo anterior la toca.
    if (!setOwner(ruta, e.uid, e.gid, e.owner, e.group)) {
        ++fallos;
    }
    fallos += fsmeta::syncXattrs(ruta, e.meta.xattrs);
#if defined(__FreeBSD__) || defined(__APPLE__)
    // FreeBSD y macOS dan modo propio a un enlace simbólico (Linux lo deja siempre en 0777 y
    // no deja cambiarlo). Se pone donde se puede, como rsync con lchmod; no cuenta como fallo.
    if (esEnlace && !sinModosDeUnix(e)) {
        (void)::fchmodat(AT_FDCWD, ruta.c_str(), static_cast<mode_t>(e.mode & 07777),
                         AT_SYMLINK_NOFOLLOW);
    }
#endif
    if (!esEnlace) {
        if (!sinModosDeUnix(e) && !setMode(ruta, e.mode)) {
            ++fallos;
        }
        if (!fsmeta::setAcl(ruta, e.meta.acl)) {
            ++fallos;
        }
    }
    if (!setMtime(ruta, e.mtime, e.mtimeNsec)) {
        ++fallos;
    }
#endif
    return fallos;
}

bool makeSpecial(const std::string& ruta, const Entry& e, std::string& error) {
#ifdef _WIN32
    (void)e;
    error = "special files do not exist on Windows: " + ruta;
    return false;
#else
    const mode_t perm = static_cast<mode_t>(e.mode & 07777);
    int rc = -1;
    switch (e.special) {
        case 'p': rc = ::mkfifo(ruta.c_str(), perm); break;
        case 'c':
            rc = ::mknod(ruta.c_str(), S_IFCHR | perm, makedev(e.devMajor, e.devMinor));
            break;
        case 'b':
            rc = ::mknod(ruta.c_str(), S_IFBLK | perm, makedev(e.devMajor, e.devMinor));
            break;
        case 's': {
            rc = ::mknod(ruta.c_str(), S_IFSOCK | perm, 0);
            if (rc != 0) {
                // macOS y FreeBSD no crean sockets con mknod: hay que atarlo, como hace rsync.
                struct sockaddr_un addr {};
                if (ruta.size() < sizeof(addr.sun_path)) {
                    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                    if (fd >= 0) {
                        addr.sun_family = AF_UNIX;
                        std::memcpy(addr.sun_path, ruta.c_str(), ruta.size() + 1);
                        rc = ::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr),
                                    sizeof(addr));
                        ::close(fd);
                    }
                }
            }
            break;
        }
        default:
            error = "unknown special file «" + std::string(1, e.special) + "»";
            return false;
    }
    if (rc != 0) {
        error = "could not create the special file " + ruta + ": " + std::strerror(errno);
        return false;
    }
    return true;
#endif
}

bool makeHardLink(const std::string& existente, const std::string& enlace, std::string& error) {
#ifdef _WIN32
    if (CreateHardLinkW(fs::path(enlace).wstring().c_str(), fs::path(existente).wstring().c_str(),
                        nullptr)
        == 0) {
        error = "could not create the hard link " + enlace;
        return false;
    }
#else
    if (::linkat(AT_FDCWD, existente.c_str(), AT_FDCWD, enlace.c_str(), 0) != 0) {
        error = "could not create the hard link " + enlace + ": " + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

// --- Delta ------------------------------------------------------------------

std::size_t blockSize(std::uint64_t tamanoFichero) {
    if (tamanoFichero < 16ULL * 1024 * 1024) {
        return 8 * 1024;
    }
    if (tamanoFichero < 1024ULL * 1024 * 1024) {
        return 64 * 1024;
    }
    return 256 * 1024;
}

std::uint32_t rollingSum(const unsigned char* datos, std::size_t n) {
    // La de rsync: una suma de los bytes y otra ponderada por la posición. La segunda es la
    // que hace que dos trozos con los mismos bytes en distinto orden NO coincidan.
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a += datos[i];
        b += static_cast<std::uint32_t>(n - i) * datos[i];
    }
    return (a & 0xffff) | ((b & 0xffff) << 16);
}

bool fileHash(const std::string& ruta, std::string& hexOut, std::string& error) {
    hexOut.clear();
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "could not open " + ruta;
        return false;
    }
    // EVP y no `SHA256_Init/Update/Final`: esos están obsoletos desde OpenSSL 3.0 y avisan
    // en cada compilación.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (ctx != nullptr) { EVP_MD_CTX_free(ctx); }
        std::fclose(f);
        error = "could not start the digest";
        return false;
    }
    std::vector<unsigned char> buf(65536);
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            break;
        }
        EVP_DigestUpdate(ctx, buf.data(), n);
    }
    std::fclose(f);
    unsigned char h[SHA256_DIGEST_LENGTH];
    unsigned int largo = 0;
    EVP_DigestFinal_ex(ctx, h, &largo);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : h) {
        hexOut.push_back(hex[c >> 4]);
        hexOut.push_back(hex[c & 0x0f]);
    }
    return true;
}

bool signaturesOf(const std::string& ruta, std::size_t tamBloque, std::vector<Signature>& salida,
              std::string& error) {
    salida.clear();
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "could not open " + ruta;
        return false;
    }
    std::vector<unsigned char> buf(tamBloque);
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, tamBloque, f);
        if (n == 0) {
            break;
        }
        Signature fi;
        fi.weak = rollingSum(buf.data(), n);
        unsigned char h[SHA256_DIGEST_LENGTH];
        SHA256(buf.data(), n, h);
        std::memcpy(fi.strong, h, sizeof(fi.strong));
        salida.push_back(fi);
        if (n < tamBloque) {
            break;  // el último bloque puede ser corto
        }
    }
    std::fclose(f);
    return true;
}

std::string serializeSignatures(const std::vector<Signature>& f) {
    std::string out;
    out.reserve(f.size() * (4 + 16));
    for (const Signature& x : f) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((x.weak >> (8 * i)) & 0xff));
        }
        out.append(reinterpret_cast<const char*>(x.strong), sizeof(x.strong));
    }
    return out;
}

bool parseSignatures(const std::string& datos, std::vector<Signature>& salida, std::string& error) {
    salida.clear();
    if (datos.size() % 20 != 0) {
        error = "the signatures arrive incomplete";
        return false;
    }
    for (std::size_t i = 0; i < datos.size(); i += 20) {
        Signature f;
        f.weak = 0;
        for (int k = 0; k < 4; ++k) {
            f.weak |= static_cast<std::uint32_t>(static_cast<unsigned char>(datos[i + k]))
                       << (8 * k);
        }
        std::memcpy(f.strong, datos.data() + i + 4, sizeof(f.strong));
        salida.push_back(f);
    }
    return true;
}

bool delta(const std::string& ruta, const std::vector<Signature>& firmas, std::size_t tamBloque,
           std::vector<Instruction>& salida, std::uint64_t& bytesLiterales,
           std::string& error) {
    salida.clear();
    bytesLiterales = 0;
    if (tamBloque == 0) {
        error = "zero block size";
        return false;
    }
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "could not open " + ruta;
        return false;
    }

    // Suma débil → qué bloques la tienen. Varios pueden compartirla; por eso el hash fuerte.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> porDebil;
    for (std::size_t i = 0; i < firmas.size(); ++i) {
        porDebil[firmas[i].weak].push_back(i);
    }

    // El buffer lleva SIEMPRE al menos una ventana entera por delante mientras quede
    // fichero. Sin ese arrastre habría que releer, y la búsqueda dejaría de ser de una sola
    // pasada.
    const std::size_t capacidad = std::max<std::size_t>(tamBloque * 4, 1024 * 1024);
    std::vector<unsigned char> buf(capacidad);
    std::size_t ini = 0;
    std::size_t fin = 0;
    bool acabado = false;
    std::string literal;

    const auto rellena = [&]() {
        if (ini > 0) {
            std::memmove(buf.data(), buf.data() + ini, fin - ini);
            fin -= ini;
            ini = 0;
        }
        while (!acabado && fin < buf.size()) {
            const std::size_t n = std::fread(buf.data() + fin, 1, buf.size() - fin, f);
            if (n == 0) {
                acabado = true;
                break;
            }
            fin += n;
        }
    };

    const auto sueltaLiteral = [&]() {
        if (literal.empty()) {
            return;
        }
        Instruction in;
        in.kind = InstructionKind::Literal;
        in.data = literal;
        bytesLiterales += literal.size();
        salida.push_back(std::move(in));
        literal.clear();
    };

    rellena();
    std::uint32_t suma = 0;
    bool haySuma = false;
    // El relleno va ANTES de comprobar si queda algo, y no al revés.
    //
    // Con `while (ini < fin)` delante, un consumo que caía justo en el final del búfer
    // dejaba `ini == fin` y el bucle salía SIN volver a leer: el delta describía solo el
    // primer búfer del fichero y el resto desaparecía. Con 8 KiB de bloque el búfer es de
    // 1 MiB, así que cualquier fichero mayor se truncaba. No se vio en las aserciones
    // porque allí el delta se comprobaba en memoria; lo cazó la verificación del hash del
    // fichero entero al aplicarlo, que para eso está.
    for (;;) {
        if (fin - ini < tamBloque && !acabado) {
            rellena();
            haySuma = false;
        }
        if (ini >= fin) {
            break;
        }
        const std::size_t ventana = std::min<std::size_t>(tamBloque, fin - ini);
        if (!haySuma || ventana != tamBloque) {
            suma = rollingSum(buf.data() + ini, ventana);
            haySuma = true;
        }

        bool coincidio = false;
        const auto it = porDebil.find(suma);
        if (it != porDebil.end()) {
            unsigned char h[SHA256_DIGEST_LENGTH];
            SHA256(buf.data() + ini, ventana, h);
            // Entre varios bloques que valgan, se prefiere EL SIGUIENTE al último copiado.
            //
            // No es un capricho: en un fichero con trozos repetidos —ceros, relleno, un
            // disco virtual medio vacío— todos esos bloques tienen la misma firma, y coger
            // siempre el primero de la lista genera una instrucción por bloque en vez de
            // una para todos. Medido: 300 KB de un solo byte repetido daban 37
            // instrucciones; con esto, una.
            std::vector<std::size_t> candidatos = it->second;
            if (!salida.empty() && salida.back().kind == InstructionKind::Send) {
                const std::size_t siguiente =
                    static_cast<std::size_t>(salida.back().block + salida.back().howMany);
                for (std::size_t k = 0; k < candidatos.size(); ++k) {
                    if (candidatos[k] == siguiente) {
                        std::swap(candidatos[0], candidatos[k]);
                        break;
                    }
                }
            }
            for (const std::size_t idx : candidatos) {
                if (std::memcmp(firmas[idx].strong, h, sizeof(firmas[idx].strong)) != 0) {
                    continue;
                }
                sueltaLiteral();
                // Bloques seguidos se juntan en una sola instrucción: un fichero que no ha
                // cambiado nada se resuelve con UNA, no con una por bloque.
                if (!salida.empty() && salida.back().kind == InstructionKind::Send
                    && salida.back().block + salida.back().howMany == idx) {
                    ++salida.back().howMany;
                } else {
                    Instruction in;
                    in.kind = InstructionKind::Send;
                    in.block = idx;
                    in.howMany = 1;
                    salida.push_back(in);
                }
                ini += ventana;
                haySuma = false;
                coincidio = true;
                break;
            }
        }
        if (coincidio) {
            continue;
        }

        // No hubo coincidencia: este byte es literal y la ventana avanza UNO. Aquí es donde la suma
        // rodante se gana su nombre: se actualiza sin volver a recorrer la ventana.
        literal.push_back(static_cast<char>(buf[ini]));
        const unsigned char sale = buf[ini];
        ++ini;
        if (haySuma && ventana == tamBloque && ini + tamBloque <= fin) {
            const unsigned char entra = buf[ini + tamBloque - 1];
            std::uint32_t a = suma & 0xffff;
            std::uint32_t b = (suma >> 16) & 0xffff;
            a = (a - sale + entra) & 0xffff;
            b = (b - static_cast<std::uint32_t>(tamBloque) * sale + a) & 0xffff;
            suma = a | (b << 16);
        } else {
            haySuma = false;
        }
    }
    sueltaLiteral();
    std::fclose(f);
    return true;
}

bool setMtime(const std::string& ruta, std::int64_t segundos, std::uint32_t nanos) {
#ifdef _WIN32
    // FILE_FLAG_OPEN_REPARSE_POINT: la fecha del propio enlace, no la de su destino.
    HANDLE h = CreateFileW(fs::path(ruta).wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    ULARGE_INTEGER t;
    t.QuadPart = (static_cast<unsigned long long>(segundos) + 11644473600ULL) * 10000000ULL
                 + nanos / 100U;
    FILETIME ft;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
#else
    // La de acceso no viaja: se deja como esté.
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = static_cast<time_t>(segundos);
    ts[1].tv_nsec = static_cast<long>(nanos);
    return ::utimensat(AT_FDCWD, ruta.c_str(), ts, AT_SYMLINK_NOFOLLOW) == 0;
#endif
}

bool setOwner(const std::string& ruta, std::uint32_t uid, std::uint32_t gid,
              const std::string& usuario, const std::string& grupo) {
#ifdef _WIN32
    (void)ruta; (void)uid; (void)gid; (void)usuario; (void)grupo;
    // En Windows el dueño es un SID y no un uid: traducirlo sería inventarse una
    // equivalencia que no existe. Mejor no tocarlo que ponerlo mal.
    return true;
#else
    if (uid == 0 && gid == 0 && usuario.empty() && grupo.empty()) {
        return true;  // nada que poner: un origen sin dueño (Windows, donde es un SID)
    }
    // El NOMBRE manda y el número es el respaldo. Si el nombre existe en esta máquina, ese
    // es el dueño correcto aunque su número sea otro; y si no existe —una cuenta que solo
    // hay en la máquina de origen— se usa el número, que al menos conserva la separación
    // entre dueños distintos.
    uid_t u = static_cast<uid_t>(uid);
    gid_t g = static_cast<gid_t>(gid);
    if (!usuario.empty()) {
        const struct passwd* pw = ::getpwnam(usuario.c_str());
        if (pw != nullptr && pw->pw_uid != static_cast<uid_t>(-1)) {
            u = pw->pw_uid;
        }
    }
    if (!grupo.empty()) {
        const struct group* gr = ::getgrnam(grupo.c_str());
        if (gr != nullptr && gr->gr_gid != static_cast<gid_t>(-1)) {
            g = gr->gr_gid;
        }
    }
    // `lchown` y no `chown`: de un enlace simbólico se cambia el enlace, no su destino.
    return ::lchown(ruta.c_str(), u, g) == 0;
#endif
}

bool setMode(const std::string& ruta, std::uint32_t modo) {
#ifdef _WIN32
    (void)ruta;
    (void)modo;
    // NTFS no tiene modo de Unix, y emularlo con ACL sería una aproximación justo donde
    // más se nota. Se deja como está: mejor no tocar los permisos que ponerlos mal.
    return true;
#else
    // Un modo 0 es legítimo (`chmod 000`), salvo que venga de un origen sin modos: eso lo
    // decide quien llama, que sabe de dónde viene.
    return ::chmod(ruta.c_str(), static_cast<mode_t>(modo & 07777)) == 0;
#endif
}

}  // namespace zfsmgr::remotetree
