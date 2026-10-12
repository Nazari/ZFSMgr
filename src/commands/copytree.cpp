#include "copytree.h"

#include "fsmeta.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
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

// Qué es una entrada. Lo que no es ni directorio, ni fichero, ni enlace simbólico, en POSIX
// es un fichero ESPECIAL —FIFO, dispositivo de caracteres o de bloque, socket— y se recrea
// como hace `rsync -a` (su -D); en Windows no existen, y lo que no se reconoce se salta.
enum class Kind { Missing, Dir, File, Symlink, Special, Other };

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



// En Windows no hay modo, dueño numérico ni dispositivos: la comparación de metadatos es la
// fecha, y la ACL se copia al copiar el contenido o el directorio.
Kind kindOf(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return Kind::Missing;
    }
    if (fs::is_symlink(st)) {
        return Kind::Symlink;
    }
    if (fs::is_directory(st)) {
        return Kind::Dir;
    }
    if (fs::is_regular_file(st)) {
        return Kind::File;
    }
    return Kind::Other;
}

bool metadataDiffers(const fs::path& src, const fs::path& dst, Kind k) {
    if (k == Kind::Symlink) {
        return false;
    }
    std::error_code e1;
    std::error_code e2;
    const auto a = fs::last_write_time(src, e1);
    const auto b = fs::last_write_time(dst, e2);
    return e1 || e2 || a != b;
}

void applyMetadata(const fs::path& src, const fs::path& dst, Kind k) {
    if (k != Kind::Symlink) {
        copyMetadata(src, dst, k == Kind::Dir);
    }
}

// La ACL de un directorio de Windows no se compara —leerla y compararla costaría lo mismo
// que reponerla—: se repone siempre, como antes, aunque en la lista de cambios solo salga
// cuando cambia la fecha.
constexpr bool kReapplyDirMetadata = true;

bool sameSpecial(const fs::path&, const fs::path&) {
    return false;
}

bool createSpecialLike(const fs::path&, const fs::path& dst, std::string& err) {
    err = "special files do not exist on Windows: " + dst.string();
    return false;
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

bool createHardLinkAt(const fs::path& existing, const fs::path& link) {
    // `linkat` sin AT_SYMLINK_FOLLOW enlaza el propio enlace simbólico, no a lo que apunta:
    // un enlace duro a un enlace simbólico se recrea como tal, igual que con `rsync -H`.
    return ::linkat(AT_FDCWD, existing.c_str(), AT_FDCWD, link.c_str(), 0) == 0;
}

void markSparse(const fs::path&) {}

constexpr bool kReapplyDirMetadata = false;

Kind kindOfMode(mode_t m) {
    if (S_ISDIR(m)) {
        return Kind::Dir;
    }
    if (S_ISREG(m)) {
        return Kind::File;
    }
    if (S_ISLNK(m)) {
        return Kind::Symlink;
    }
    if (S_ISFIFO(m) || S_ISCHR(m) || S_ISBLK(m) || S_ISSOCK(m)) {
        return Kind::Special;
    }
    return Kind::Other;
}

Kind kindOf(const fs::path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return Kind::Missing;
    }
    return kindOfMode(st.st_mode);
}

// Los atributos extendidos y las ACL, con `fsmeta`: la MISMA lectura y escritura que usa la
// copia entre máquinas, para que las dos conserven exactamente lo mismo. Se SINCRONIZAN, no
// solo se añaden: un atributo quitado en el origen se quita en el destino, como `rsync -X`.
// Antes solo se añadían, y una ACL retirada en el origen seguía dando permiso en la copia.

struct timespec mtimeOf(const struct stat& st) {
    // Los nombres de los campos de tiempo NO son los mismos en todas partes: POSIX 2008
    // dice st_mtim, y macOS conserva los de BSD, st_mtimespec.
#if defined(__APPLE__)
    return st.st_mtimespec;
#else
    return st.st_mtim;
#endif
}

struct timespec atimeOf(const struct stat& st) {
#if defined(__APPLE__)
    return st.st_atimespec;
#else
    return st.st_atim;
#endif
}

