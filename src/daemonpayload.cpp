#include "daemonpayload.h"
#include "mainwindow_helpers.h"

namespace daemonpayload {

QString unixBinPath() { return QStringLiteral("/usr/local/libexec/zfsmgr-agent"); }
QString unixConfigPath() { return QStringLiteral("/etc/zfsmgr/agent.conf"); }
QString macPlistPath() { return QStringLiteral("/Library/LaunchDaemons/org.zfsmgr.agent.plist"); }
QString linuxServicePath() { return QStringLiteral("/etc/systemd/system/zfsmgr-agent.service"); }
QString freeBsdRcPath() { return QStringLiteral("/usr/local/etc/rc.d/zfsmgr_agent"); }
QString windowsDirPath() { return QStringLiteral("C:\\ProgramData\\ZFSMgr\\agent"); }
QString windowsScriptPath() { return QStringLiteral("C:\\ProgramData\\ZFSMgr\\agent\\zfsmgr-agent.ps1"); }
QString windowsTaskName() { return QStringLiteral("ZFSMgr-Agent"); }
QString tlsDirPath() { return QStringLiteral("/etc/zfsmgr/tls"); }
QString tlsServerCertPath() { return QStringLiteral("/etc/zfsmgr/tls/server.crt"); }
QString tlsServerKeyPath() { return QStringLiteral("/etc/zfsmgr/tls/server.key"); }

QString unixStubScript(const QString& version, const QString& apiVersion) {
    QString daemonScript = QString::fromUtf8(
        "#!/bin/sh\n"
        "# ZFSMgr Agent Version: __VERSION__\n"
        "# ZFSMgr Agent API: __API__\n"
        "set -eu\n"
        "case \"${1:-serve}\" in\n"
        "  --version|version) printf '%s\\n' '__VERSION__'; exit 0 ;;\n"
        "  --api-version|api) printf '%s\\n' '__API__'; exit 0 ;;\n"
        "  --serve|serve) while :; do sleep 3600; done ;;\n"
        "  *) printf 'usage: %s [--version|--api-version|--serve]\\n' \"$0\" >&2; exit 2 ;;\n"
        "esac\n");
    daemonScript.replace(QStringLiteral("__VERSION__"), version.trimmed());
    daemonScript.replace(QStringLiteral("__API__"), apiVersion.trimmed());
    return daemonScript;
}

QString windowsStubScript(const QString& version, const QString& apiVersion) {
    QString payload = QString::fromUtf8(
        "# ZFSMgr Agent Version: __VERSION__\n"
        "# ZFSMgr Agent API: __API__\n"
        "param([string]$Mode='serve')\n"
        "if ($Mode -eq 'version') { Write-Output '__VERSION__'; exit 0 }\n"
        "if ($Mode -eq 'api') { Write-Output '__API__'; exit 0 }\n"
        "while ($true) { Start-Sleep -Seconds 3600 }\n");
    payload.replace(QStringLiteral("__VERSION__"), version.trimmed());
    payload.replace(QStringLiteral("__API__"), apiVersion.trimmed());
    return payload;
}

QString macLaunchdPlist() {
    return QString::fromUtf8(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>org.zfsmgr.agent</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>/usr/local/libexec/zfsmgr-agent</string>\n"
        "    <string>--serve</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "</dict>\n"
        "</plist>\n");
}

QString freeBsdRcScript() {
    return QString::fromUtf8(
        "#!/bin/sh\n"
        "# PROVIDE: zfsmgr_agent\n"
        "# REQUIRE: LOGIN\n"
        "# KEYWORD: shutdown\n"
        ". /etc/rc.subr\n"
        "name=\"zfsmgr_agent\"\n"
        "rcvar=zfsmgr_agent_enable\n"
        "pidfile=\"/var/run/${name}.pid\"\n"
        "command=\"/usr/sbin/daemon\"\n"
        "command_args=\"-P ${pidfile} /usr/local/libexec/zfsmgr-agent --serve\"\n"
        "load_rc_config $name\n"
        ": ${zfsmgr_agent_enable:=YES}\n"
        "run_rc_command \"$1\"\n");
}

QString linuxSystemdService() {
    return QString::fromUtf8(
        "[Unit]\n"
        "Description=ZFSMgr native daemon\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/local/libexec/zfsmgr-agent --serve\n"
        "Restart=always\n"
        "RestartSec=5\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n");
}

QString simpleConfigPayload(const QString& version, const QString& apiVersion) {
    return QStringLiteral(
               "VERSION=%1\n"
               "API=%2\n"
               "TLS_DIR=%3\n"
               "TLS_CERT=%4\n"
               "TLS_KEY=%5\n")
        .arg(mwhelpers::shSingleQuote(version.trimmed()),
             mwhelpers::shSingleQuote(apiVersion.trimmed()),
             mwhelpers::shSingleQuote(tlsDirPath()),
             mwhelpers::shSingleQuote(tlsServerCertPath()),
             mwhelpers::shSingleQuote(tlsServerKeyPath()));
}

QString tlsBootstrapShellCommand() {
    return QStringLiteral(
        "mkdir -p %1; "
        "if [ ! -s %2 ] || [ ! -s %3 ]; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent' "
        "      -keyout %3 -out %2 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "touch %2 %3; "
        "chmod 600 %2 %3")
        .arg(tlsDirPath(), tlsServerCertPath(), tlsServerKeyPath());
}

} // namespace daemonpayload
