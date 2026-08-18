#include "daemonpayload.h"

#include "strutil.h"

namespace zfsmgr::base::daemonpayload {

std::string unixBinPath() { return std::string("/usr/local/libexec/zfsmgr-agent"); }
std::string unixConfigPath() { return std::string("/etc/zfsmgr/agent.conf"); }
std::string macPlistPath() { return std::string("/Library/LaunchDaemons/org.zfsmgr.agent.plist"); }
std::string linuxServicePath() { return std::string("/etc/systemd/system/zfsmgr-agent.service"); }
std::string freeBsdRcPath() { return std::string("/usr/local/etc/rc.d/zfsmgr_agent"); }
std::string windowsDirPath() { return std::string("C:\\ProgramData\\ZFSMgr\\agent"); }
std::string windowsTaskName() { return std::string("ZFSMgr-Agent"); }
std::string windowsBinPath() { return std::string("C:\\ProgramData\\ZFSMgr\\agent\\zfsmgr-agent.exe"); }
// Ruta intermedia del scp: el destino final puede estar en uso por el agente en
// marcha, así que se sube aparte y se mueve tras pararlo.
std::string windowsUploadPath() { return std::string("C:/Users/Public/zfsmgr-agent.upload"); }
std::string tlsDirPath() { return std::string("/etc/zfsmgr/tls"); }
std::string tlsServerCertPath() { return std::string("/etc/zfsmgr/tls/server.crt"); }
std::string tlsServerKeyPath() { return std::string("/etc/zfsmgr/tls/server.key"); }
std::string tlsClientCertPath() { return std::string("/etc/zfsmgr/tls/client.crt"); }
std::string tlsClientKeyPath() { return std::string("/etc/zfsmgr/tls/client.key"); }
std::string defaultAgentPath() {
    return std::string("/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin");
}

std::string unixStubScript(const std::string& version, const std::string& apiVersion) {
    std::string daemonScript = std::string(R"SH(
#!/bin/sh
# ZFSMgr Agent Version: __VERSION__
# ZFSMgr Agent API: __API__
set -eu

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

run_allow_batch_payload() {
  payload="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available for allow-batch payload" >&2
    return 127
  fi
  python3 - "$payload" <<'PY'
import base64, json, subprocess, sys
payload = sys.argv[1]
try:
    items = json.loads(base64.b64decode(payload))
except Exception:
    print("invalid allow-batch payload", file=sys.stderr)
    sys.exit(2)
if not isinstance(items, list) or not items or not all(isinstance(v, str) for v in items):
    print("invalid allow-batch payload", file=sys.stderr)
    sys.exit(2)
batch = []
for it in items:
    try:
        argv = json.loads(base64.b64decode(it))
    except Exception:
        print("invalid allow-batch entry", file=sys.stderr)
        sys.exit(2)
    if not isinstance(argv, list) or not argv or not all(isinstance(v, str) for v in argv):
        print("invalid allow-batch entry", file=sys.stderr)
        sys.exit(2)
    if argv[0].strip().lower() not in ("allow", "unallow"):
        print("unsupported allow-batch op: " + argv[0], file=sys.stderr)
        sys.exit(2)
    batch.append(argv)
rc = 0
for argv in batch:
    sub = subprocess.call(["zfs"] + argv)
    if sub != 0 and rc == 0:
        rc = sub
sys.exit(rc)
PY
}

run_pipe_local_payload() {
  payload="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available for pipe-local payload" >&2
    return 127
  fi
  python3 - "$payload" <<'PY'
import base64, json, subprocess, sys
payload = sys.argv[1]
try:
    items = json.loads(base64.b64decode(payload))
except Exception:
    print("invalid pipe-local payload", file=sys.stderr)
    sys.exit(2)
if not isinstance(items, list) or len(items) != 2 or not all(isinstance(v, str) for v in items):
    print("invalid pipe-local payload", file=sys.stderr)
    sys.exit(2)
def decode_one(b64, expect):
    try:
        argv = json.loads(base64.b64decode(b64))
    except Exception:
        print("invalid pipe-local entry", file=sys.stderr)
        sys.exit(2)
    if not isinstance(argv, list) or not argv or not all(isinstance(v, str) for v in argv):
        print("invalid pipe-local entry", file=sys.stderr)
        sys.exit(2)
    if argv[0].strip().lower() != expect:
        print("pipe-local entry must start with " + expect, file=sys.stderr)
        sys.exit(2)
    return argv
send_argv = decode_one(items[0], "send")
recv_argv = decode_one(items[1], "recv")
p1 = subprocess.Popen(["zfs"] + send_argv, stdout=subprocess.PIPE)
p2 = subprocess.Popen(["zfs"] + recv_argv, stdin=p1.stdout)
p1.stdout.close()
p2.wait()
p1.wait()
sys.exit(p2.returncode if p2.returncode != 0 else p1.returncode)
PY
}

run_rsync_local_payload() {
  payload="$1"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available for rsync payload" >&2
    return 127
  fi
  python3 - "$payload" <<'PY'
import base64, json, subprocess, sys
try:
    f = json.loads(base64.b64decode(sys.argv[1]))
except Exception:
    print("invalid rsync payload", file=sys.stderr)
    sys.exit(2)
if (not isinstance(f, list) or len(f) < 6 or (len(f) - 4) % 2 != 0
        or not all(isinstance(v, str) for v in f)):
    print("invalid rsync payload", file=sys.stderr)
    sys.exit(2)
del_flag, dry_flag, rsh, dst_host = f[0].strip(), f[1].strip(), f[2], f[3].strip()
if del_flag not in ("0", "1") or dry_flag not in ("0", "1"):
    print("invalid rsync flag", file=sys.stderr)
    sys.exit(2)
pairs = []
for i in range(4, len(f), 2):
    s, d = f[i].strip(), f[i + 1].strip()
    if not s or not d or not s.startswith("/") or not d.startswith("/"):
        print("rsync paths must be absolute", file=sys.stderr)
        sys.exit(2)
    pairs.append((s, d))
if not pairs:
    print("invalid rsync payload", file=sys.stderr)
    sys.exit(2)
def probe(args):
    try:
        return subprocess.run(["rsync"] + args, capture_output=True).returncode == 0
    except Exception:
        return False
try:
    h = subprocess.run(["rsync", "--help"], capture_output=True)
    help_text = (h.stdout or b"").decode("utf-8", "replace") + (h.stderr or b"").decode("utf-8", "replace")
except Exception:
    help_text = ""
argv = ["-aHWS"]
if del_flag == "1":
    argv.append("--delete")
if dry_flag == "1":
    argv.append("--dry-run")
if probe(["-A", "--version"]):
    argv.append("-A")
if probe(["-X", "--version"]):
    argv.append("-X")
elif "--extended-attributes" in help_text:
    argv.append("--extended-attributes")
argv.append("--info=progress2" if "--info" in help_text else "--progress")
if dst_host and rsh:
    argv += ["-e", rsh]
for s, d in pairs:
    full = argv + [s + "/", (dst_host + ":" + d + "/") if dst_host else (d + "/")]
    rc = subprocess.call(["rsync"] + full)
    if rc != 0:
        sys.exit(rc)
sys.exit(0)
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
    exec zfs --version
    ;;
  --dump-zfs-mount)
    exec zfs mount -H
    ;;
  --dump-zpool-list)
    exec zpool list -j
    ;;
  --dump-zpool-import-probe)
    (zpool import || true; zpool import -s || true)
    ;;
  --dump-zpool-guid-status-batch)
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
    exec zfs list -H -p -t filesystem,volume,snapshot -o name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,canmount -r "$2"
    ;;
  --dump-zfs-guid-map)
    exec zfs list -H -o name,guid -r "$2"
    ;;
  --dump-zfs-list-children)
    exec zfs list -H -o name -r "$2"
    ;;
  --dump-advanced-breakdown-list)
    ds="${2:-}"
    [ -n "$ds" ] || exit 2
    mp="$(zfs mount 2>/dev/null | awk -v d="$ds" '$1==d{print $2; exit}')"
    [ -n "$mp" ] || exit 0
    [ -d "$mp" ] || exit 0
    printf '__MP__=%s\n' "$mp"
    find "$mp" -mindepth 1 -type d -print 2>/dev/null | while IFS= read -r d; do
      rel="$d"
      case "$rel" in
        "$mp"/*) rel=${rel#"$mp"/} ;;
        *) rel='' ;;
      esac
      [ -n "$rel" ] || continue
      case "$rel" in
        .zfs|.zfs/*) continue ;;
      esac
      printf '%s\n' "$rel"
    done | sort -u
    exit 0
    ;;
  --dump-zfs-get-prop)
    exec zfs get -H -o value "$2" "$3"
    ;;
  --dump-zfs-get-all)
    exec zfs get -j all "$2"
    ;;
  --dump-zfs-get-json)
    exec zfs get -j "$2" "$3"
    ;;
  --dump-zfs-get-gsa-raw-all-pools)
    zpool list -H -o name 2>/dev/null | while IFS= read -r pool; do
      [ -z "$pool" ] && continue
      zfs get -H -o name,property,value,source -r org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino "$pool" 2>/dev/null || true
    done
    ;;
  --dump-zfs-get-gsa-raw-recursive)
    exec zfs get -H -o name,property,value,source -r org.fc16.gsa:activado,org.fc16.gsa:recursivo,org.fc16.gsa:horario,org.fc16.gsa:diario,org.fc16.gsa:semanal,org.fc16.gsa:mensual,org.fc16.gsa:anual,org.fc16.gsa:nivelar,org.fc16.gsa:destino "$2"
    ;;
  --wait-for-event)
    # Block until a ZED event arrives or timeout expires.
    # zpool events -f -H follows the event stream; we read one line and exit.
    timeout="${2:-30}"
    line="$(command -v timeout >/dev/null 2>&1 \
      && timeout "$timeout" sh -c 'zpool events -f -H 2>/dev/null | head -1' \
      || zpool events -f -H 2>/dev/null | head -1)"
    if [ -n "$line" ]; then
      printf 'EVENT_TYPE=zed\n'; exit 0
    fi
    printf 'TIMEOUT=1\n'; exit 1
    ;;
  --dump-zfs-exists)
    zfs list -H -o name "$2" >/dev/null 2>&1 && echo "EXISTS=yes" || { echo "EXISTS=no"; exit 1; }
    ;;
  --mutate-zfs-clone)
    exec zfs clone "$2" "$3"
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
    DATASET="${2:-}"
    shift 2 || true
    [ -n "$DATASET" ] || exit 2
    MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
    [ -n "$MP" ] || { echo 'mountpoint=none'; exit 2; }
    while [ $# -ge 2 ]; do
      rel="$1"; name="$2"; shift 2
      [ -n "$rel" ] || continue
      [ -n "$name" ] || exit 2
      SRC="$MP/$rel"
      [ -d "$SRC" ] || continue
      # Este respaldo borra el original con rm -rf, que no distingue puntos de montaje.
      # El agente nativo desmonta antes y remonta despues; aqui no, asi que se aborta.
      if zfs mount 2>/dev/null | awk -v p="$SRC/" 'index($2,p)==1{f=1} END{exit !f}'; then
        echo "hay datasets montados dentro de $rel; use el agente nativo" >&2
        exit 1
      fi
      CHILD="$DATASET/$name"
      if zfs list -H -o name "$CHILD" >/dev/null 2>&1; then
        echo "child_exists=$CHILD"
        continue
      fi
      TMP_CHILD_MP="$(mktemp -d /tmp/zfsmgr-breakdown-child-XXXXXX)"
      zfs create -p -o mountpoint="$TMP_CHILD_MP" "$CHILD"
      zfs mount "$CHILD" >/dev/null 2>&1 || true
      rsync -aHWS "$SRC"/ "$TMP_CHILD_MP"/
      rm -rf "$SRC"
      zfs set mountpoint="$SRC" "$CHILD"
      zfs mount "$CHILD" >/dev/null 2>&1 || true
      rm -rf "$TMP_CHILD_MP" >/dev/null 2>&1 || true
      echo "[BREAKDOWN] ok $rel -> $CHILD"
    done
    exit 0
    ;;
  --mutate-advanced-assemble)
    DATASET="${2:-}"
    shift 2 || true
    [ -n "$DATASET" ] || exit 2
    PARENT_MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
    [ -n "$PARENT_MP" ] || { echo 'mountpoint=none'; exit 2; }
    for child in "$@"; do
      [ -n "$child" ] || continue
      zfs mount "$child" >/dev/null 2>&1 || true
      CMP="$(zfs mount 2>/dev/null | awk -v d="$child" '$1==d{print $2;exit}')"
      [ -n "$CMP" ] || continue
      BN="$(basename "$child")"
      [ -n "$BN" ] || continue
      TMP="$(mktemp -d /tmp/zfsmgr-assemble-XXXXXX)"
      rsync -aHWS "$CMP"/ "$TMP"/
      zfs destroy -r "$child"
      mkdir -p "$PARENT_MP/$BN"
      rsync -aHWS "$TMP"/ "$PARENT_MP/$BN"/
      rm -rf "$TMP" >/dev/null 2>&1 || true
      echo "[ASSEMBLE] ok $child -> $PARENT_MP/$BN"
    done
    exit 0
    ;;
  --repair-alt-mountpoints)
    APPLY="${2:-}"
    SAVED_PROP='org.fc16.zfsmgr:savedmountpoint'
    stranded=0; repaired=0; failed=0
    while IFS="$(printf '\t')" read -r ds saved; do
      [ -n "$ds" ] || continue
      cur="$(zfs get -H -o value mountpoint "$ds" 2>/dev/null || true)"
      printf 'STRANDED=%s saved=%s current=%s\n' "$ds" "$saved" "$cur"
      stranded=$((stranded+1))
      [ "$APPLY" = 'apply' ] || continue
      zfs unmount "$ds" >/dev/null 2>&1 || true
      ok=1
      if [ -n "$saved" ] && [ "$saved" != '-' ]; then
        zfs set mountpoint="$saved" "$ds" >/dev/null 2>&1 || ok=0
      fi
      if [ "$ok" = "1" ]; then
        zfs inherit "$SAVED_PROP" "$ds" >/dev/null 2>&1 || true
        printf 'REPAIRED=%s -> %s\n' "$ds" "$saved"
        repaired=$((repaired+1))
      else
        echo "cannot restore mountpoint for $ds" >&2
        failed=$((failed+1))
      fi
    done <<EOF_STRANDED
$(zfs get -H -o name,value -s local "$SAVED_PROP" 2>/dev/null || true)
EOF_STRANDED
    printf 'STRANDED_COUNT=%s\nREPAIRED_COUNT=%s\nFAILED_COUNT=%s\n' "$stranded" "$repaired" "$failed"
    [ "$failed" = "0" ] || exit 1
    exit 0
    ;;
  --mutate-sync-temp-tar-source|--mutate-sync-temp-tar-dest)
    MODE="$1"
    DATASET="${2:-}"
    CODEC="${3:-none}"
    [ -n "$DATASET" ] || exit 2
    case "$CODEC" in none|zstd|gzip) ;; *) echo 'invalid codec' >&2; exit 2 ;; esac
    SAVED_PROP='org.fc16.zfsmgr:savedmountpoint'
    TMP_MP=''
    cleanup() {
      [ -n "$TMP_MP" ] || return 0
      zfs unmount "$DATASET" >/dev/null 2>&1 || true
      saved_mp="$(zfs get -H -o value "$SAVED_PROP" "$DATASET" 2>/dev/null || true)"
      if [ -n "$saved_mp" ] && [ "$saved_mp" != "-" ]; then
        zfs set mountpoint="$saved_mp" "$DATASET" >/dev/null 2>&1 || true
      fi
      zfs inherit "$SAVED_PROP" "$DATASET" >/dev/null 2>&1 || true
      rmdir "$TMP_MP" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT INT TERM
    MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
    if [ -z "$MP" ]; then
      TMP_MP="$(mktemp -d /tmp/zfsmgr-sync-XXXXXX)"
      cur_mp="$(zfs get -H -o value mountpoint "$DATASET" 2>/dev/null || true)"
      zfs set "$SAVED_PROP=$cur_mp" "$DATASET"
      zfs set mountpoint="$TMP_MP" "$DATASET"
      zfs mount "$DATASET"
      MP="$TMP_MP"
    fi
    [ -n "$MP" ] || { echo 'could not resolve a usable mountpoint' >&2; exit 41; }
    if [ "$MODE" = '--mutate-sync-temp-tar-source' ]; then
      [ -d "$MP" ] || exit 41
      case "$CODEC" in
        zstd) tar --acls --xattrs -cpf - -C "$MP" . | zstd -1 -T0 -q -c ;;
        gzip) tar --acls --xattrs -cpf - -C "$MP" . | gzip -1 -c ;;
        *)    tar --acls --xattrs -cpf - -C "$MP" . ;;
      esac
    else
      mkdir -p "$MP"
      case "$CODEC" in
        zstd) zstd -d -q -c - | tar --acls --xattrs -xpf - -C "$MP" ;;
        gzip) gzip -d -c - | tar --acls --xattrs -xpf - -C "$MP" ;;
        *)    tar --acls --xattrs -xpf - -C "$MP" ;;
      esac
    fi
    exit $?
    ;;
  --mutate-advanced-fromdir)
    DATASET="${2:-}"
    REL="${3:-}"
    [ -n "$DATASET" ] || exit 2
    case "$REL" in
      /*|*..*) echo 'invalid relative subdirectory' >&2; exit 2 ;;
    esac
    zfs set canmount=on "$DATASET" >/dev/null 2>&1 || true
    zfs mount "$DATASET" >/dev/null 2>&1 || true
    MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
    [ -n "$MP" ] || { echo 'could not resolve effective mountpoint' >&2; exit 4; }
    if [ -n "$REL" ]; then DST="$MP/$REL"; else DST="$MP"; fi
    mkdir -p "$DST"
    echo "[FROMDIR] tar recv -> $DST"
    exec tar --acls --xattrs -xpf - -C "$DST"
    ;;
  --mutate-advanced-todir)
    DATASET="${2:-}"
    DST_DIR="${3:-}"
    DELETE_SRC="${4:-0}"
    [ -n "$DATASET" ] || exit 2
    [ -n "$DST_DIR" ] || exit 2
    zfs mount "$DATASET" >/dev/null 2>&1 || true
    SRC_MP="$(zfs mount 2>/dev/null | awk -v d="$DATASET" '$1==d{print $2;exit}')"
    [ -n "$SRC_MP" ] || { echo 'mountpoint=none'; exit 2; }
    mkdir -p "$DST_DIR"
    rsync -aHWS "$SRC_MP"/ "$DST_DIR"/
    if [ "$DELETE_SRC" = "1" ]; then
      zfs destroy -r "$DATASET"
    fi
    echo "[TODIR] ok"
    exit 0
    ;;
  --mutate-zpool-generic)
    run_generic_payload zpool "$2"
    ;;
  --mutate-zfs-allow-batch)
    run_allow_batch_payload "$2"
    ;;
  --zfs-pipe-local)
    run_pipe_local_payload "$2"
    ;;
  --mutate-rsync-local)
    run_rsync_local_payload "$2"
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
    replaceAll(daemonScript, std::string("__VERSION__"), trim(version));
    replaceAll(daemonScript, std::string("__API__"), trim(apiVersion));
    return daemonScript;
}

// Instalación del daemon NATIVO en Windows, en sustitución del stub de PowerShell.
//
// El binario llega en base64 por la entrada estándar: mandarlo en la propia línea de
// comandos no cabe (son ~12 MB codificados) y enviarlo en crudo por stdin se topa
// con la conversión de codificación de PowerShell. Se enlaza estáticamente, así que
// no hay que llevar ninguna DLL al lado.
//
// Después se ejecuta --ensure-tls, que genera el material TLS con el OpenSSL que el
// propio agente enlaza: en Windows no hay openssl en el PATH (solo aparece si está
// Git instalado, que no es garantía), así que el bootstrap por shell no sirve aquí.
std::string windowsNativeInstallCommand() {
    // El binario ya está en $tmpUpload, subido por scp ANTES de ejecutar esto.
    //
    // Antes viajaba en base64 por la entrada estándar, y no funcionaba: PowerShell no
    // devuelve de [Console]::In.ReadToEnd() con volúmenes de megabytes, así que la
    // instalación se colgaba hasta agotar el plazo. Falla ya alrededor de 1 MB y el
    // agente son 9,3 MB.
    //
    // Después se ejecuta --ensure-tls, que genera el material TLS con el OpenSSL que
    // el propio agente enlaza: en Windows no hay openssl en el PATH (solo aparece si
    // está Git instalado, que no es garantía).
    return format(std::string(
        // SIN $ErrorActionPreference='Stop': con él, cualquier salida por stderr de un
        // comando nativo se convierte en excepción, y schtasks /End y /Delete escriben
        // ahí cuando la tarea todavía no existe —que es el caso normal en la primera
        // instalación—. Se comprueban explícitamente los pasos que sí importan.
        "$dir='%1'; $bin='%2'; $task='%3'; $up='%4'; "
        "if (-not (Test-Path -LiteralPath $up)) { Write-Error 'no llegó el binario'; exit 1 } "
        "New-Item -ItemType Directory -Force -Path $dir | Out-Null; "
        "cmd /c \"schtasks /End /TN $task >nul 2>&1\" | Out-Null; "
        "cmd /c \"schtasks /Delete /F /TN $task >nul 2>&1\" | Out-Null; "
        "Get-Process zfsmgr-agent,zfsmgr_agent -ErrorAction SilentlyContinue | Stop-Process -Force; "
        "Start-Sleep -Milliseconds 500; "
        "Move-Item -Force -LiteralPath $up -Destination $bin; "
        "if (-not (Test-Path -LiteralPath $bin)) { Write-Output 'ZFSMGR_WIN_AGENT_MOVE_FAIL'; exit 1 } "
        "& $bin --ensure-tls | Out-Null; "
        "if ($LASTEXITCODE -ne 0) { Write-Output 'ZFSMGR_WIN_AGENT_TLS_FAIL'; exit 1 } "
        // [char]34 en vez de comillas escapadas: este texto atraviesa el literal de
        // C++, el envoltorio de shell y el -Command de PowerShell, y en cada capa se
        // reinterpretan los escapes. Así no depende de ninguna.
        // Regla de cortafuegos para el propio binario.
        //
        // Hace falta para RECIBIR una transferencia: el agente abre un puerto efímero y
        // el emisor se conecta a él. Sin regla, Windows rechaza esa conexión y la copia
        // expira sin explicación —comprobado: el cortafuegos viene activo en los tres
        // perfiles y sin ninguna regla nuestra—.
        //
        // Va acotada al PROGRAMA, no a un puerto: los puertos son efímeros y distintos
        // en cada transferencia, así que abrir un rango sería peor y menos preciso.
        // Se borra y se recrea para que apunte al binario actual si cambió de sitio, y
        // no interrumpe la instalación si falla: sin ella el agente sirve igual para
        // todo lo demás.
        "Remove-NetFirewallRule -Name 'ZFSMgr-Agent' -ErrorAction SilentlyContinue; "
        "New-NetFirewallRule -Name 'ZFSMgr-Agent' -DisplayName 'ZFSMgr Agent (transferencias ZFS)' "
        "-Direction Inbound -Action Allow -Program $bin -Profile Any "
        "-ErrorAction SilentlyContinue | Out-Null; "
        "$q=[char]34; $action=$q + $bin + $q + ' --serve'; "
        "schtasks /Create /SC ONSTART /RL HIGHEST /RU SYSTEM /TN $task /TR $action /F | Out-Null; "
        "schtasks /Run /TN $task | Out-Null; "
        "Write-Output 'ZFSMGR_WIN_AGENT_OK'; exit 0"),
        {windowsDirPath(), windowsBinPath(), windowsTaskName(), windowsUploadPath()});
}

std::string macLaunchdPlist() {
    return std::string(
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

std::string freeBsdRcScript() {
    return std::string(
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

std::string linuxSystemdService() {
    return std::string(
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

std::string simpleConfigPayload(const std::string& version, const std::string& apiVersion) {
    return format(std::string(
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
               "TLS_KEY=%13\n"),
        {shSingleQuote(trim(version)),
         shSingleQuote(trim(apiVersion)),
         shSingleQuote(std::string("127.0.0.1")),
         shSingleQuote(std::string("47653")),
         shSingleQuote(defaultAgentPath()),
         shSingleQuote(std::string("2000")),
         shSingleQuote(std::string("8000")),
         shSingleQuote(std::string("512")),
         shSingleQuote(std::string("60000")),
         shSingleQuote(std::string("1")),
         shSingleQuote(tlsDirPath()),
         shSingleQuote(tlsServerCertPath()),
         shSingleQuote(tlsServerKeyPath())})
        + format(std::string("TLS_CLIENT_CERT=%1\nTLS_CLIENT_KEY=%2\n"),
                 {shSingleQuote(tlsClientCertPath()),
                  shSingleQuote(tlsClientKeyPath())});
}

std::string tlsBootstrapShellCommand() {
    // Los certificados DEBEN llevar subjectAltName. Emitirlos solo con CN funciona
    // con el backend OpenSSL de Qt, que aún acepta el CN como nombre de host, pero
    // no con el SecureTransport de Apple, que sigue el RFC 6125 y lo ignora por
    // completo. Con certificados sin SAN, la aplicación de macOS no podía hablar
    // con NINGÚN daemon: "The host name did not match any of the valid hosts for
    // this certificate". Se incluyen los dos nombres que prueba el cliente y la
    // IP de loopback, que es a donde apunta el túnel SSH.
    //
    // La condición de regeneración comprueba además que el certificado existente
    // TENGA SAN: si no, se rehace. Sin eso, los hosts ya aprovisionados se
    // quedarían con el certificado viejo para siempre, porque el fichero existe y
    // no está vacío.
    // Con SAN el handshake dejó de fallar por nombre de host y pasó a fallar por
    // "The root CA certificate is not trusted for this purpose", que es el
    // InvalidPurpose de Qt: al certificado le faltaba extendedKeyUsage. Cada uno
    // hace de ancla y de hoja a la vez, así que el del servidor declara serverAuth
    // y el del cliente clientAuth. keyUsage lleva keyCertSign porque ambos son
    // CA:TRUE de sí mismos, más lo que TLS necesita de la hoja.
    const std::string san = std::string(
        "subjectAltName=DNS:zfsmgr-agent-server,DNS:zfsmgr-agent,DNS:localhost,IP:127.0.0.1");
    const std::string keyUse = std::string(
        "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign");
    return format(std::string(
        "mkdir -p %1; "
        "_zfsmgr_needs_san() { "
        "  [ -s \"$1\" ] || return 0; "
        "  openssl x509 -in \"$1\" -noout -ext subjectAltName 2>/dev/null | grep -q 'DNS:' || return 0; "
        "  return 1; "
        "}; "
        "if [ ! -s %2 ] || [ ! -s %3 ] || _zfsmgr_needs_san %2; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-server' -addext '%6' -addext '%7' "
        "      -addext 'extendedKeyUsage=serverAuth' "
        "      -keyout %3 -out %2 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "if [ ! -s %4 ] || [ ! -s %5 ] || _zfsmgr_needs_san %4; then "
        "  if command -v openssl >/dev/null 2>&1; then "
        "    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 3650 "
        "      -subj '/CN=zfsmgr-agent-client' -addext '%6' -addext '%7' "
        "      -addext 'extendedKeyUsage=clientAuth' "
        "      -keyout %5 -out %4 >/dev/null 2>&1 || true; "
        "  fi; "
        "fi; "
        "touch %2 %3 %4 %5; "
        "chmod 600 %2 %3 %4 %5"),
        {tlsDirPath(),
         tlsServerCertPath(),
         tlsServerKeyPath(),
         tlsClientCertPath(),
         tlsClientKeyPath(),
         san,
         keyUse});
}

}  // namespace zfsmgr::base::daemonpayload