// ¿Hay que reponer dueño, modo, fecha o atributos? Es la otra mitad de lo que hace rsync con
// lo que no ha cambiado de contenido: un `chmod` en el origen tiene que llegar aunque el
// fichero sea el mismo. Antes solo se copiaban los metadatos al copiar los datos.
bool metadataDiffers(const fs::path& src, const fs::path& dst, Kind k) {
    struct stat a {};
    struct stat b {};
    if (::lstat(src.c_str(), &a) != 0 || ::lstat(dst.c_str(), &b) != 0) {
        return true;
    }
    if (a.st_uid != b.st_uid || a.st_gid != b.st_gid) {
        return true;
    }
    if (k != Kind::Symlink && (a.st_mode & 07777) != (b.st_mode & 07777)) {
        return true;
    }
    const struct timespec ta = mtimeOf(a);
    const struct timespec tb = mtimeOf(b);
    if (ta.tv_sec != tb.tv_sec || ta.tv_nsec != tb.tv_nsec) {
        return true;
    }
    return fsmeta::read(src.string()) != fsmeta::read(dst.string());
}

// El orden importa:
//   - el dueño primero: en Linux, cambiar el dueño de un fichero con setuid borra esos bits;
//   - los atributos antes que el modo: reponer la ACL POSIX de Linux reescribe los bits de
//     grupo (la máscara), y el `chmod` posterior los deja como en el origen;
//   - la ACL de FreeBSD y macOS después del modo, al revés: ahí es el `chmod` el que la
//     reescribiría;
//   - la fecha la última: todo lo anterior la toca.
// Todo sin seguir enlaces simbólicos: el dueño y la fecha de un enlace son suyos.
void applyMetadata(const fs::path& src, const fs::path& dst, Kind k) {
    struct stat st {};
    if (::lstat(src.c_str(), &st) != 0) {
        return;
    }
    if (::lchown(dst.c_str(), st.st_uid, st.st_gid) != 0) {
        // Sin root solo se puede regalar a uno mismo; lo demás sale bien igual.
    }
    const fsmeta::Metadata meta = fsmeta::read(src.string());
    (void)fsmeta::syncXattrs(dst.string(), meta.xattrs);
    if (k != Kind::Symlink) {
        (void)::chmod(dst.c_str(), st.st_mode & 07777);
        (void)fsmeta::setAcl(dst.string(), meta.acl);
    }
#if defined(__FreeBSD__) || defined(__APPLE__)
    else {
        // El modo propio del enlace, donde existe (en Linux siempre es 0777).
        (void)::fchmodat(AT_FDCWD, dst.c_str(), st.st_mode & 07777, AT_SYMLINK_NOFOLLOW);
    }
#endif
    struct timespec times[2] = {atimeOf(st), mtimeOf(st)};
    (void)::utimensat(AT_FDCWD, dst.c_str(), times, AT_SYMLINK_NOFOLLOW);
}

void copyMetadata(const fs::path& src, const fs::path& dst, bool isDir) {
    applyMetadata(src, dst, isDir ? Kind::Dir : Kind::File);
}

bool sameSpecial(const fs::path& src, const fs::path& dst) {
    struct stat a {};
    struct stat b {};
    return ::lstat(src.c_str(), &a) == 0 && ::lstat(dst.c_str(), &b) == 0
           && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT)
           && (!(S_ISCHR(a.st_mode) || S_ISBLK(a.st_mode)) || a.st_rdev == b.st_rdev);
}

// Un fichero especial como el del origen: FIFO, dispositivo o socket. Lo que hace `rsync -a`
// con su -D (--devices --specials). Un socket no lleva datos —quien lo escuchaba no viaja—;
// se recrea el nodo, que es lo que rsync hace.
bool createSpecialLike(const fs::path& src, const fs::path& dst, std::string& err) {
    struct stat st {};
    if (::lstat(src.c_str(), &st) != 0) {
        err = "could not read " + src.string() + ": " + std::strerror(errno);
        return false;
    }
    int rc = 0;
    if (S_ISFIFO(st.st_mode)) {
        rc = ::mkfifo(dst.c_str(), st.st_mode & 07777);
    } else {
        rc = ::mknod(dst.c_str(), st.st_mode & (S_IFMT | 07777), st.st_rdev);
        if (rc != 0 && S_ISSOCK(st.st_mode)) {
            // macOS y FreeBSD no crean sockets con mknod: hay que atarlo, como hace rsync.
            struct sockaddr_un addr {};
            const std::string path = dst.string();
            if (path.size() < sizeof(addr.sun_path)) {
                const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd >= 0) {
                    addr.sun_family = AF_UNIX;
                    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                    rc = ::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
                    ::close(fd);
                }
            }
        }
    }
    if (rc != 0) {
        err = "could not create the special file " + dst.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

#endif

// ── Contenido ───────────────────────────────────────────────────────────────────────────

// Bloque con el que se buscan ceros. Antes se miraba el búfer de lectura entero (256 KiB): un
// fichero con 4 KiB de datos al final de un hueco materializaba 252 KiB de ceros. Se vio
// midiendo contra rsync: 128 KiB de más en una imagen dispersa de 100 MiB.
constexpr std::size_t kZeroBlock = 4096;

bool isZero(const char* p, std::size_t n) {
    static const char zeros[kZeroBlock] = {};
    return std::memcmp(p, zeros, n) == 0;
}

// Un nombre de temporal junto al destino. Corto a propósito: el destino puede llamarse con
// 255 bytes, y anteponerle nada pasaría del límite del nombre.
fs::path tempNameNextTo(const fs::path& dst) {
    static std::atomic<unsigned> counter{0};
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return dst.parent_path()
           / (".zfsmgr-" + std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1)) + ".tmp");
}

