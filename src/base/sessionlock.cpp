#include "sessionlock.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zfsmgr::base::session {
namespace {

std::string encode(const std::string& kind) {
    std::string s;
    s += "kind=" + kind + "\n";
    s += "user=" + currentUser() + "\n";
#ifdef _WIN32
    s += "pid=" + std::to_string(static_cast<long long>(GetCurrentProcessId())) + "\n";
#else
    s += "pid=" + std::to_string(static_cast<long long>(::getpid())) + "\n";
#endif
    s += "since=" + std::to_string(static_cast<long long>(std::time(nullptr))) + "\n";
    return s;
}

Holder decode(const std::string& text) {
    Holder h;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "kind") {
            h.kind = value;
        } else if (key == "user") {
            h.user = value;
        } else if (key == "pid") {
            h.pid = std::atoll(value.c_str());
        } else if (key == "since") {
            h.since = std::atoll(value.c_str());
        }
    }
    return h;
}

// Far beyond anything written: on Windows a locked range cannot be read by anyone else, and
// the file's contents must stay readable for the next one to say who holds it.
#ifdef _WIN32
constexpr DWORD kLockOffset = 0x40000000;

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string lastError() {
    return "error " + std::to_string(static_cast<unsigned long>(GetLastError()));
}

// Opened by everybody: the first one to create it gives every user full access, or the next
// user could not take the session once the first had gone.
HANDLE openLock(const std::string& path, bool& writable, std::string& detail) {
    const std::wstring w = wide(path);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;WD)", SDDL_REVISION_1,
                                                             &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, share, &sa, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sd) {
        LocalFree(sd);
    }
    writable = h != INVALID_HANDLE_VALUE;
    if (!writable) {
        detail = path + ": " + lastError();
        h = CreateFileW(w.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return h;
}

bool tryLock(HANDLE h) {
    OVERLAPPED ov{};
    ov.Offset = kLockOffset;
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0;
}

void unlock(HANDLE h) {
    OVERLAPPED ov{};
    ov.Offset = kLockOffset;
    UnlockFileEx(h, 0, 1, 0, &ov);
}

std::string readAll(HANDLE h) {
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    char buf[4096];
    DWORD n = 0;
    if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
        return {};
    }
    return std::string(buf, n);
}

bool isFree(const std::string& path) {
    bool writable = false;
    std::string ignored;
    HANDLE h = openLock(path, writable, ignored);
    if (h == INVALID_HANDLE_VALUE) {
        return true;
    }
    const bool free = tryLock(h);
    if (free) {
        unlock(h);
    }
    CloseHandle(h);
    return free;
}

Holder readHolder(const std::string& path) {
    bool writable = false;
    std::string ignored;
    HANDLE h = openLock(path, writable, ignored);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    Holder holder = decode(readAll(h));
    CloseHandle(h);
    return holder;
}
#else
// `O_NOFOLLOW` and a regular file only: it lives in /tmp, where anybody can leave a link
// waiting. And no O_CREAT on a file that already exists: with fs.protected_regular a user may
// not O_CREAT another user's file in a sticky directory, even with permission to write it.
int openLock(const std::string& path, bool& writable, std::string& detail) {
    writable = true;
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd >= 0) {
            ::fchmod(fd, 0666);  // past the umask: every user takes it in turn
        } else if (errno == EEXIST) {
            fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        }
    }
    if (fd < 0 && errno == EACCES) {
        // Left without write permission by someone: still enough to see who holds it.
        detail = path + ": " + std::strerror(errno);
        writable = false;
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) {
        detail = path + ": " + std::strerror(errno);
        return -1;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        detail = path + ": not a regular file";
        return -1;
    }
    return fd;
}

// 1 taken, 0 somebody else has it, -1 failed.
int tryLock(int fd) {
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == EWOULDBLOCK ? 0 : -1;
    }
}

std::string readAll(int fd) {
    std::string out;
    char buf[4096];
    off_t at = 0;
    for (;;) {
        const ssize_t n = ::pread(fd, buf, sizeof buf, at);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
        at += n;
        if (out.size() > 65536) {
            break;
        }
    }
    return out;
}

bool isFree(const std::string& path) {
    bool writable = false;
    std::string ignored;
    const int fd = openLock(path, writable, ignored);
    if (fd < 0) {
        return true;
    }
    const bool free = tryLock(fd) == 1;
    if (free) {
        ::flock(fd, LOCK_UN);
    }
    ::close(fd);
    return free;
}

Holder readHolder(const std::string& path) {
    bool writable = false;
    std::string ignored;
    const int fd = openLock(path, writable, ignored);
    if (fd < 0) {
        return {};
    }
    Holder h = decode(readAll(fd));
    ::close(fd);
    return h;
}
#endif

bool waitFree(const std::string& path, int ms) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        if (isFree(path)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= limit) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

std::string lockPath() {
    if (const char* p = std::getenv("ZFSMGR_SESSION_LOCK"); p && *p) {
        return p;
    }
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    const std::string base =
        (n > 0 && n < MAX_PATH) ? narrow(std::wstring(buf, n)) : std::string("C:\\ProgramData");
    return base + "\\zfsmgr-session.lock";
#else
    return "/tmp/zfsmgr-session.lock";
#endif
}

