#pragma once

#include <string>

// Why the daemon could not be talked to, TYPED.
//
// This used to be a sentence in Spanish, and the code decided by reading it:
//
//     if (contains(reason, "handshake tls daemon-rpc") || contains(reason, "conexión ..."))
//         tryReviveRemoteDaemonService(p);
//
// That tied together three things that need not travel together: how the failure is told to
// a person, in which language, and what the program does next. Changing a comma in the
// sentence switched off the retry; translating it to English switched it off entirely and
// **silently**, because there is no way for that to fail loudly: the function simply stops
// being called. The same held for comparing against `rpcTunnelBusyReason()`, where «busy»
// —which is NOT a failure— would have started being treated as a broken connection, 30-second
// penalty included.
//
// Now the base layer returns WHAT happened and whoever has an interface decides how it is
// worded. It is the same split `store::Reason` already made with the store's warnings.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

enum class Failure {
    None_ = 0,

    // --- Before even getting to try
    // Busy is NOT broken: the tunnel is being built in an earlier stack frame. This call
    // skips the RPC and leaves by the usual path, without penalising the connection.
    TunnelBusy,
    OffTheTunnelThread,
    EmptyArguments,
    ConnectionNotSsh,
    Cooling,                 // penalty in force. detail: the seconds left

    // --- The remote daemon's TLS material
    MaterialCannotBeRead,    // detail: what the other machine said
    MaterialIncomplete,      // an answer arrived, but without the three pieces
    ClientKeyUnavailable,    // neither local nor remote; the daemon hands it over only once
    InvalidCertificates,
    InvalidClientKey,

    // --- The SSH tunnel
    TunnelCannotBeBuilt,

    // --- The TLS session against the daemon
    // The socket never opened. This is NOT a handshake failure: counting it as one would
    // point the diagnosis at the certificates and fire off a re-provisioning incapable of
    // fixing a transport problem.
    ConnectionRefused,       // detail: the socket's error
    CertificateMismatch,     // pinning: the daemon presents a different certificate
    SendFailed,
    TunnelCutWhileWaiting,
    HandshakeFailed,         // detail: the TLS error
    InvalidAnswer,           // detail: the parser's error, when there is one

    // It failed without saying why. It exists so that «it did not fail» never has to be told
    // apart from «it failed and did not say», which is exactly where an empty reason slipped
    // through.
    Unspecified,
};

// --- The WARNINGS the transport sends to the log.
//
// Same reason as the failure reasons, different place: the base layer cannot write the prose
// of these either, because they end up in front of the user. With `--lang en` you saw an
// English session sprinkled with «no se pudo leer el material TLS del daemon».
//
// What does NOT come through here, and it is not an oversight: the TRACES —the command being
// run, the `[daemon-rpc:fallback]` lines, the resolved addresses—. That is not prose, it is
// the technical trail one reads with grep, and translating it would get in the way rather
// than help.
enum class Warning {
    None_ = 0,

    // --- TLS material of THIS machine's daemon
    LocalTlsUnreadable,      // path: where it was expected to be
    LocalTlsNeedsSudo,
    LocalTlsCannotBeRead,    // detail: what the command said
    LocalTlsIncomplete,

    // --- SSH
    SshHostUnverified,
    NoSshpass,
    MultiplexingFailed,
    MultiplexingDisabled,

    // --- Tunnel. Two warnings and not one with the reason inside: «the ssh died» and «the
    // wait ran out» are different things, and putting which one it was into the detail would
    // have gone back to keeping text where a type belongs. In both, `detail` is milliseconds.
    TunnelNotAcceptingSshDied,
    TunnelNotAcceptingTimedOut,
};

// One warning with what goes with it. Named fields, as in `store::Warning`.
struct WarningNote {
    Warning warning{Warning::None_};
    std::string path;
    std::string detail;

    bool empty() const { return warning == Warning::None_; }
};

// The stable ASCII label of a warning. Used as a FALLBACK when nobody has installed a
// translator: it is ugly, but losing a warning silently is worse.
const char* labelOf(Warning w);

// The reason with what goes with it. See `store::Warning`: named fields and not a list of
// arguments, so that the place that builds it reads on its own.
struct FailureReason {
    Failure failure{Failure::None_};
    std::string detail;

    bool empty() const { return failure == Failure::None_; }
};

// --- The decisions that used to be made by reading the sentence.
//
// All three are implemented with a `switch` WITHOUT a `default`: that way, the day a new
// reason is added, the compiler forces someone to come through here and decide. With the
// text comparison, a new reason simply matched nothing and nobody found out.

// Does this failure look like a downed daemon, and therefore deserve trying to bring it back
// before retrying? The certificate ones do NOT: if the material is wrong, reviving the
// service fixes nothing and spends an SSH connection doing it.
bool suggestsDaemonRevival(Failure f);

// Is it about the TLS material or the handshake? The interface uses this to decide whether
// to show the penalty as «TLS cooling off» or keep quiet about it.
bool looksLikeTls(Failure f);

// Is it worth penalising the connection for 30 s? Busy and «off the thread» are not: they say
// nothing about whether the daemon is alive.
bool deservesPenalty(Failure f);

// A stable ASCII label for the LOG. It is not text to be read: it is what one greps for in a
// log that may come from a machine in another language. The text for people is supplied by
// whoever has an interface.
const char* labelOf(Failure f);

}  // namespace zfsmgr::base::transport
