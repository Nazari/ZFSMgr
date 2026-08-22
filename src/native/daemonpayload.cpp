#include "daemonpayload.h"

#include "base/daemonpayload.h"

// Adaptador. La lógica está en `src/base/daemonpayload.cpp`, que no depende de Qt;
// aquí solo se convierte en la frontera.
//
// Existe para que la extracción no obligara a tocar los 47 puntos de llamada del
// cliente en el mismo commit. Cuando esos puntos pasen a std::string, este fichero
// desaparece sin dejar rastro.
namespace B = zfsmgr::base::daemonpayload;

namespace daemonpayload {

QString unixBinPath() { return QString::fromStdString(B::unixBinPath()); }
QString unixConfigPath() { return QString::fromStdString(B::unixConfigPath()); }
QString macPlistPath() { return QString::fromStdString(B::macPlistPath()); }
QString linuxServicePath() { return QString::fromStdString(B::linuxServicePath()); }
QString freeBsdRcPath() { return QString::fromStdString(B::freeBsdRcPath()); }
QString windowsDirPath() { return QString::fromStdString(B::windowsDirPath()); }
QString windowsTaskName() { return QString::fromStdString(B::windowsTaskName()); }
QString tlsDirPath() { return QString::fromStdString(B::tlsDirPath()); }
QString tlsServerCertPath() { return QString::fromStdString(B::tlsServerCertPath()); }
QString tlsServerKeyPath() { return QString::fromStdString(B::tlsServerKeyPath()); }
QString tlsClientCertPath() { return QString::fromStdString(B::tlsClientCertPath()); }
QString tlsClientKeyPath() { return QString::fromStdString(B::tlsClientKeyPath()); }
QString windowsBinPath() { return QString::fromStdString(B::windowsBinPath()); }
QString windowsUploadPath() { return QString::fromStdString(B::windowsUploadPath()); }
QString windowsNativeInstallCommand() { return QString::fromStdString(B::windowsNativeInstallCommand()); }
QString macLaunchdPlist() { return QString::fromStdString(B::macLaunchdPlist()); }
QString freeBsdRcScript() { return QString::fromStdString(B::freeBsdRcScript()); }
QString linuxSystemdService() { return QString::fromStdString(B::linuxSystemdService()); }
QString tlsBootstrapShellCommand() { return QString::fromStdString(B::tlsBootstrapShellCommand()); }
QString simpleConfigPayload(const QString& version, const QString& apiVersion) {
    return QString::fromStdString(B::simpleConfigPayload(version.toStdString(), apiVersion.toStdString()));
}

} // namespace daemonpayload
