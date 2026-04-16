#pragma once

#include <QString>

namespace daemonpayload {

QString unixBinPath();
QString unixConfigPath();
QString macPlistPath();
QString linuxServicePath();
QString freeBsdRcPath();
QString windowsDirPath();
QString windowsScriptPath();
QString windowsTaskName();
QString tlsDirPath();
QString tlsServerCertPath();
QString tlsServerKeyPath();
QString tlsClientCertPath();
QString tlsClientKeyPath();

QString unixStubScript(const QString& version, const QString& apiVersion);
QString windowsStubScript(const QString& version, const QString& apiVersion);
QString macLaunchdPlist();
QString freeBsdRcScript();
QString linuxSystemdService();
QString simpleConfigPayload(const QString& version, const QString& apiVersion);
QString tlsBootstrapShellCommand();

} // namespace daemonpayload