// Copia el contenido de un fichero conservando los huecos, en un TEMPORAL que luego ocupa el
// sitio del destino con un rename.
//
// No se usa std::filesystem::copy_file: escribiría los ceros de un fichero disperso como
// datos reales, y una imagen de disco de 100 GB con 2 GB usados llegaría ocupando 100. Aquí,
// un bloque de ceros no se escribe: se salta, y el sistema de ficheros deja el hueco. En
// POSIX además los huecos del ORIGEN ni se leen (SEEK_DATA/SEEK_HOLE).
//
// **Por qué un temporal**, como rsync: escribir encima truncaba el fichero que ya estaba, y si
// en el destino tenía otros enlaces duros los cambiaba a todos —un fichero que en el origen
// ya no estaba enlazado arrastraba a su antiguo compañero—. Y un corte a mitad dejaba el
// fichero bueno destruido y el nuevo a medias.
#ifdef _WIN32
bool writeContents(const fs::path& src, const fs::path& tmp, std::uint64_t& bytesOut,
                   std::string& err, std::atomic<std::uint64_t>* progreso) {
    std::FILE* in = _wfopen(src.wstring().c_str(), L"rb");
    if (!in) {
        err = "could not read " + src.string();
        return false;
    }
    std::FILE* out = _wfopen(tmp.wstring().c_str(), L"wb");
    if (!out) {
        std::fclose(in);
        err = "could not create " + tmp.string();
        return false;
    }
    // Se marca disperso con el fichero ya creado y antes de escribir nada.
    std::fclose(out);
    markSparse(tmp);
    out = _wfopen(tmp.wstring().c_str(), L"r+b");
    if (!out) {
        std::fclose(in);
        err = "could not open for writing " + tmp.string();
        return false;
    }
    constexpr std::size_t kBuf = 256 * 1024;
    std::vector<char> buf(kBuf);
    bool ok = true;
    std::uint64_t pos = 0;
    std::uint64_t written = 0;
    while (ok) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) {
            break;
        }
        if (progreso) {
            progreso->fetch_add(n, std::memory_order_relaxed);
        }
        for (std::size_t off = 0; off < n && ok; off += kZeroBlock) {
            const std::size_t len = std::min(kZeroBlock, n - off);
            if (!isZero(buf.data() + off, len)) {
                if (_fseeki64(out, static_cast<long long>(pos + off), SEEK_SET) != 0
                    || std::fwrite(buf.data() + off, 1, len, out) != len) {
                    ok = false;
                    err = "could not write to " + tmp.string();
                }
                written += len;
            }
        }
        pos += n;
    }
    std::fclose(in);
    std::fclose(out);
    if (!ok) {
        return false;
    }
    // Un fichero que TERMINA en ceros: saltarlos no lo alarga, así que se fija el tamaño.
    std::error_code rec;
    fs::resize_file(tmp, pos, rec);
    if (rec) {
        err = "could not set the size of " + tmp.string() + ": " + rec.message();
        return false;
    }
    bytesOut += written;
    return true;
}
#else
bool writeContents(const fs::path& src, const fs::path& tmp, std::uint64_t& bytesOut,
                   std::string& err, std::atomic<std::uint64_t>* progreso) {
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        err = "could not read " + src.string() + ": " + std::strerror(errno);
        return false;
    }
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        err = "could not create " + tmp.string() + ": " + std::strerror(errno);
        ::close(in);
        return false;
    }
    struct stat st {};
    (void)::fstat(in, &st);
    const off_t size = st.st_size;
    constexpr std::size_t kBuf = 256 * 1024;
    std::vector<char> buf(kBuf);
    bool ok = true;
    std::uint64_t written = 0;
    off_t pos = 0;
    while (ok && pos < size) {
        // El tramo de datos que empieza en `pos` o después. Sin soporte —o en un sistema
        // que no lo implementa—, todo el resto del fichero es un tramo.
        off_t dataStart = pos;
        off_t dataEnd = size;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
        const off_t d = ::lseek(in, pos, SEEK_DATA);
        if (d < 0 && errno == ENXIO) {
            dataStart = size;  // de aquí al final, hueco
        } else if (d >= 0) {
            dataStart = d;
            const off_t h = ::lseek(in, d, SEEK_HOLE);
            dataEnd = h < 0 ? size : std::min(h, size);
        }
#endif
        if (progreso && dataStart > pos) {
            // Los huecos cuentan como recorridos: el avance es por el tamaño del fichero.
            progreso->fetch_add(static_cast<std::uint64_t>(dataStart - pos),
                                std::memory_order_relaxed);
        }
        off_t at = dataStart;
        while (ok && at < dataEnd) {
            const std::size_t want =
                static_cast<std::size_t>(std::min<off_t>(static_cast<off_t>(kBuf), dataEnd - at));
            const ssize_t n = ::pread(in, buf.data(), want, at);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                err = "could not read " + src.string() + ": " + std::strerror(errno);
                break;
            }
            if (n == 0) {
                break;  // el fichero encogió mientras se copiaba
            }
            if (progreso) {
                progreso->fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
            }
            for (std::size_t off = 0; off < static_cast<std::size_t>(n) && ok; off += kZeroBlock) {
                const std::size_t len = std::min(kZeroBlock, static_cast<std::size_t>(n) - off);
                if (isZero(buf.data() + off, len)) {
                    continue;
                }
                std::size_t done = 0;
                while (done < len) {
                    const ssize_t w = ::pwrite(out, buf.data() + off + done, len - done,
                                               at + static_cast<off_t>(off + done));
                    if (w < 0 && errno == EINTR) {
                        continue;
                    }
                    if (w <= 0) {
                        ok = false;
                        err = "could not write to " + tmp.string() + ": " + std::strerror(errno);
                        break;
                    }
                    done += static_cast<std::size_t>(w);
                }
                written += len;
            }
            at += n;
        }
        pos = dataEnd;
    }
    // El tamaño final, siempre: un fichero que TERMINA en hueco no se alarga solo. Con
    // truncate y no escribiendo un byte al final: ese byte materializaba un bloque, y un
    // fichero enteramente disperso acababa ocupando 4 KiB donde rsync deja 0.
    if (ok && ::ftruncate(out, size) != 0) {
        ok = false;
        err = "could not set the size of " + tmp.string() + ": " + std::strerror(errno);
    }
    ::close(in);
    if (::close(out) != 0 && ok) {
        ok = false;
        err = "could not write to " + tmp.string() + ": " + std::strerror(errno);
    }
    if (ok) {
        bytesOut += written;
    }
    return ok;
}
#endif

