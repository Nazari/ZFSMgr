#pragma once

// Rutas y cargas útiles de instalación del agente, SIN Qt.
//
// Primera pieza de la capa base: la lógica vive aquí y `src/daemonpayload.h` se queda
// como adaptador que convierte a QString en la frontera, para no tocar los 47 puntos
// de llamada del cliente de una sentada. Ver
// docs/diseno_tecnico_capa_base_sin_qt.md.

#include <string>

namespace zfsmgr::base::daemonpayload {

std::string unixBinPath();
std::string unixConfigPath();
std::string macPlistPath();
std::string linuxServicePath();
std::string freeBsdRcPath();
std::string windowsDirPath();
std::string windowsTaskName();
std::string tlsDirPath();
std::string tlsServerCertPath();
std::string tlsServerKeyPath();
std::string tlsClientCertPath();
std::string tlsClientKeyPath();

std::string windowsBinPath();
std::string windowsUploadPath();
std::string windowsNativeInstallCommand();
std::string macLaunchdPlist();
std::string freeBsdRcScript();
std::string linuxSystemdService();
std::string simpleConfigPayload(const std::string& version, const std::string& apiVersion);
std::string tlsBootstrapShellCommand();

}  // namespace zfsmgr::base::daemonpayload
