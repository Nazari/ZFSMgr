#include "copytree.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace fs = std::filesystem;

namespace zfsmgr::copytree {
namespace {

// Identidad de un fichero en su sistema de ficheros.
//
// Es lo que permite saber que dos rutas son EL MISMO fichero y recrear el enlace duro en
// vez de copiar los datos dos veces. En Unix es el par dispositivo+inodo; en Windows, el
// número de serie del volumen y el índice de fichero.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t index = 0;
    std::uint32_t linkCount = 1;
    bool valid = false;
};

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

FileIdentity identityOf(const fs::path& p) {
    FileIdentity id;
    const HANDLE h = CreateFileW(p.wstring().c_str(), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return id;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info)) {
        id.volume = info.dwVolumeSerialNumber;
        id.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        id.linkCount = info.nNumberOfLinks;
        id.valid = true;
    }
    CloseHandle(h);
    return id;
}

bool isReparsePoint(const fs::path& p) {
    const DWORD attr = GetFileAttributesW(p.wstring().c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Marcas de tiempo y ACL. En Windows los permisos NO son un modo POSIX: son una lista de
// control de acceso, y copiar el «modo» equivalente sería inventarse una correspondencia.
// Se copia la ACL de verdad, junto con propietario y grupo.
void copyMetadata(const fs::path& src, const fs::path& dst, bool isDir) {
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE hs = CreateFileW(src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, flags, nullptr);
    if (hs != INVALID_HANDLE_VALUE) {
        FILETIME cr{}, ac{}, wr{};
        if (GetFileTime(hs, &cr, &ac, &wr)) {
            HANDLE hd = CreateFileW(dst.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
            if (hd != INVALID_HANDLE_VALUE) {
                SetFileTime(hd, &cr, &ac, &wr);
                CloseHandle(hd);
            }
        }
        CloseHandle(hs);
    }
    (void)isDir;

    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    PSID group = nullptr;
    std::wstring srcW = src.wstring();
    if (GetNamedSecurityInfoW(srcW.c_str(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
                                  | DACL_SECURITY_INFORMATION,
                              &owner, &group, &dacl, nullptr, &sd)
        == ERROR_SUCCESS) {
        std::wstring dstW = dst.wstring();
        SetNamedSecurityInfoW(dstW.data(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
                                  | DACL_SECURITY_INFORMATION,
                              owner, group, dacl, nullptr);
        if (sd) {
            LocalFree(sd);
        }
    }
}

bool createHardLinkAt(const fs::path& existing, const fs::path& link) {
    return CreateHardLinkW(link.wstring().c_str(), existing.wstring().c_str(), nullptr) != 0;
}

// Marca el destino como disperso ANTES de escribir. Sin esto, los huecos que se dejan
// saltando con seek se materializan como ceros en disco y la copia ocupa más que el
// original.
void markSparse(const fs::path& p) {
    const HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD ret = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    CloseHandle(h);
}

#else  // POSIX

FileIdentity identityOf(const fs::path& p) {
    FileIdentity id;
    struct stat st {};
    if (::lstat(p.c_str(), &st) == 0) {
        id.volume = static_cast<std::uint64_t>(st.st_dev);
        id.index = static_cast<std::uint64_t>(st.st_ino);
        id.linkCount = static_cast<std::uint32_t>(st.st_nlink);
        id.valid = true;
    }
    return id;
}

bool isReparsePoint(const fs::path&) { return false; }

void copyExtendedAttributes(const fs::path& src, const fs::path& dst);

void copyMetadata(const fs::path& src, const fs::path& dst, bool isDir) {
    struct stat st {};
    if (::lstat(src.c_str(), &st) != 0) {
        return;
    }
    // El propietario primero: en Linux, cambiar el dueño de un fichero con setuid borra
    // esos bits, así que hacerlo al revés dejaría los permisos mal.
    (void)::chown(dst.c_str(), st.st_uid, st.st_gid);
    if (!isDir || true) {
        (void)::chmod(dst.c_str(), st.st_mode & 07777);
    }
    // Los nombres de los campos de tiempo NO son los mismos en todas partes: POSIX 2008
    // dice st_atim, y macOS conserva los de BSD, st_atimespec. Linux y FreeBSD compilaban
    // sin protestar y macOS no, que es exactamente para lo que el orden de compilación
    // pone macOS por delante.
    struct timespec times[2];
#if defined(__APPLE__)
    times[0] = st.st_atimespec;
    times[1] = st.st_mtimespec;
#else
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
#endif
    // Los atributos ANTES de las marcas de tiempo: reponer la ACL vuelve a tocar ctime,
    // y el orden inverso dejaría las marcas del destino distintas de las del origen.
    copyExtendedAttributes(src, dst);
    (void)::utimensat(AT_FDCWD, dst.c_str(), times, 0);
}

// Atributos extendidos, y con ellos las ACL POSIX.
//
// Las ACL de Linux NO son un modo: viven en el atributo extendido
// `system.posix_acl_access`. Copiando los atributos vienen incluidas, y así no hace
// falta enlazar libacl en un binario que viaja solo a máquinas ajenas.
//
// Hizo falta porque al sustituir rsync se perdieron: `rsync -A` conservaba
// `user:nobody:rwx` y esta copia lo dejaba fuera. Se vio comparando las dos copias del
// mismo fichero, no leyendo el código.
//
// Se usan las variantes l* para no seguir enlaces simbólicos, y cada atributo se
// intenta por separado: algunos —`security.selinux`, por ejemplo— pueden no ser
// escribibles, y eso no debe abortar la copia entera.
void copyExtendedAttributes(const fs::path& src, const fs::path& dst) {
#if defined(__linux__) || defined(__APPLE__)
#if defined(__APPLE__)
    const auto listAttrs = [](const char* p, char* buf, std::size_t sz) {
        return ::listxattr(p, buf, sz, XATTR_NOFOLLOW);
    };
    const auto getAttr = [](const char* p, const char* n, void* v, std::size_t sz) {
        return ::getxattr(p, n, v, sz, 0, XATTR_NOFOLLOW);
    };
    const auto setAttr = [](const char* p, const char* n, const void* v, std::size_t sz) {
        return ::setxattr(p, n, v, sz, 0, XATTR_NOFOLLOW);
    };
#else
    const auto listAttrs = [](const char* p, char* buf, std::size_t sz) {
        return ::llistxattr(p, buf, sz);
    };
    const auto getAttr = [](const char* p, const char* n, void* v, std::size_t sz) {
        return ::lgetxattr(p, n, v, sz);
    };
    const auto setAttr = [](const char* p, const char* n, const void* v, std::size_t sz) {
        return ::lsetxattr(p, n, v, sz, 0);
    };
#endif
    const ssize_t listLen = listAttrs(src.c_str(), nullptr, 0);
    if (listLen <= 0) {
        return;
    }
    std::vector<char> names(static_cast<std::size_t>(listLen));
    if (listAttrs(src.c_str(), names.data(), names.size()) != listLen) {
        return;
    }
    std::size_t off = 0;
    while (off < names.size()) {
        const char* name = names.data() + off;
        const std::size_t len = std::strlen(name);
        if (len == 0) {
            break;
        }
        off += len + 1;
        const ssize_t valLen = getAttr(src.c_str(), name, nullptr, 0);
        if (valLen < 0) {
            continue;
        }
        std::vector<char> value(static_cast<std::size_t>(valLen));
        if (valLen > 0 && getAttr(src.c_str(), name, value.data(), value.size()) != valLen) {
            continue;
        }
        (void)setAttr(dst.c_str(), name, value.data(), value.size());
    }
#else
    // FreeBSD usa extattr_*, con otra interfaz y espacios de nombres distintos. No se
    // copian ahí, igual que rsync tampoco lo hacía sin soporte detectado.
    (void)src;
    (void)dst;
#endif
}

bool createHardLinkAt(const fs::path& existing, const fs::path& link) {
    return ::link(existing.c_str(), link.c_str()) == 0;
}

void markSparse(const fs::path&) {}

#endif

// Copia el contenido de un fichero conservando los huecos.
//
// No se usa std::filesystem::copy_file: escribiría los ceros de un fichero disperso como
// datos reales, y una imagen de disco de 100 GB con 2 GB usados llegaría ocupando 100.
// Aquí, un bloque entero de ceros no se escribe: se salta con seek, y el sistema de
// ficheros deja el hueco.
bool copyFileData(const fs::path& src, const fs::path& dst, std::uint64_t& bytesOut,
                  std::string& err) {
    std::FILE* in = std::fopen(src.string().c_str(), "rb");
    if (!in) {
        err = "no se pudo leer " + src.string();
        return false;
    }
    std::FILE* out = std::fopen(dst.string().c_str(), "wb");
    if (!out) {
        std::fclose(in);
        err = "no se pudo crear " + dst.string();
        return false;
    }
    // Se marca disperso con el fichero ya creado y antes de escribir nada.
    std::fclose(out);
    markSparse(dst);
    out = std::fopen(dst.string().c_str(), "r+b");
    if (!out) {
        std::fclose(in);
        err = "no se pudo abrir para escribir " + dst.string();
        return false;
    }

    constexpr std::size_t kBuf = 256 * 1024;
    std::vector<char> buf(kBuf);
    bool ok = true;
    std::uint64_t written = 0;
    std::uint64_t holePending = 0;
    while (true) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) {
            break;
        }
        const bool allZero = std::all_of(buf.begin(), buf.begin() + static_cast<long>(n),
                                         [](char c) { return c == '\0'; });
        if (allZero) {
            holePending += n;
            continue;
        }
        if (holePending > 0) {
            if (std::fseek(out, static_cast<long>(holePending), SEEK_CUR) != 0) {
                ok = false;
                err = "no se pudo dejar el hueco en " + dst.string();
                break;
            }
            holePending = 0;
        }
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            err = "no se pudo escribir en " + dst.string();
            break;
        }
        written += n;
    }
    std::fclose(in);
    const std::uint64_t finalSize = written + holePending;
    std::fclose(out);

    // Un fichero que TERMINA en ceros: saltar con seek no alarga el fichero, así que hay
    // que fijar el tamaño final aparte.
    //
    // Con truncate y no escribiendo un byte al final, que es lo que se hacía antes: ese
    // byte materializaba un bloque, y un fichero enteramente disperso acababa ocupando
    // 4 KiB donde rsync deja 0. Se vio comparando las dos copias del mismo árbol.
    if (ok && holePending > 0) {
        std::error_code rec;
        fs::resize_file(dst, finalSize, rec);
        if (rec) {
            err = "no se pudo fijar el tamaño de " + dst.string() + ": " + rec.message();
            return false;
        }
    }
    bytesOut += written;
    return ok;
}

// ¿El destino ya tiene este fichero, igual? Tamaño y fecha, que es el criterio de
// rsync por omisión. No compara contenido: dos ficheros del mismo tamaño y la misma
// fecha con datos distintos existen, pero comprobarlo obligaría a leerlo todo en cada
// pasada y entonces saltárselo no ahorraría nada.
bool sameAlreadyThere(const fs::path& src, const fs::path& dst) {
    std::error_code e1;
    std::error_code e2;
    const auto dstSize = fs::file_size(dst, e1);
    if (e1) {
        return false;
    }
    const auto srcSize = fs::file_size(src, e2);
    if (e2 || dstSize != srcSize) {
        return false;
    }
    std::error_code e3;
    std::error_code e4;
    const auto dstTime = fs::last_write_time(dst, e3);
    const auto srcTime = fs::last_write_time(src, e4);
    return !e3 && !e4 && dstTime == srcTime;
}

struct Walker {
    Options opt;
    Result res;
    std::uint64_t rootVolume = 0;
    // Identidad → primera ruta ya copiada en el destino. Solo entran los ficheros con
    // más de un enlace, así que la memoria es proporcional a esos y no al total.
    std::map<std::pair<std::uint64_t, std::uint64_t>, fs::path> hardLinks;

    bool excluded(const std::string& name, int depth) const {
        if (depth != 0) {
            return false;
        }
        return std::find(opt.excludes.begin(), opt.excludes.end(), name) != opt.excludes.end();
    }

    bool shouldDescend(const fs::path& p) const {
        if (!opt.oneFileSystem) {
            return true;
        }
        if (isReparsePoint(p)) {
            return false;
        }
        const FileIdentity id = identityOf(p);
        return !id.valid || id.volume == rootVolume;
    }

    bool walk(const fs::path& src, const fs::path& dst, int depth) {
        std::error_code ec;
        fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            res.error = "no se pudo listar " + src.string() + ": " + ec.message();
            return false;
        }
        for (const fs::directory_entry& entry : it) {
            const fs::path from = entry.path();
            const std::string name = from.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            const fs::path to = dst / from.filename();
            const fs::file_status st = fs::symlink_status(from, ec);
            if (ec) {
                continue;
            }
            if (fs::is_symlink(st)) {
                const fs::path target = fs::read_symlink(from, ec);
                if (!ec) {
                    if (opt.dryRun) {
                        ++res.symlinksCopied;
                        continue;
                    }
                    fs::remove(to, ec);
                    fs::create_symlink(target, to, ec);
                    if (!ec) {
                        ++res.symlinksCopied;
                    }
                }
                continue;
            }
            if (fs::is_directory(st)) {
                if (!shouldDescend(from)) {
                    continue;
                }
                if (!opt.dryRun) {
                    fs::create_directories(to, ec);
                    if (ec) {
                        res.error = "no se pudo crear " + to.string() + ": " + ec.message();
                        return false;
                    }
                }
                ++res.dirsCreated;
                if (!walk(from, to, depth + 1)) {
                    return false;
                }
                if (!opt.dryRun) {
                    // Las marcas de tiempo del directorio, DESPUÉS de llenarlo: crear
                    // ficheros dentro las vuelve a tocar.
                    copyMetadata(from, to, true);
                }
                continue;
            }
            if (!fs::is_regular_file(st)) {
                // Dispositivos, sockets y FIFOs no se copian. rsync tampoco lo hace sin
                // pedírselo, y en un dataset de datos no deberían estar.
                continue;
            }

            const FileIdentity id = identityOf(from);
            if (id.valid && id.linkCount > 1) {
                const auto key = std::make_pair(id.volume, id.index);
                const auto seen = hardLinks.find(key);
                if (seen != hardLinks.end()) {
                    fs::remove(to, ec);
                    if (createHardLinkAt(seen->second, to)) {
                        ++res.hardLinksRecreated;
                        continue;
                    }
                    // Si el enlace falla —otro volumen, permisos— se copia y no se pierde
                    // nada salvo el ahorro de espacio.
                }
            }

            if (opt.skipUnchanged && sameAlreadyThere(from, to)) {
                ++res.filesSkipped;
                // Se apunta igualmente para los enlaces duros: si otra ruta apunta al
                // mismo fichero, tiene que enlazar contra este, no copiarse aparte.
                if (id.valid && id.linkCount > 1) {
                    hardLinks.emplace(std::make_pair(id.volume, id.index), to);
                }
                continue;
            }
            if (opt.dryRun) {
                ++res.filesCopied;
                continue;
            }
            std::string err;
            if (!copyFileData(from, to, res.bytesWritten, err)) {
                res.error = err;
                return false;
            }
            copyMetadata(from, to, false);
            ++res.filesCopied;
            if (id.valid && id.linkCount > 1) {
                hardLinks.emplace(std::make_pair(id.volume, id.index), to);
            }
        }
        if (opt.deleteExtraneous && !removeExtraneous(src, dst, depth)) {
            return false;
        }
        return true;
    }

    // Lo que hay en el destino y no en el origen. Es el `--delete` de rsync, y se hace
    // DESPUÉS de copiar: si se hiciera antes, un fallo a mitad dejaría el destino con
    // menos de lo que tenía y sin lo nuevo.
    //
    // Lo excluido no se toca, igual que en rsync: dejarlo fuera de la copia a propósito
    // y que el borrado se lo llevara sería lo peor de los dos mundos.
    bool removeExtraneous(const fs::path& src, const fs::path& dst, int depth) {
        std::error_code ec;
        fs::directory_iterator it(dst, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            return true;  // el destino puede no existir aún; no es un fallo
        }
        for (const fs::directory_entry& entry : it) {
            const fs::path victim = entry.path();
            const std::string name = victim.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            if (fs::exists(fs::symlink_status(src / victim.filename(), ec))) {
                continue;
            }
            ++res.entriesDeleted;
            if (opt.dryRun) {
                continue;
            }
            std::error_code rmec;
            fs::remove_all(victim, rmec);
            if (rmec) {
                res.error = "no se pudo borrar " + victim.string() + ": " + rmec.message();
                return false;
            }
        }
        return true;
    }
};

struct Counter {
    Options opt;
    std::uint64_t rootVolume = 0;
    long long pending = 0;

    bool excluded(const std::string& name, int depth) const {
        if (depth != 0) {
            return false;
        }
        return std::find(opt.excludes.begin(), opt.excludes.end(), name) != opt.excludes.end();
    }

    bool shouldDescend(const fs::path& p) const {
        if (!opt.oneFileSystem) {
            return true;
        }
        if (isReparsePoint(p)) {
            return false;
        }
        const FileIdentity id = identityOf(p);
        return !id.valid || id.volume == rootVolume;
    }

    bool walk(const fs::path& src, const fs::path& dst, int depth) {
        std::error_code ec;
        fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            return false;
        }
        for (const fs::directory_entry& entry : it) {
            const fs::path from = entry.path();
            const std::string name = from.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            const fs::path to = dst / from.filename();
            const fs::file_status st = fs::symlink_status(from, ec);
            if (ec) {
                continue;
            }
            if (fs::is_directory(st) && !fs::is_symlink(st)) {
                if (!shouldDescend(from)) {
                    continue;
                }
                if (!fs::exists(fs::symlink_status(to, ec))) {
                    // Un directorio que falta cuenta como pendiente Y hay que seguir
                    // contando lo que lleva dentro: si no, un subárbol entero ausente
                    // contaría como un solo pendiente.
                    ++pending;
                }
                if (!walk(from, to, depth + 1)) {
                    return false;
                }
                continue;
            }
            if (!fs::exists(fs::symlink_status(to, ec))) {
                ++pending;
            }
        }
        return true;
    }
};

}  // namespace

