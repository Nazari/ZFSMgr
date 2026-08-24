#pragma once

#include "connectionprofile.h"
#include "processes.h"
#include "transportreason.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// What stays OPEN while the remote machines are being talked to: the RPC tunnels and the
// memory of the attempts that failed.
//
// It exists for two reasons. One: it is what a CLI needs in order to talk to the agent, and
// while these were loose fields on the window they could not be used from anywhere else.
// Two, and more important: **the lock and what it protects were separated**, and only a
// comment said which ones belonged together. Now they live in the same structure.
//
// The keys are NOT connection indices: they come from the coordinates (user, host, port, key
// path), so they survive the list being reordered. See
// docs/diseno_tecnico_capa_base_sin_qt.md, the section on position-keyed caches, for what
// happens when they are not.
namespace zfsmgr::base {

struct RemoteRpcTunnelState {
    // The process LIVES in here. It used to be a `QProcess` hanging off the window, and
    // that was the transport's last tie to an object with an event loop. Since
    // `ChildProcess` does not copy, neither does this structure: it moves.
    ChildProcess process;
    std::uint16_t localPort{0};
    std::uint16_t remotePort{0};
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point lastUsed;

    RemoteRpcTunnelState() = default;
    RemoteRpcTunnelState(RemoteRpcTunnelState&&) = default;
    RemoteRpcTunnelState& operator=(RemoteRpcTunnelState&&) = default;
};

struct TransportSession {
    // --- Where what the transport reports while working goes.
    //
    // Having each call RETURN the list of what happened, and letting the caller decide what
    // to do with it, was considered. It is cleaner on paper, but **it would have been a
    // regression**: the application's log writes as it goes, so today it fills up WHILE the
    // operation happens. Accumulating and returning at the end would leave thirty seconds of
    // silence and then a dump all at once.
    //
    // So it is emitted as it goes, but **to something that is supplied**, not to something
    // the transport goes looking for. The interface supplies a sink that writes into its tab;
    // a CLI would supply one that writes to standard error.
    enum class Level { Normal, Info, Warn, Error, Debug };

    // An empty `connId` means «to the general log»; with a value, to that connection's as
    // well. With no sink supplied nothing important is lost: it simply is not reported.
    std::function<void(Level, const std::string& connId, const std::string& msg)> sink;

    void log(Level n, const std::string& msg) const {
        if (sink) {
            sink(n, std::string(), msg);
        }
    }
    // To the general log AND to the connection's, which is the pair that was being repeated
    // by hand in thirty places.
    void logConn(Level n, const std::string& connId, const std::string& msg) const {
        if (sink) {
            sink(n, connId, msg);
        }
    }

    // --- The warnings, which are PROSE and therefore not written by this layer.
    //
    // `sink` remains for the TRACES: the command being run, the `[daemon-rpc:...]` lines, the
    // resolved addresses. That is a technical trail and it goes as-is. What ends up in front
    // of the user as a sentence comes through here typed, and whoever knows the language does
    // the wording. Without this split, a session with `--lang en` came out sprinkled with
    // Spanish.
    std::function<void(Level, const std::string& connId, const transport::WarningNote&)> warningSink;

    void warning(Level n, const std::string& connId, const transport::WarningNote& a) const {
        if (warningSink) {
            warningSink(n, connId, a);
            return;
        }
        // With no translator supplied it falls back to the stable label. It is ugly, but
        // losing a warning silently because nobody wired up the translator would be worse.
        if (sink) {
            sink(n, connId,
                 std::string(transport::labelOf(a.warning))
                     + (a.detail.empty() ? std::string() : ": " + a.detail));
        }
    }

    // --- Letting whoever called us breathe while we wait.
    //
    // It replaces the `QCoreApplication::processEvents` calls that were scattered around the
    // transport. It is the same thing `StreamCallbacks::onTick` already did: a single hook
    // for the three things Qt's loop did —repaint, count down what is left, and check whether
    // the user cancelled—.
    //
    // **Returning false CANCELS** the wait in progress. Whoever has no interface does not
    // supply it, and then the wait simply sleeps.
    //
    // **The parameter is NOT a detail.** It tells apart the two contexts the Qt version
    // deliberately treated differently:
    //
    // - While WAITING for a tunnel to accept connections: `false`. Pumping events reenters,
    //   and letting user actions through allowed a reload of connections to slip in, which
    //   left dangling the references the caller was holding.
    // - While a long command is RUNNING: `true`. It is what allows pressing Cancel during a
    //   transfer; without it the window repaints but does not respond.
    //
    // Unifying them on the strict one would stop Cancel working during transfers, and on the
    // permissive one would reopen the reentrancy. They are two different things.
    std::function<bool(bool allowUserInput)> pump;

    bool breathe(bool allowUserInput = true) const {
        return pump ? pump(allowUserInput) : true;
    }

    // --- Can tunnels be built from here?
    //
    // Without supplying it, yes: a single-threaded tool competes with nobody.
    //
    // **The original reason for this restriction NO LONGER EXISTS.** It was there because
    // tunnels were `QProcess` objects hanging off the window, and creating them from a
    // refresh thread produced an affinity warning or a crash; now they are `ChildProcess`,
    // which hang off nobody. It is kept so as NOT to change behaviour in the same step that
    // changes the engine: removing it would allow building tunnels from the refresh threads,
    // which is a real concurrency change and deserves measuring on its own.
    std::function<bool()> tunnelsAllowedHere;

