#pragma once

#include "connectionprofile.h"

#include <cstdint>
#include <string>
#include <vector>

// The part of the transport that **decides and parses text**, without touching network or
// processes.
//
// It is the first batch of the transport's move into the base layer. It is split this way on
// purpose: everything here is a pure function —text or a profile goes in, a decision or more
// text comes out—, so it can be checked BYTE FOR BYTE against the Qt version without
// bringing up a machine or opening a socket. What stays in `src/transport.h` is what does
// open sockets, launch processes and keep tunnels alive, and that goes in later batches
// because it needs new pieces in the base.
//
// It is not called `transport.h` because `src/base` is on the include path alongside `src/`,
// and while the `src/transport.h` adapter exists two files with the same name would make the
// `#include` ambiguous. When the adapter goes away, this gets renamed.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// --- What kind of machine is on the other side.

// «LOCAL» as the connection type: there is no SSH in between.
bool isLocalConnection(const ConnectionProfile& p);
bool isWindowsConnection(const ConnectionProfile& p);

// --- The key a connection is remembered by.
//
// It comes from the COORDINATES (user, host, port, key path), not from its position in the
// list. It is the same rule that fixed the index-keyed caches: when a connection is deleted,
// the next one must inherit nothing of it.
std::string remoteDaemonTlsCacheKey(const ConnectionProfile& p);

// --- The command sent to the other end.

// Wraps the command for the shell on the other side: PowerShell on Windows, and as-is
// everywhere else. On Windows it goes encoded as UTF-16LE base64 (`-EncodedCommand`), which
// is the only thing that survives crossing an `ssh` and a `cmd.exe`.
std::string wrapRemoteCommand(const ConnectionProfile& p, const std::string& remoteCmd);

// --- Whether a command changes the state of the other side.
//
// **This is the delicate function of the whole file.** Whether a mutation that may have
// arrived is NOT resent depends on it: resending a `--dump-*` costs nothing, but resending a
// `--job-submit` launches the same transfer twice over the same data.
bool isMutatingAgentCommand(const std::vector<std::string>& agentArgs);

// --- LEGACY path: recovering the arguments out of a shell string.
//
// It exists only for the places that still build the command as a string. **Do not add new
// callers here**: the split on a separator, the prefix allow-list and the undoing of the
// double quoting are assumptions about how the string was built, and each one has failed at
// least once.
bool extractLocalAgentArgs(const std::string& remoteCmd, std::vector<std::string>& argsOut);

// --- El material TLS que el daemon remoto manda por SSH.

struct RemoteTlsBundle {
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    std::uint16_t port{47653};
    bool clientKeyIncluded{false};
};

// Parses the dump delimited by `__ZFSMGR_TLS_BEGIN__:` / `__ZFSMGR_TLS_END__:`. It returns
// false when the server's or the client's certificate is missing, which are the two
// indispensable ones: without them no conversation is possible.
bool parseRemoteDaemonTlsBundle(const std::string& text, RemoteTlsBundle& out);

// --- The configuration of THIS machine's daemon.

struct LocalAgentConfig {
    std::string bindAddress{"127.0.0.1"};
    std::uint16_t port{47653};
    std::string tlsCertPath;
    std::string tlsClientCertPath;
    std::string tlsClientKeyPath;
};

// This platform's default paths. They have to match kDefaultTlsDir and
// kDefaultAgentConfigPath in daemon_main.cpp; they used to be hard-wired to the POSIX ones,
// which is why on Windows the Local connection never found its own TLS material.
const char* defaultAgentConfigPath();
const char* defaultAgentTlsCertPath();
const char* defaultAgentTlsClientCertPath();
const char* defaultAgentTlsClientKeyPath();

// Parses the contents of agent.conf. Whatever does not appear keeps its default value: a
// half-written file must not leave the configuration unusable.
LocalAgentConfig parseLocalAgentConfig(const std::string& text);
// The same one, reading the file. When it cannot be opened it returns the defaults —which is
// the normal case when no daemon is installed, not an error—.
LocalAgentConfig loadLocalAgentConfig(const std::string& path = defaultAgentConfigPath());

// --- Cleaning up what the other side answers.

// PowerShell spits a CLIXML preamble onto the error stream whenever there are warning or
// information streams. It is not an error: it is noise shaped like XML, and it gets stripped.
std::string sanitizeWindowsCliXml(const std::string& raw);

// Is it worth retrying SSH without multiplexing? Only for the failures that give away that
// the control socket is unusable; for any other, retrying would be hiding the problem.
bool shouldRetrySshWithoutMultiplexing(const std::string& stderrText);

}  // namespace zfsmgr::base::transport
