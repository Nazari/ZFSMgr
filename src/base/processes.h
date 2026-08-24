#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Running a program: no shell, no Qt, and on all four platforms.
//
// This is the agent's code, which had been doing it without Qt for a while and is tested
// against Linux, macOS, FreeBSD and Windows. It was pulled out here because the CLIENT needs
// it just the same: the transport used `QProcess`, and that is what kept the network layer
// from living outside Qt.
//
// There is NEVER a shell in between: argv is passed and executed directly. That is what
// makes a dataset name containing `;` or quotes unable to turn into a different command.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

// Translates the status `wait` returns into an exit code: the program's, or 128+signal when
// it died from one, which is the shells' convention.
int decodeWaitStatus(int status);

struct ExecResult {
    int rc{1};
    std::string out;
    std::string err;
};

// Runs and captures output and error separately.
ExecResult runExecCapture(const std::string& program, const std::vector<std::string>& args);

// Runs inheriting the current process's output: for what goes to the console as-is.
int runExecStreaming(const std::string& program, const std::vector<std::string>& args);

// Runs while feeding standard input. This is what allows handing a stream to `zfs recv` or a
// passphrase to `zfs load-key` without it going through the command line —where it would be
// visible in `ps`—.
ExecResult runExecCaptureWithStdin(const std::string& program,
                                   const std::vector<std::string>& args,
                                   const std::string& stdinData);

// --- Running with feedback, for long operations.
//
// `runExecCapture` is enough for a command that answers and finishes. What it does NOT cover
// is what a transfer needs: showing the lines as they arrive, reporting how much is left and
// being cancellable. `QProcess` did that by pumping Qt's event loop, and that is what tied
// the transport to the interface.
struct StreamCallbacks {
    // Called with each COMPLETE line, without the trailing newline. Whatever is left
    // unterminated when the process ends is delivered all the same: `zfs send` writes its
    // progress with carriage returns and does not always close the last one.
    std::function<void(const std::string& line)> onStdoutLine;
    std::function<void(const std::string& line)> onStderrLine;

    // Called every few milliseconds even when nothing arrives. **Returning false CANCELS**:
    // the process is terminated and the result comes out with the corresponding code.
    //
    // A single hook for the three things Qt's loop did: letting the interface breathe,
    // counting down what is left, and checking whether the user cancelled. Whoever has no
    // interface simply does not supply it.
    std::function<bool(int elapsedMs)> onTick;
};

// Runs with feedback. `timeoutMs <= 0` means NO limit, which is what a long transfer needs;
// control then rests with `onTick`.
//
// The result's `out` and `err` also carry the full text, for whoever wants it at the end
// without having accumulated it.
ExecResult runExecStream(const std::string& program,
                         const std::vector<std::string>& args,
                         const std::string& stdinData,
                         int timeoutMs,
                         const StreamCallbacks& cb);

// --- A process that stays ALIVE between calls.
//
// Everything above launches something, waits and collects. An `ssh -L` tunnel is not that:
// it is brought up, used many times and closed when it is no longer needed. That was the
// last thing forcing tunnels to be `QProcess` objects hanging off something with an event
// loop.
//
// **The destructor kills it.** An `ssh -L` that outlives whoever created it leaves a port
// listening and a connection open against the other machine, and nobody closes them again.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    // Neither copyable nor assignable: two objects holding the same child would kill it twice.
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    // Launches. Returns false when it could not. As everywhere in this file, NO shell.
    bool start(const std::string& program, const std::vector<std::string>& args);

    // Still alive? It does not block, and it also REAPS the child when it has just died:
    // without this, every closed tunnel would leave a zombie.
    bool isRunning();

    // Ends it politely and, when it does not listen within `waitMs`, impolitely. Idempotent.
    void stop(int waitMs = 1500);

    long long pid() const { return m_pid; }

private:
    void olvida();
    long long m_pid{0};
#ifdef _WIN32
    void* m_handle{nullptr};
#endif
    bool m_recogido{true};
};

// --- Local ports.

// A free port on the machine, for the local end of an `ssh -L` tunnel. It reserves one on
// 127.0.0.1 and releases it. Returns 0 when there is none.
//
// **There is a race and it is unavoidable**: between releasing it and `ssh -L` taking it,
// another process could grab it. It is the same thing the Qt version did, and the
// alternative —handing ssh an already-open descriptor— does not exist on its command line.
// When it happens, `ssh` fails to forward and the tunnel is not accepted, which is the
// correct behaviour.
//
// **It never returns a port this program reserves for itself** —47653 the daemon, 47654 the
// web server—, even when the kernel offers it. Linux's ephemeral range starts at 32768, so
// both fall inside it and the system hands them out like any other: a tunnel could take the
// web server's port and then the web server would not start, with a message that did not say
// who had it.
std::uint16_t reserveFreeLocalPort();

// Is that port on 127.0.0.1 accepting connections yet? It is the question to ask a
// freshly-built tunnel: connecting too early gives ECONNREFUSED, and the caller counted that
// as a TLS handshake failure and penalised the connection for no reason.
bool canConnectLocal(std::uint16_t port, int timeoutMs);

// --- Killing a process tree.
//
// Cancelling an action has to take down the WHOLE descent, not just the direct children: the
// real chain of a transfer is `sh -> sudo -> sh -> zfsmgr-agent -> tar`, and if the `tar`
// survives it goes on writing at the target and leaves the mountpoint busy —to the point of
// not being able to destroy the dataset—. Actually seen after aborting a copy.
//
// There used to be a shell script here that called `pgrep -P` per process and per each of
// eight levels, finishing with a `sleep 0.3` and two `kill` loops. It is replaced by ONE
// read of `ps` and a direct `kill()`, which is a system call and not a process. The
// eight-level cap disappears along the way, having had no reason to exist.

// The descendants of `root` according to the output of `ps -eo pid=,ppid=`, **leaves first,
// root last**.
//
// That order is the part that matters: killing the parent first orphans the child, which can
// then carry on; and a live parent can beget another child while the grandchild is being
// killed.
//
// `root` is NOT included —whoever launched it takes care of that— and cycles do not hang:
// each pid is visited exactly once.
//
// Kept apart from the execution so it can be tested with a hand-written `ps` output, which
// is the only part of this that can be checked without killing real processes.
std::vector<long long> descendantsOf(long long root, const std::string& psOutput);

// Kills the descent of `root`: TERM to all of them, `graceMs` of waiting, and KILL to
// whoever is still standing. It does not touch `root`.
void killDescendants(long long root, int graceMs = 300);

#ifdef _WIN32
// CreateProcess takes ONE string and it is the program itself that splits it again, so the
// quoting is the responsibility of whoever builds it. The rule is not the intuitive one:
// backslashes are only doubled when they precede a quote.
std::string winBuildCommandLine(const std::string& program,
                                const std::vector<std::string>& args);
#endif

}  // namespace zfsmgr::base
