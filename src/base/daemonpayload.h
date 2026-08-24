#pragma once

// Paths and installation payloads for the agent, WITHOUT Qt.
//
// The first piece of the base layer: the logic lives here and `src/daemonpayload.h` remains
// as an adapter that converts to QString at the boundary, so as not to touch the client's 47
// call sites in one sitting. See docs/diseno_tecnico_capa_base_sin_qt.md.

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
