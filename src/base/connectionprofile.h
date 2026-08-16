#pragma once

#include <string>

// Los datos de una conexión, sin Qt.
//
// Espejo campo a campo de `ConnectionProfile` (src/connectionstore.h). Se copian TODOS,
// incluidos los que la capa base no usa hoy: un espejo parcial invita a que alguien
// lea más adelante un campo que llega silenciosamente vacío, y ese fallo es mucho peor
// que copiar unas cadenas de más al construir una orden que va a lanzar un proceso.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
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