// El contenido a un temporal, sus metadatos, y el rename sobre el destino. Los metadatos van
// ANTES del rename para que el fichero no aparezca nunca en su sitio con dueño o modo
// equivocados.
bool copyFileData(const fs::path& src, const fs::path& dst, std::uint64_t& bytesOut,
                  std::string& err, std::atomic<std::uint64_t>* progreso = nullptr) {
    const fs::path tmp = tempNameNextTo(dst);
    if (!writeContents(src, tmp, bytesOut, err, progreso)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    copyMetadata(src, tmp, false);
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) {
        err = "could not put " + dst.string() + " in place: " + ec.message();
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

// ¿El destino ya tiene este fichero, igual? Tamaño y fecha, que es el criterio de rsync por
// omisión —salvo que aquí la fecha se compara al NANOSEGUNDO y rsync al segundo: un cambio
// dentro del mismo segundo que conserva el tamaño, rsync no lo ve—. No compara contenido:
// comprobarlo obligaría a leerlo todo en cada pasada y entonces saltárselo no ahorraría nada.
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

bool sameFile(const fs::path& a, const fs::path& b) {
    const FileIdentity x = identityOf(a);
    const FileIdentity y = identityOf(b);
    return x.valid && y.valid && x.volume == y.volume && x.index == y.index;
}

bool dirIsEmpty(const fs::path& p) {
    std::error_code ec;
    return fs::is_empty(p, ec) && !ec;
}

std::string joinRel(const std::string& rel, const std::string& name) {
    return rel.empty() ? name : rel + "/" + name;
}

struct Walker {
    Options opt;
    Result res;
    std::uint64_t rootVolume = 0;
    // Identidad → primera ruta ya copiada en el destino. Solo entran las entradas con más
    // de un enlace, así que la memoria es proporcional a esas y no al total.
    std::map<std::pair<std::uint64_t, std::uint64_t>, fs::path> hardLinks;

    void item(Change c, const std::string& rel) {
        if (opt.itemize) {
            opt.itemize(c, rel);
        }
    }

    // Un fallo NO corta la pasada, como en rsync: se anota y se sigue con lo demás. El
    // resultado sale con ok=false y todos los motivos. Antes el primer error abortaba, y un
    // directorio que había pasado a fichero dejaba sin hacer todo lo que venía detrás.
    void fail(const std::string& why) {
        res.errors.push_back(why);
    }

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

    void walk(const fs::path& src, const fs::path& dst, const std::string& rel, int depth) {
        std::error_code ec;
        // SIN skip_permission_denied: un directorio del origen que no se puede leer salía
        // vacío, y con borrado eso vaciaba su copia. Ahora es un error y ahí no se borra nada.
        fs::directory_iterator it(src, ec);
        if (ec) {
            fail("could not list " + src.string() + ": " + ec.message());
            return;
        }
        bool listedAll = true;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            const fs::path from = it->path();
            const std::string name = from.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            one(from, dst / from.filename(), joinRel(rel, name), depth);
        }
        if (ec) {
            fail("could not list " + src.string() + ": " + ec.message());
            listedAll = false;
        }
        if (opt.deleteExtraneous && listedAll) {
            removeExtraneous(src, dst, rel, depth);
        }
    }

    void one(const fs::path& from, const fs::path& to, const std::string& rel, int depth) {
        const Kind k = kindOf(from);
        if (k == Kind::Missing || k == Kind::Other) {
            return;  // desaparecido mientras se recorría, o algo que aquí no se sabe copiar
        }
        if (k == Kind::Dir && !shouldDescend(from)) {
            return;
        }
        Kind dk = kindOf(to);
        std::error_code ec;

        // Otro TIPO en el destino —un directorio donde ahora hay un fichero, un fichero
        // donde ahora hay un enlace—, o un especial de otra clase: se quita lo que hay. Un
        // directorio con contenido solo con borrado, como rsync sin --force: sin él, la
        // orden no pidió borrar nada del destino.
        bool replacing = false;
        if (dk != Kind::Missing
            && (dk != k || (k == Kind::Special && !sameSpecial(from, to)))) {
            if (dk == Kind::Dir && !opt.deleteExtraneous && !dirIsEmpty(to)) {
                fail("not replacing the non-empty directory " + to.string()
                     + " without deletion enabled");
                return;
            }
            if (!opt.dryRun) {
                fs::remove_all(to, ec);
                if (ec) {
                    fail("could not remove " + to.string() + ": " + ec.message());
                    return;
                }
            }
            dk = Kind::Missing;
            replacing = true;
        }
        const Change created = replacing ? Change::Replaced : Change::Created;

        // Enlaces duros, de cualquier cosa que no sea un directorio.
        const FileIdentity id = k == Kind::Dir ? FileIdentity{} : identityOf(from);
        const bool linked = id.valid && id.linkCount > 1;
        const auto key = std::make_pair(id.volume, id.index);
        if (linked) {
            const auto seen = hardLinks.find(key);
            if (seen != hardLinks.end()) {
                if (dk != Kind::Missing && sameFile(seen->second, to)) {
                    return;  // ya es ese enlace; los metadatos son del inodo, ya puestos
                }
                item(dk == Kind::Missing ? created : Change::Content, rel);
                ++res.hardLinksRecreated;
                if (opt.dryRun) {
                    return;
                }
                if (dk != Kind::Missing) {
                    fs::remove(to, ec);
                }
                if (createHardLinkAt(seen->second, to)) {
                    return;
                }
                // Si el enlace falla —otro volumen, permisos— se copia y no se pierde nada
                // salvo el ahorro de espacio.
                --res.hardLinksRecreated;
                dk = Kind::Missing;
            }
        }

        switch (k) {
            case Kind::Dir: {
                if (dk == Kind::Missing) {
                    item(created, rel + "/");
                    ++res.dirsCreated;
                    if (!opt.dryRun) {
                        fs::create_directory(to, ec);
                        if (ec) {
                            fail("could not create " + to.string() + ": " + ec.message());
                            return;
                        }
                    }
                }
                walk(from, to, rel, depth + 1);
                // Los metadatos del directorio, DESPUÉS de llenarlo: crear dentro cambia su
                // fecha.
                finishMetadata(from, to, k, dk, rel + "/");
                return;
            }
            case Kind::File: {
                if (dk == Kind::File && opt.skipUnchanged && sameAlreadyThere(from, to)) {
                    ++res.filesSkipped;
                    finishMetadata(from, to, k, dk, rel);
                    break;
                }
                item(dk == Kind::Missing ? created : Change::Content, rel);
                ++res.filesCopied;
                if (opt.dryRun) {
                    break;
                }
                std::string err;
                if (!copyFileData(from, to, res.bytesWritten, err, opt.progress)) {
                    fail(err);
                    return;
                }
                break;
            }
            case Kind::Symlink: {
                const fs::path target = fs::read_symlink(from, ec);
                if (ec) {
                    fail("could not read the link " + from.string() + ": " + ec.message());
                    return;
                }
                std::error_code ec2;
                if (dk == Kind::Symlink && fs::read_symlink(to, ec2) == target && !ec2) {
                    finishMetadata(from, to, k, dk, rel);
                    break;
                }
                item(dk == Kind::Missing ? created : Change::Content, rel);
                ++res.symlinksCopied;
                if (opt.dryRun) {
                    break;
                }
                if (dk != Kind::Missing) {
                    fs::remove(to, ec);
                }
                fs::create_symlink(target, to, ec);
                if (ec) {
                    fail("could not create the link " + to.string() + ": " + ec.message());
                    return;
                }
                applyMetadata(from, to, k);
                break;
            }
            case Kind::Special: {
                if (dk == Kind::Special) {
                    finishMetadata(from, to, k, dk, rel);
                    break;
                }
                item(created, rel);
                ++res.specialsCreated;
                if (opt.dryRun) {
                    break;
                }
                std::string err;
                if (!createSpecialLike(from, to, err)) {
                    fail(err);
                    return;
                }
                applyMetadata(from, to, k);
                break;
            }
            default:
                return;
        }
        if (linked) {
            hardLinks.emplace(key, to);
        }
    }

    // Lo que ya estaba y no cambia de contenido: solo se repone lo que difiera.
    void finishMetadata(const fs::path& from, const fs::path& to, Kind k, Kind before,
                        const std::string& rel) {
        if (opt.dryRun) {
            if (before != Kind::Missing && metadataDiffers(from, to, k)) {
                item(Change::Metadata, rel);
            }
            return;
        }
        if (metadataDiffers(from, to, k)) {
            if (before != Kind::Missing) {
                item(Change::Metadata, rel);
            }
            applyMetadata(from, to, k);
        } else if (kReapplyDirMetadata && k == Kind::Dir) {
            applyMetadata(from, to, k);
        }
    }

    // Lo que hay en el destino y no en el origen. Es el `--delete` de rsync, y se hace
    // DESPUÉS de copiar el directorio: si se hiciera antes, un fallo a mitad dejaría el
    // destino con menos de lo que tenía y sin lo nuevo.
    //
    // Lo excluido no se toca, igual que en rsync: dejarlo fuera de la copia a propósito
    // y que el borrado se lo llevara sería lo peor de los dos mundos.
    void removeExtraneous(const fs::path& src, const fs::path& dst, const std::string& rel,
                          int depth) {
        std::error_code ec;
        fs::directory_iterator it(dst, ec);
        if (ec) {
            return;  // el destino puede no existir aún (en seco, seguro); no es un fallo
        }
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                fail("could not list " + dst.string() + ": " + ec.message());
                return;
            }
            const fs::path victim = it->path();
            const std::string name = victim.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            if (kindOf(src / victim.filename()) != Kind::Missing) {
                continue;
            }
            const bool isDir = kindOf(victim) == Kind::Dir;
            item(Change::Deleted, joinRel(rel, name) + (isDir ? "/" : ""));
            ++res.entriesDeleted;
            if (opt.dryRun) {
                continue;
            }
            std::error_code rmec;
            fs::remove_all(victim, rmec);
            if (rmec) {
                fail("could not delete " + victim.string() + ": " + rmec.message());
            }
        }
    }

    Result finish() {
        res.ok = res.errors.empty();
        if (!res.ok) {
            res.error = res.errors.front();
            if (res.errors.size() > 1) {
                res.error += " (and " + std::to_string(res.errors.size() - 1) + " more)";
            }
        }
        return res;
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
        fs::directory_iterator it(src, ec);
        if (ec) {
            return false;
        }
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                return false;
            }
            const fs::path from = it->path();
            const std::string name = from.filename().string();
            if (excluded(name, depth)) {
                continue;
            }
            const fs::path to = dst / from.filename();
            const Kind k = kindOf(from);
            if (k == Kind::Missing || k == Kind::Other) {
                continue;
            }
            if (k == Kind::Dir) {
                if (!shouldDescend(from)) {
                    continue;
                }
                if (kindOf(to) == Kind::Missing) {
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
            if (kindOf(to) == Kind::Missing) {
                ++pending;
            }
        }
        return !ec;
    }
};

}  // namespace

