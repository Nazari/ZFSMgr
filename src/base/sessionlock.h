#pragma once

// One interactive session of ZFSMgr per machine: the interface, or the CLI's shell on a
// terminal, never two at once —whoever runs them—. Commands given on the command line, a
// script feeding the shell through a pipe and the `--serve` services the interface starts are
// not sessions and take nothing.
//
// The lock is a file every user can open, locked by the kernel: `flock` on Linux and macOS,
// `LockFileEx` on Windows. The kernel lets go when its owner dies, however it dies, so a crash
// leaves nothing behind. Inside the file the owner writes who it is —what, which user, its PID
// and since when— for the next one to say so.
//
// Only the standard library and the system's own calls: the interface, which links nothing
// but Qt, compiles this file as it is (resources/CMakeLists.txt).
//
// ZFSMGR_SESSION_LOCK=<path> moves the file, for the tests: they must not collide with the
// interface somebody has open on the same machine.

#include <string>

namespace zfsmgr::base::session {

// Who holds the session. Fields empty or 0 when the file could not be read —its owner may not
// have written it yet—.
struct Holder {
    std::string kind;  // «gui» or «cli»
    std::string user;
    long long pid{0};
    long long since{0};  // seconds since the epoch
};

enum class Acquire {
    Held,    // this process has it
    Busy,    // another one has it: `holder` says who
    Failed,  // the file could not be opened: `detail` says why. Go on WITHOUT it —refusing to
             // start over a lock file would be worse—, but say so.
};

class Lock {
public:
    Lock() = default;
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    // `kind`: «gui» or «cli».
    Acquire acquire(const std::string& kind, Holder& holder, std::string& detail);
    void release();
    bool held() const;

private:
#ifdef _WIN32
    void* m_handle{nullptr};
#else
    int m_fd{-1};
#endif
};

std::string lockPath();
// The user running this process.
std::string currentUser();
// This machine's name, without its domain: what other sessions are told this one runs on.
std::string hostName();

enum class Stop {
    Stopped,       // it ended and let go of the lock
    NotAllowed,    // it belongs to another user: only an administrator can end it
    StillRunning,  // it was told to end and did not
    Failed,        // `detail` says why
};

// Ends the process holding the session: asked first (SIGTERM) and forced after a few seconds
// (SIGKILL); on Windows it is terminated straight away. Only while it still holds the lock and
// is the same process the file named, so a PID reused meanwhile is never touched.
Stop stop(const Holder& holder, std::string& detail);

}  // namespace zfsmgr::base::session