Result copyTree(const std::string& srcDir, const std::string& dstDir, const Options& opt) {
    Result r;
    std::error_code ec;
    const fs::path src(srcDir);
    const fs::path dst(dstDir);
    if (!fs::is_directory(src, ec)) {
        r.error = "el origen no es un directorio: " + srcDir;
        return r;
    }
    fs::create_directories(dst, ec);
    if (ec && !fs::is_directory(dst)) {
        r.error = "no se pudo crear el destino " + dstDir + ": " + ec.message();
        return r;
    }
    Walker w;
    w.opt = opt;
    const FileIdentity rootId = identityOf(src);
    w.rootVolume = rootId.volume;
    if (!w.walk(src, dst, 0)) {
        w.res.ok = false;
        return w.res;
    }
    w.res.ok = true;
    return w.res;
}

long long countPending(const std::string& srcDir, const std::string& dstDir,
                       const Options& opt) {
    std::error_code ec;
    const fs::path src(srcDir);
    if (!fs::is_directory(src, ec)) {
        return -1;
    }
    Counter c;
    c.opt = opt;
    const FileIdentity rootId = identityOf(src);
    c.rootVolume = rootId.volume;
    if (!c.walk(src, fs::path(dstDir), 0)) {
        return -1;
    }
    return c.pending;
}

}  // namespace zfsmgr::copytree
