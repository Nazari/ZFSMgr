#pragma once

#include <QString>

namespace daemonpayload {

QString unixBinPath();
QString unixConfigPath();
QString macPlistPath();
QString linuxServicePath();
QString freeBsdRcPath();
QString windowsDirPath();
QString windowsTaskName();
QString tlsDirPath();
QString tlsServerCertPath();
QString tlsServerKeyPath();
QString tlsClientCertPath();
QString tlsClientKeyPath();

QString unixStubScript(const QString& version, const QString& apiVersion);
QString windowsBinPath();
QString windowsUploadPath();
QString windowsNativeInstallCommand();
QString macLaunchdPlist();
QString freeBsdRcScript();
QString linuxSystemdService();
QString simpleConfigPayload(const QString& version, const QString& apiVersion);
QString tlsBootstrapShellCommand();

} // namespace daemonpayload
