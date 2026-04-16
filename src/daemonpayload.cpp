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
QString tlsClientCertPath() { return QStringLiteral("/etc/zfsmgr/tls/client.crt"); }
QString tlsClientKeyPath() { return QStringLiteral("/etc/zfsmgr/tls/client.key"); }

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
               "AGENT_BIND=%3\n"
               "AGENT_PORT=%4\n"
               "CACHE_TTL_FAST_MS=%5\n"
               "CACHE_TTL_SLOW_MS=%6\n"
               "CACHE_MAX_ENTRIES=%7\n"
               "RECONCILE_INTERVAL_MS=%8\n"
               "ZED_EVENTS_ENABLED=%9\n"
               "TLS_DIR=%10\n"
               "TLS_CERT=%11\n"
               "TLS_KEY=%12\n")
        .arg(mwhelpers::shSingleQuote(version.trimmed()),
             mwhelpers::shSingleQuote(apiVersion.trimmed()),
             mwhelpers::shSingleQuote(QStringLiteral("127.0.0.1")),
             mwhelpers::shSingleQuote(QStringLiteral("47653")),
             mwhelpers::shSingleQuote(QStringLiteral("2000")),
             mwhelpers::shSingleQuote(QStringLiteral("8000")),
             mwhelpers::shSingleQuote(QStringLiteral("512")),
             mwhelpers::shSingleQuote(QStringLiteral("60000")),
             mwhelpers::shSingleQuote(QStringLiteral("1")),
             mwhelpers::shSingleQuote(tlsDirPath()),
             mwhelpers::shSingleQuote(tlsServerCertPath()),
             mwhelpers::shSingleQuote(tlsServerKeyPath()))
        + QStringLiteral("TLS_CLIENT_CERT=%1\nTLS_CLIENT_KEY=%2\n")
              .arg(mwhelpers::shSingleQuote(tlsClientCertPath()),
                   mwhelpers::shSingleQuote(tlsClientKeyPath()));
}

QString tlsBootstrapShellCommand() {
    return QStringLiteral(
        "mkdir -p %1; "
        "if [ ! -s %2 ] || [ ! -s %3 ]; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-server' "
        "      -keyout %3 -out %2 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "if [ ! -s %4 ] || [ ! -s %5 ]; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-client' "
        "      -keyout %5 -out %4 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "touch %2 %3 %4 %5; "
        "chmod 600 %2 %3 %4 %5")
        .arg(tlsDirPath(),
             tlsServerCertPath(),
             tlsServerKeyPath(),
             tlsClientCertPath(),
             tlsClientKeyPath());
}

} // namespace daemonpayload
