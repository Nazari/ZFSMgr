#pragma once

#include "connectionprofile.h"
#include "transportreason.h"
#include "transportsession.h"

#include <cstdint>
#include <string>
#include <vector>

// RPC over the `ssh -L` tunnel, which is the normal path whenever there is a daemon.
//
// The third batch of the move, and the delicate one: mutations come through here. Everything
// in this file exists to answer a single question precisely —**could the command have
// reached the other side?**—, because whether a retry is allowed depends on it. Resending a
// `--dump-*` costs nothing; resending a `--job-submit` launches the same transfer twice over
// the same data.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

// Fetches the REMOTE daemon's TLS material: first from the in-memory cache, then from the
// saved profile, and only when there is none, over SSH.
//
// `forceRefresh` skips the first two, which is what has to happen when the saved material
// has stopped being valid.
struct RemoteTlsMaterial {
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    std::uint16_t daemonPort{47653};
    // Whether it came from the remote machine —and is therefore worth saving— and whether
    // the private key came with it. The second matters because the daemon stops handing it
    // over once provisioned.
    bool fetchedFromRemote{false};
    bool clientKeyFetchedFromRemote{false};
};
bool fetchRemoteDaemonTlsMaterial(const ConnectionProfile& p,
                                  bool forceRefresh,
                                  RemoteTlsMaterial& out,
                                  FailureReason* failureReason = nullptr);

// Empties the in-memory cache of remote TLS material. Needed when a connection is
// re-provisioned: otherwise the old certificate would go on being used for up to five
// minutes.
void clearRemoteDaemonTlsCache();
// Only one connection's, which is what re-provisioning it needs: emptying everyone's would
// force the other machines into an SSH round trip for no reason.
void clearRemoteDaemonTlsCacheForConnection(const ConnectionProfile& p);

// Tries to bring the daemon's service up on the other machine. It returns whether the
// command got to run, NOT whether the daemon came back: that is learned by retrying.
bool tryReviveRemoteDaemonService(const ConnectionProfile& p);

// Closes every live tunnel in the session.
void closeAllTunnels(TransportSession& ses);
// Closes the one belonging to a specific connection, when there is one.
void closeTunnelForConnection(TransportSession& ses, const ConnectionProfile& p);

// The RPC over the tunnel.
//
// `commandMayHaveRunOut` tells «it could not be sent» apart from «it was sent and there was
// no answer». That is the distinction that keeps a destructive mutation from being resent
// twice, and it is set BEFORE the first byte is written: a partial write arrives too.
bool tryRunRemoteAgentRpcViaTunnel(TransportSession& ses,
                                   const ConnectionProfile& p,
                                   const std::vector<std::string>& agentArgs,
                                   int timeoutMs,
                                   std::string& out,
                                   std::string& err,
                                   int& rc,
                                   FailureReason* failureReason = nullptr,
                                   bool* commandMayHaveRunOut = nullptr);

}  // namespace zfsmgr::base::transport
