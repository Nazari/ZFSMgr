#pragma once

#include <string>

// The data of one connection, without Qt.
//
// A field-for-field mirror of `ConnectionProfile` (src/connectionstore.h). ALL of them are
// copied, including the ones the base layer does not use today: a partial mirror invites
// someone later on to read a field that arrives silently empty, and that failure is far
// worse than copying a few extra strings while building a command that is about to launch a
// process.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

struct ConnectionProfile {
    std::string id;
    std::string name;
    std::string machineUid;
    std::string connType;
    std::string osType;
    std::string host;
    int port{0};
    std::string sshAddressFamily;
    std::string username;
    std::string password;
    std::string keyPath;
    bool useSudo{false};
    std::string daemonTlsServerCertPem;
    std::string daemonTlsClientCertPem;
    std::string daemonTlsClientKeyPem;
    int daemonTlsPort{47653};
};

}  // namespace zfsmgr::base
