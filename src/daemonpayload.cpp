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
QString defaultAgentPath() {
    return QStringLiteral("/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin");
}

QString unixStubScript(const QString& version, const QString& apiVersion) {
    QString daemonScript = QString::fromUtf8(R"SH(
#!/bin/sh
# ZFSMgr Agent Version: __VERSION__
# ZFSMgr Agent API: __API__
set -eu

find_helper() {
  helper="$1"
  for d in "/home/${SUDO_USER:-}/.config/ZFSMgr/bin" "/Users/${SUDO_USER:-}/.config/ZFSMgr/bin" "$HOME/.config/ZFSMgr/bin"; do
    [ -x "$d/$helper" ] && { printf '%s\n' "$d/$helper"; return 0; }
  done
  return 1
}

run_generic_payload() {
  tool="$1"
  payload="$2"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available for generic mutation payload" >&2
    return 127
  fi
  python3 - "$tool" "$payload" <<'PY'
import base64, json, subprocess, sys
tool = sys.argv[1]
payload = sys.argv[2]
allowed_zfs = {
    "create","destroy","rollback","clone","rename","set","inherit",
    "mount","unmount","hold","release","load-key","unload-key","change-key","promote"
}
allowed_zpool = {
    "create","destroy","add","remove","attach","detach","replace",
    "offline","online","clear","export","import","scrub","trim",
    "initialize","sync","upgrade","reguid","split","checkpoint"
}
try:
    arr = json.loads(base64.b64decode(payload))
except Exception:
    print("invalid generic payload", file=sys.stderr)
    sys.exit(2)
if not isinstance(arr, list) or not arr or not all(isinstance(v, str) for v in arr):
    print("invalid generic payload", file=sys.stderr)
    sys.exit(2)
op = arr[0].strip().lower()
if tool == "zfs":
    if op not in allowed_zfs:
        print("unsupported zfs mutation op", file=sys.stderr)
        sys.exit(2)
elif tool == "zpool":
    if op not in allowed_zpool:
        print("unsupported zpool mutation op", file=sys.stderr)
        sys.exit(2)
else:
    print("unsupported mutation tool", file=sys.stderr)
    sys.exit(2)
sys.exit(subprocess.call([tool] + arr))
PY
}

run_shell_payload() {
  /usr/bin/env python3 - "$1" <<'PY'
import base64, subprocess, sys
if len(sys.argv) < 2:
    print("invalid shell payload", file=sys.stderr)
    sys.exit(2)
try:
    script = base64.b64decode(sys.argv[1]).decode("utf-8", "strict").strip()
except Exception:
    print("invalid shell payload", file=sys.stderr)
    sys.exit(2)
if not script:
    print("empty shell payload", file=sys.stderr)
    sys.exit(2)
sys.exit(subprocess.call(["sh", "-lc", script]))
PY
}

cmd="${1:---serve}"
case "$cmd" in
  --version|version) printf '%s\n' '__VERSION__'; exit 0 ;;
  --api-version|api) printf '%s\n' '__API__'; exit 0 ;;
  --serve|serve) while :; do sleep 3600; done ;;
  --health)
    printf 'STATUS=OK\nSERVER=1\nCACHE_ENTRIES=0\nCACHE_MAX_ENTRIES=0\nCACHE_INVALIDATIONS=0\nPOOL_INVALIDATIONS=0\nRPC_FAILURES=0\nRPC_COMMANDS=\nZED_ACTIVE=0\n'
    ;;
  --dump-refresh-basics)
    if h="$(find_helper zfsmgr-refresh-basics 2>/dev/null)"; then exec "$h"; fi
    os_line="$(uname -s 2>/dev/null) $(uname -r 2>/dev/null)"
    if [ -r /etc/os-release ]; then
      os_line="$(. /etc/os-release 2>/dev/null; printf '%s %s' "${NAME:-$(uname -s)}" "${VERSION_ID:-}")"
    fi
    machine_uuid="$(cat /etc/machine-id 2>/dev/null | head -n1 || true)"
    [ -z "$machine_uuid" ] && machine_uuid="$(cat /var/lib/dbus/machine-id 2>/dev/null | head -n1 || true)"
    zraw="$(zfs --version 2>&1 | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g' | sed 's/[[:space:]]$//')"
    zsem="$(printf '%s\n' "$zraw" | sed -n 's/.*\([0-9][0-9]*\.[0-9][0-9]*\(\.[0-9][0-9]*\)\{0,1\}\).*/\1/p' | head -n1)"
    printf 'OS_LINE=%s\nMACHINE_UUID=%s\nZFS_VERSION_RAW=%s\nZFS_VERSION_SEMVER=%s\n' "$os_line" "$machine_uuid" "$zraw" "$zsem"
    ;;
  --dump-zfs-version)
    if h="$(find_helper zfsmgr-zfs-version 2>/dev/null)"; then exec "$h"; fi
    exec zfs --version
    ;;
  --dump-zfs-mount)
    if h="$(find_helper zfsmgr-zfs-mount-list 2>/dev/null)"; then exec "$h"; fi
    exec zfs mount -H
    ;;
  --dump-zpool-list)
    if h="$(find_helper zfsmgr-zpool-list-json 2>/dev/null)"; then exec "$h"; fi
    exec zpool list -j
    ;;
  --dump-zpool-import-probe)
    if h="$(find_helper zfsmgr-zpool-import-probe 2>/dev/null)"; then exec "$h"; fi
    (zpool import || true; zpool import -s || true)
    ;;
  --dump-zpool-guid-status-batch)
    if h="$(find_helper zfsmgr-zpool-guid-status-all 2>/dev/null)"; then exec "$h"; fi
    zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do
      [ -z "$pool" ] && continue
      guid="$(zpool get -H -o value guid "$pool" 2>/dev/null | head -n1 || true)"
      printf '__ZFSMGR_POOL__:%s\n' "$pool"
      printf '__ZFSMGR_GUID__:%s\n' "$guid"
      zpool status -v "$pool" 2>&1 || true
      printf '__ZFSMGR_END__:%s\n' "$pool"
    done
    ;;
  --dump-zpool-guid)
    exec zpool get -H -o value guid "$2"
    ;;
  --dump-zpool-status)
    exec zpool status -v "$2"
    ;;
  --dump-zpool-status-p)
    exec zpool status -P "$2"
    ;;
  --dump-zpool-history)
    exec zpool history "$2"
    ;;
  --dump-zpool-get-all)
    exec zpool get -j all "$2"
    ;;
  --dump-zfs-list-all)
    if h="$(find_helper zfsmgr-zfs-list-all 2>/dev/null)"; then exec "$h" "$2"; fi
    exec zfs list -H -p -t filesystem,volume,snapshot -o name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r "$2"
    ;;
  --dump-zfs-guid-map)
    if h="$(find_helper zfsmgr-zfs-guid-map 2>/dev/null)"; then exec "$h" "$2"; fi
    exec zfs list -H -o name,guid -r "$2"
    ;;
  --dump-zfs-list-children)
    if h="$(find_helper zfsmgr-zfs-list-children 2>/dev/null)"; then exec "$h" "$2"; fi
    exec zfs list -H -o name -r "$2"
    ;;
  --dump-advanced-breakdown-list)
    if h="$(find_helper zfsmgr-advanced-breakdown-list 2>/dev/null)"; then exec "$h" "$2"; fi
    echo "helper zfsmgr-advanced-breakdown-list not found" >&2
    exit 127
    ;;
  --dump-zfs-get-prop)
    exec zfs get -H -o value "$2" "$3"
    ;;
  --dump-zfs-get-all)
    if h="$(find_helper zfsmgr-zfs-get-all-json 2>/dev/null)"; then exec "$h" "$2"; fi
    exec zfs get -j all "$2"
    ;;
  --dump-zfs-get-json)
    if h="$(find_helper zfsmgr-zfs-get-json 2>/dev/null)"; then exec "$h" "$2" "$3"; fi
    exec zfs get -j "$2" "$3"
    ;;
  --dump-zfs-get-gsa-raw-all-pools)
    if h="$(find_helper zfsmgr-zfs-get-gsa-raw-all-pools 2>/dev/null)"; then exec "$h"; fi
    zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do
      [ -z "$pool" ] && continue
      zfs get -H -o name,property,value,source -r org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino "$pool" 2>/dev/null || true
    done
    ;;
  --dump-zfs-get-gsa-raw-recursive)
    if h="$(find_helper zfsmgr-zfs-get-gsa-raw-recursive 2>/dev/null)"; then exec "$h" "$2"; fi
    exec zfs get -H -o name,property,value,source -r org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino "$2"
    ;;
  --dump-gsa-connections-conf)
    [ -r /etc/zfsmgr/gsa-connections.conf ] && cat /etc/zfsmgr/gsa-connections.conf || true
    ;;
  --mutate-zfs-snapshot)
    [ "${3:-0}" = "1" ] && exec zfs snapshot -r "$2" || exec zfs snapshot "$2"
    ;;
  --mutate-zfs-destroy)
    force=""; rec=""
    [ "${3:-0}" = "1" ] && force="-f"
    [ "${4:-none}" = "R" ] && rec="-R"
    [ "${4:-none}" = "r" ] && rec="-r"
    exec zfs destroy $force $rec "$2"
    ;;
  --mutate-zfs-rollback)
    force=""; rec=""
    [ "${3:-0}" = "1" ] && force="-f"
    [ "${4:-none}" = "R" ] && rec="-R"
    [ "${4:-none}" = "r" ] && rec="-r"
    exec zfs rollback $force $rec "$2"
    ;;
  --mutate-zfs-generic)
    run_generic_payload zfs "$2"
    ;;
  --mutate-advanced-breakdown)
    if h="$(find_helper zfsmgr-advanced-breakdown 2>/dev/null)"; then shift 1; exec "$h" "$@"; fi
    echo "helper zfsmgr-advanced-breakdown not found" >&2
    exit 127
    ;;
  --mutate-advanced-assemble)
    if h="$(find_helper zfsmgr-advanced-assemble 2>/dev/null)"; then shift 1; exec "$h" "$@"; fi
    echo "helper zfsmgr-advanced-assemble not found" >&2
    exit 127
    ;;
  --mutate-advanced-todir)
    if h="$(find_helper zfsmgr-advanced-todir 2>/dev/null)"; then shift 1; exec "$h" "$@"; fi
    echo "helper zfsmgr-advanced-todir not found" >&2
    exit 127
    ;;
  --mutate-zpool-generic)
    run_generic_payload zpool "$2"
    ;;
  --mutate-shell-generic)
    run_shell_payload "$2"
    ;;
  *)
    printf 'usage: %s [--version|--api-version|--serve|--health|--dump-*|--mutate-*]\n' "$0" >&2
    exit 2
    ;;
esac
)SH");
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
               "AGENT_PATH=%5\n"
               "CACHE_TTL_FAST_MS=%6\n"
               "CACHE_TTL_SLOW_MS=%7\n"
               "CACHE_MAX_ENTRIES=%8\n"
               "RECONCILE_INTERVAL_MS=%9\n"
               "ZED_EVENTS_ENABLED=%10\n"
               "TLS_DIR=%11\n"
               "TLS_CERT=%12\n"
               "TLS_KEY=%13\n")
        .arg(mwhelpers::shSingleQuote(version.trimmed()),
             mwhelpers::shSingleQuote(apiVersion.trimmed()),
             mwhelpers::shSingleQuote(QStringLiteral("127.0.0.1")),
             mwhelpers::shSingleQuote(QStringLiteral("47653")),
             mwhelpers::shSingleQuote(defaultAgentPath()),
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
