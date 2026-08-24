#pragma once

#include "connectionprofile.h"
#include "transportreason.h"
#include "transportsession.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The part of the transport that **executes**: a command over SSH, and the RPC against this
// machine's daemon.
//
// The second batch of the move. It is split from `transportcmd.h` because here there is I/O
// —a process is launched, a socket is opened—, and that changes how it gets verified: what
// lives there is checked byte for byte against the Qt version, and this has to be tested
// against a machine.
//
// What is NOT here, and goes in the third batch, is the `ssh -L` tunnel: it keeps processes
// alive between calls, needs a free port, and depends on somebody letting the interface
// breathe while it is being built.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// Runs a command over SSH and returns what came out. **It logs nothing**: it is the path
// the background watchdog threads use, where writing to the log from outside the interface
// thread was the problem, not the solution.
//
// `timeoutMs <= 0` takes 15 s, which is the value the Qt version had.
bool runSshRaw(const ConnectionProfile& p,
               const std::string& remoteCmd,
               int timeoutMs,
               std::string& out,
               std::string& err,
               int& rc);

// Which address to connect to, given what `AGENT_BIND` says in agent.conf.
//
// It exists because the LISTEN address is no use as a CONNECT address: the daemon may listen
// on `0.0.0.0` or on `::`, which mean «on all of them», and nobody connects to that. Same
// when the value is not a valid address. In all three cases it falls back to `127.0.0.1`,
// which is where the local daemon is anyway.
std::string bindAddressToConnectHost(const std::string& bindAddress);

// What the call cost and why it failed, so the caller can log it. Returned rather than
// written here: this layer does not know where the log is.
struct LocalRpcDiag {
    long long elapsedMs{0};
    FailureReason failure;  // empty when it went well
};

// The RPC against THIS machine's daemon: one JSON line out, one back, over TLS with mutual
// authentication and **validating by certificate pinning**, not by CA.
//
// The TLS material arrives as a parameter and is not read from disk: it lives under
// /etc/zfsmgr with root permissions, so whoever holds it already had to elevate to read it.
bool runLocalAgentRpc(const std::vector<std::string>& agentArgs,
                      const std::string& serverCertPem,
                      const std::string& clientCertPem,
                      const std::string& clientKeyPem,
                      std::uint16_t daemonPort,
                      int timeoutMs,
                      std::string& out,
                      std::string& err,
                      int& rc,
                      LocalRpcDiag* diag = nullptr);

// --- Name resolution, only so it can be REPORTED.
//
// It is not used to connect —`ssh` takes care of that—, but to put on record what a name
// resolved to. It exists because `*.local` names go over mDNS and failures there are the
// kind that get misdiagnosed: they look like «the machine is not answering».
struct HostResolution {
    bool ok{false};
    std::string error;
    // «IPv4:192.168.1.33», exactly as it is written into the log.
    std::vector<std::string> addresses;
};
HostResolution resolveHostAddresses(const std::string& host);

// --- The TLS material of THIS machine's daemon.
//
// It lives under /etc/zfsmgr with root permissions, so elevating may be needed to read it;
// it is cached for five minutes so as not to ask for credentials on every command.
bool ensureLocalDaemonTlsMaterial(TransportSession& ses,
                                  std::string& serverCertPem,
                                  std::string& clientCertPem,
                                  std::string& clientKeyPem,
                                  std::uint16_t& daemonPort);
void clearLocalDaemonTlsCache();

// --- The two high-level paths.

// Tries the agent's typed RPC against an SSH connection. Returns false when it has to fall
// back to the usual path.
//
// **It returns TRUE with rc=124 in one very specific case**: when a MUTATION reached the
// daemon and there was no answer. It is «true» because it must not be retried over SSH —that
// would be running the same destructive command a second time—, and the message says so to
// the user.
bool tryAgentRpcOverSsh(TransportSession& ses,
                        const ConnectionProfile& p,
                        const std::vector<std::string>& agentArgs,
                        int timeoutMs,
                        std::string& out,
                        std::string& err,
                        int& rc,
                        const std::function<void(const std::string&)>& onStdoutLine = {},
                        const std::function<void(const std::string&)>& onStderrLine = {},
                        bool echoOutputToLog = true);

// Runs a command on the machine. With `allowAgentRpc`, it tries the agent's typed RPC first
// and only falls back to raw SSH when it cannot.
//
// **The timeout is one of INACTIVITY, not a total**: it restarts with every chunk that
// arrives. A transfer that takes hours must not die for taking long; it must die when it
// goes silent.
bool runSsh(TransportSession& ses,
            const ConnectionProfile& p,
            const std::string& remoteCmd,
            int timeoutMs,
            std::string& out,
            std::string& err,
            int& rc,
            const std::function<void(const std::string&)>& onStdoutLine = {},
            const std::function<void(const std::string&)>& onStderrLine = {},
            const std::function<void(int)>& onIdleTimeoutRemaining = {},
            const std::string& stdinPayload = {},
            bool allowAgentRpc = true,
            bool echoOutputToLog = true);

}  // namespace zfsmgr::base::transport
