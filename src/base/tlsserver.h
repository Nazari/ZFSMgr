#pragma once

#include <functional>
#include <string>

// A minimal TLS server, without Qt: it accepts connections and hands over bytes.
//
// It knows nothing about HTTP or about the daemon's protocol: it reads, lets the caller
// answer, and closes. Who decides what those bytes mean is the caller.
//
// It lives in the base layer because more than one artefact needs it —the agent already
// issued its own certificates— and because issuing them with OpenSSL instead of invoking
// `openssl` through a shell is what makes it work on Windows, where that binary is not on
// the PATH.
namespace zfsmgr::base::tlsserver {

// Issues a SELF-SIGNED certificate + key pair.
//
// It carries subjectAltName, keyUsage and extendedKeyUsage because without them Apple's TLS
// backend rejects them. The files are left readable by their owner only.
bool writeSelfSignedPair(const std::string& certPath, const std::string& keyPath,
                           const std::string& commonName, bool forServer,
                           const std::string& altNames, std::string& error);

// Writes a chunk of the answer straight into the connection. False when it was cut.
using Writer = std::function<bool(const char* data, std::size_t howMany)>;

// Listens on `bind:port` and serves connections one at a time.
//
// `handle` receives what arrived and returns what to answer; when it returns false, the
// connection is closed without answering. `stillAlive` is consulted between connections so
// that it can be stopped.
//
// One at a time and not with threads: what matters here is the surface, not the throughput,
// and a sequential server has no races to review.
//
// `onListening`, when supplied, is called ONCE and only when the socket is already
// listening. That is where the «server up» banner belongs: printing it before calling in
// here left the user with a URL that never worked whenever the port was taken.
bool serve(const std::string& bind, int port, const std::string& certPath,
           const std::string& keyPath,
           const std::function<bool(const std::string& request, std::string& response)>& handle,
           const std::function<bool()>& stillAlive, std::string& error,
           const std::function<void()>& onListening = {},
           // For answers that do NOT fit in memory. Tried before `handle`; when it returns
           // true it has already written its own output and the other is not called.
           //
           // It exists because of files: serving a 50 GB image by composing the answer in a
           // `std::string` is asking for 50 GB of memory in order to push it down a socket.
           const std::function<bool(const std::string& request, const Writer& write)>&
               handleStream = {});

}  // namespace zfsmgr::base::tlsserver