Result copyTree(const std::string& srcDir, const std::string& dstDir, const Options& opt) {
    Result r;
    std::error_code ec;
    const fs::path src(srcDir);
    const fs::path dst(dstDir);
    // Un FICHERO suelto como origen.
    //
    // Antes se rechazaba —«el origen no es un directorio»— porque todo lo que llamaba aquí
    // copiaba árboles: `todir`, `breakdown`, `assemble`. `cp` sí copia ficheros sueltos, y
    // es un caso que no se puede resolver por fuera envolviéndolo en un directorio: eso
    // cambiaría dónde aterriza.
    //
    // El destino es la RUTA FINAL del fichero, no su contenedor. Quien llama ya ha decidido
    // el nombre —`cp` lo hace con `--as` o con el del origen— y meterlo aquí dentro de un
    // directorio sería repartir esa decisión entre dos sitios.
    if (kindOf(src) == Kind::File) {
        if (fs::is_directory(dst, ec)) {
            r.error = "the target already has a DIRECTORY with the same name: " + dstDir;
            r.errors.push_back(r.error);
            return r;
        }
        if (!opt.dryRun) {
            fs::create_directories(dst.parent_path(), ec);
            if (ec && !fs::is_directory(dst.parent_path())) {
                r.error = "could not create " + dst.parent_path().string() + ": " + ec.message();
                r.errors.push_back(r.error);
                return r;
            }
        }
        Walker w;
        w.opt = opt;
        w.one(src, dst, src.filename().string(), 1);
        return w.finish();
    }
    if (!fs::is_directory(src, ec)) {
        r.error = "the source is neither a file nor a directory: " + srcDir;
        r.errors.push_back(r.error);
        return r;
    }
    // En SECO no se crea el destino. Parece obvio y no lo era: `create_directories` estaba
    // antes de mirar `dryRun`, así que una pasada que promete no tocar nada dejaba el
    // directorio creado.
    const Kind before = kindOf(dst);
    if (!opt.dryRun) {
        fs::create_directories(dst, ec);
        if (ec && !fs::is_directory(dst)) {
            r.error = "could not create the target " + dstDir + ": " + ec.message();
            r.errors.push_back(r.error);
            return r;
        }
    }
    Walker w;
    w.opt = opt;
    const FileIdentity rootId = identityOf(src);
    w.rootVolume = rootId.volume;
    w.walk(src, dst, std::string(), 0);
    // Y la RAÍZ, que el recorrido no visita: solo copia lo que hay dentro. Sin esto el
    // directorio de llegada se quedaba como lo crea el agente —root:root 0755— aunque su
    // contenido conservara dueño y modo. Al final, porque escribir dentro cambia su fecha.
    w.finishMetadata(src, dst, Kind::Dir, before, "./");
    return w.finish();
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
