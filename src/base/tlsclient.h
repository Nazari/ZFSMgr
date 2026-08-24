#pragma once

#include <functional>
#include <string>

// A TLS client with mutual authentication, over OpenSSL and without Qt.
//
// It was the third piece tying the transport to Qt, and the only one that had to be written:
// the agent already had the SERVER side with OpenSSL, and process execution existed too.
// This is its counterpart.
//
// **VALIDATION IS BY CERTIFICATE PINNING, NOT BY CA**, and that is not a shortcut. It is the
// same decision the Qt version already made, and its reason is measured: on macOS,
// SecureTransport never validated the chain («The root CA certificate is not trusted for
// this purpose»), not even with correct subjectAltName, extendedKeyUsage, keyUsage and
// basicConstraints. Since the daemon's certificate is fetched OVER SSH and stored, comparing
// against THAT exact certificate is stricter than trusting a chain.
//
// Mutual authentication is kept intact: the client sends its certificate and the daemon
// requires it with SSL_VERIFY_PEER.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

struct TlsClientConfig {
    std::string host;
    unsigned short port{0};
    // The daemon's certificate, in PEM. THIS is what gets compared against.
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    int connectTimeoutMs{8000};
    int ioTimeoutMs{30000};
};

// At which point it failed. Returned SEPARATELY from the text because the caller makes
// different decisions depending on which one it is, and making them by searching for
// substrings in a message is fragile: never connecting and the TLS handshake failing point at
// opposite causes —transport versus certificates—, and confusing them leads to
// re-provisioning TLS in order to fix a tunnel.
enum class TlsFailure {
    None,
    BadMaterial,  // the PEM we were handed is not valid
    Connect,      // the socket never opened
    Handshake,    // TLS failed
    Pinning,      // the certificate is NOT the expected one. Never a transient failure.
    Write,
    Read,
};

// Hooks for whoever needs more than «send and wait».
struct TlsRequestHooks {
    // Called JUST BEFORE the first byte is written. It is the point from which the command
    // may have reached the other side, and therefore the point from which RESENDING it would
    // be running it twice. Before and not after, because a partial write arrives too.
    std::function<void()> onBeforeWrite;

    // Called while waiting for the answer, every few hundred milliseconds.
    // **Returning false ABANDONS the wait.** It is what allows leaving the moment the tunnel
    // process dies, instead of sitting out the whole timeout.
    std::function<bool()> keepWaiting;
};

// Sends a request and returns the answer up to the first newline, which is the daemon's
// protocol: one JSON line out, one back.
//
// It returns false and describes the failure in `error` when it could not connect, when the
// certificate presented is NOT the expected one, or when the conversation was cut. When in
// any doubt, false: the caller must be able to tell «it could not» from «it answered no».
bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error);

// The same one, also saying at which point it failed and accepting hooks.
bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error,
                    TlsFailure& failure,
                    const TlsRequestHooks& hooks);

// Is this really a certificate / a private key?
//
// Checked BEFORE anything is built. Finding out inside the TLS handshake would cost the whole
// tunnel —almost a second— and, worse, the failure would read as a network problem when what
// is going on is that the stored material is unusable.
bool pemCertificateIsValid(const std::string& pem);
bool pemPrivateKeyIsValid(const std::string& pem);

}  // namespace zfsmgr::base
