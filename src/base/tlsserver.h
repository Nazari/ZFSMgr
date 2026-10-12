#pragma once

#include <functional>
#include <string>

// Issuing the agent's TLS certificates with OpenSSL, without Qt. Done here instead of invoking
// `openssl` through a shell, which is what makes it work on Windows, where that binary is not
// on the PATH. (The HTTPS server that also lived here served the web client, retired.)
namespace zfsmgr::base::tlsserver {

// Issues a SELF-SIGNED certificate + key pair.
//
// It carries subjectAltName, keyUsage and extendedKeyUsage because without them Apple's TLS
// backend rejects them. The files are left readable by their owner only.
bool writeSelfSignedPair(const std::string& certPath, const std::string& keyPath,
                           const std::string& commonName, bool forServer,
                           const std::string& altNames, std::string& error);

}  // namespace zfsmgr::base::tlsserver