    bool tunnelsAllowedFromHere() const { return tunnelsAllowedHere ? tunnelsAllowedHere() : true; }

    // Runs the task WHERE tunnels can be built, and waits for it to finish. The interface
    // solves it with a blocking call onto the window's thread.
    //
    // Without supplying it, it runs inline. That is right for whoever has no other thread:
    // doing nothing would leave the operation unperformed, which is worse than doing it
    // here.
    std::function<void(const std::function<void()>&)> runWhereTunnelsAllowed;

    void onTheTunnelThread(const std::function<void()>& task) const {
        if (!tunnelsAllowedFromHere() && runWhereTunnelsAllowed) {
            runWhereTunnelsAllowed(task);
            return;
        }
        task();
    }

    // --- A pretend transport, for the tests.
    //
    // It lives here and not on the window because it is a property OF THE TRANSPORT: while
    // it is set no connection is opened, argv commands go to that function, and any that
    // leave as a shell string are recorded and fail —so that a test can assert something did
    // NOT go out that way—.
    struct AgentCallForTest {
        std::vector<std::string> argv;  // empty when the command left as a shell string
        std::string shellCommand;       // non-empty only in that case
        std::string stdinPayload;
    };
    using AgentTransportForTest = std::function<bool(const std::vector<std::string>& argv,
                                                     std::string& out, std::string& err, int& rc)>;

    AgentTransportForTest transportForTest;
    std::vector<AgentCallForTest> callsForTest;

    // --- How credentials are asked for when they are needed.
    //
    // It is the second thing the transport needs from outside, alongside the log's sink:
    // **where to report** and **how to ask**. Both are supplied, neither is sought out — and
    // that is why there is not a single widget in here.
    //
    // It returns false when they could not be obtained —the user cancelled, or there was no
    // descriptor in a non-interactive context—. With no provider supplied it returns false,
    // which is the prudent answer: better to do nothing than to try without credentials.
    using CredentialProvider =
        std::function<bool(const std::string& reason, std::string& user, std::string& password)>;
    CredentialProvider credentialProvider;

    bool askCredentials(const std::string& reason, std::string& user, std::string& password) const {
        return credentialProvider ? credentialProvider(reason, user, password) : false;
    }

    // --- The two things the transport needs from the connection REGISTRY.
    //
    // The whole registry is deliberately not handed over: what it needs are not the profiles,
    // they are two decisions that depend on them. Handing it the registry would give it
    // access to everything —every machine's password included— in order to do two specific
    // things.
    //
    // Without supplying them, the transport still works: it does not resolve local
    // credentials and it does not store the TLS material it negotiates. A read-only CLI can
    // live like that.

    using LocalSudoResolver = std::function<bool(ConnectionProfile& profile)>;
    LocalSudoResolver localSudoResolver;

    using TlsPersister = std::function<bool(const ConnectionProfile& p,
                                            const std::string& serverCertPem,
                                            const std::string& clientCertPem,
                                            const std::string& clientKeyPem,
                                            std::uint16_t daemonPort,
                                            std::string* errorOut)>;
    TlsPersister tlsPersister;

    bool resolveLocalSudo(ConnectionProfile& profile) const {
        return localSudoResolver ? localSudoResolver(profile) : false;
    }
    bool persistTls(const ConnectionProfile& p, const std::string& serverCertPem,
                    const std::string& clientCertPem, const std::string& clientKeyPem,
                    std::uint16_t daemonPort, std::string* errorOut) const {
        if (!tlsPersister) {
            if (errorOut) {
                *errorOut = "no hay dónde guardar el material TLS";
            }
            return false;
        }
        return tlsPersister(p, serverCertPem, clientCertPem, clientKeyPem, daemonPort, errorOut);
    }

    // EVERYTHING below goes under this lock. The connection refresh runs in threads and
    // these maps get touched from several at once.
    mutable std::mutex mutex;

    // Live `ssh -L` tunnels, by connection key.
    std::map<std::string, RemoteRpcTunnelState> tunnelsByConnKey;

    // Keys whose tunnel is being built RIGHT NOW. It guards against the reentrancy the
    // wait's event pumping causes: without it, duplicate tunnels were built and left
    // orphaned outside the map.
    std::set<std::string> tunnelsBeingCreated;

    // Until when a connection's RPC is not retried, and why. Without this, a connection whose
    // daemon is down costs an SSH round trip on every operation.
    std::map<std::string, std::chrono::steady_clock::time_point> retryAfterByConnKey;
    std::map<std::string, transport::FailureReason> retryReasonByConnKey;

    // Connections for which SSH multiplexing has been given up on, and those whose name
    // resolution has already been noted in the log: both exist so as not to repeat the same
    // message on every operation.
    std::set<std::string> disableMultiplexKeys;
    std::set<std::string> loggedResolutionKeys;
};

}  // namespace zfsmgr::base