std::string currentUser() {
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (GetUserNameW(buf, &n) && n > 0) {
        return narrow(std::wstring(buf, n - 1));
    }
    return "?";
#else
    if (const passwd* pw = ::getpwuid(::geteuid()); pw && pw->pw_name) {
        return pw->pw_name;
    }
    return std::to_string(static_cast<long long>(::geteuid()));
#endif
}

std::string hostName() {
    std::string name;
#ifdef _WIN32
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n) && n > 0) {
        name = narrow(std::wstring(buf, n));
    }
#else
    char buf[256] = {};
    if (::gethostname(buf, sizeof buf - 1) == 0) {
        name = buf;
    }
#endif
    if (const std::size_t dot = name.find('.'); dot != std::string::npos) {
        name.resize(dot);
    }
    return name.empty() ? std::string("?") : name;
}

Lock::~Lock() {
    release();
}

bool Lock::held() const {
#ifdef _WIN32
    return m_handle != nullptr;
#else
    return m_fd >= 0;
#endif
}

Acquire Lock::acquire(const std::string& kind, Holder& holder, std::string& detail) {
    release();
    const std::string path = lockPath();
    bool writable = false;
#ifdef _WIN32
    HANDLE h = openLock(path, writable, detail);
    if (h == INVALID_HANDLE_VALUE) {
        return Acquire::Failed;
    }
    if (!tryLock(h)) {
        if (GetLastError() != ERROR_LOCK_VIOLATION && GetLastError() != ERROR_IO_PENDING) {
            detail = path + ": " + lastError();
            CloseHandle(h);
            return Acquire::Failed;
        }
        holder = decode(readAll(h));
        CloseHandle(h);
        return Acquire::Busy;
    }
    if (!writable) {
        unlock(h);
        CloseHandle(h);
        return Acquire::Failed;
    }
    const std::string info = encode(kind);
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    DWORD n = 0;
    WriteFile(h, info.data(), static_cast<DWORD>(info.size()), &n, nullptr);
    SetEndOfFile(h);
    FlushFileBuffers(h);
    m_handle = h;
#else
    const int fd = openLock(path, writable, detail);
    if (fd < 0) {
        return Acquire::Failed;
    }
    const int r = tryLock(fd);
    if (r == 0) {
        holder = decode(readAll(fd));
        ::close(fd);
        return Acquire::Busy;
    }
    if (r < 0) {
        detail = path + ": " + std::strerror(errno);
        ::close(fd);
        return Acquire::Failed;
    }
    if (!writable) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return Acquire::Failed;
    }
    const std::string info = encode(kind);
    if (::ftruncate(fd, 0) != 0
        || ::pwrite(fd, info.data(), info.size(), 0) != static_cast<ssize_t>(info.size())) {
        // Held all the same: only the «who» is missing for the next one.
        detail = path + ": " + std::strerror(errno);
    }
    m_fd = fd;
#endif
    return Acquire::Held;
}

void Lock::release() {
#ifdef _WIN32
    if (m_handle) {
        HANDLE h = static_cast<HANDLE>(m_handle);
        unlock(h);
        CloseHandle(h);
        m_handle = nullptr;
    }
#else
    if (m_fd >= 0) {
        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}

Stop stop(const Holder& holder, std::string& detail) {
    const std::string path = lockPath();
    if (isFree(path)) {
        return Stop::Stopped;
    }
    // Still the same one: a PID is only trusted while its owner keeps the lock it wrote.
    const Holder now = readHolder(path);
    if (holder.pid <= 0 || now.pid != holder.pid || now.since != holder.since) {
        detail = "the session changed hands meanwhile";
        return Stop::Failed;
    }
#ifdef _WIN32
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(holder.pid));
    if (!p) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            return Stop::NotAllowed;
        }
        detail = lastError();
        return waitFree(path, 2000) ? Stop::Stopped : Stop::Failed;
    }
    if (!TerminateProcess(p, 1)) {
        const DWORD e = GetLastError();
        CloseHandle(p);
        if (e == ERROR_ACCESS_DENIED) {
            return Stop::NotAllowed;
        }
        detail = "error " + std::to_string(static_cast<unsigned long>(e));
        return Stop::Failed;
    }
    WaitForSingleObject(p, 5000);
    CloseHandle(p);
    return waitFree(path, 3000) ? Stop::Stopped : Stop::StillRunning;
#else
    const pid_t pid = static_cast<pid_t>(holder.pid);
    if (::kill(pid, SIGTERM) != 0) {
        if (errno == EPERM) {
            return Stop::NotAllowed;
        }
        if (errno != ESRCH) {
            detail = std::strerror(errno);
            return Stop::Failed;
        }
    }
    if (waitFree(path, 5000)) {
        return Stop::Stopped;
    }
    ::kill(pid, SIGKILL);
    return waitFree(path, 3000) ? Stop::Stopped : Stop::StillRunning;
#endif
}

}  // namespace zfsmgr::base::session
