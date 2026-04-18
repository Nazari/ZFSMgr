#include "gsaversion.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QVector>

namespace {

constexpr const char* kGsaSchemaMarker =
    "unix-script:/usr/local/libexec/zfsmgr-gsa.sh\n"
    "unix-config-dir:/etc/zfsmgr\n"
    "unix-config:/etc/zfsmgr/gsa.conf\n"
    "unix-connections:/etc/zfsmgr/gsa-connections.conf\n"
    "unix-known-hosts:/etc/zfsmgr/gsa_known_hosts\n"
    "mac-plist:/Library/LaunchDaemons/org.zfsmgr.gsa.plist\n"
    "linux-runtime:/var/lib/zfsmgr\n"
    "linux-service:/etc/systemd/system/zfsmgr-gsa.service\n"
    "linux-timer:/etc/systemd/system/zfsmgr-gsa.timer\n";

QVector<int> versionOrderingKey(const QString& version) {
    QVector<int> out;
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:rc(\\d+))?(?:[.-](\\d+))?$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(version.trimmed());
    if (!m.hasMatch()) {
        return out;
    }
    out << m.captured(1).toInt()
        << m.captured(2).toInt()
        << m.captured(3).toInt();
    out << (m.captured(4).isEmpty() ? 999999 : m.captured(4).toInt());
    out << (m.captured(5).isEmpty() ? 0 : m.captured(5).toInt());
    return out;
}

QString versionFingerprintSuffix() {
    QByteArray material;
    material += gsaversion::unixTemplate().toUtf8();
    material += "\n--ZFSMGR-GSA--\n";
    material += gsaversion::windowsTemplate().toUtf8();
    material += "\n--ZFSMGR-GSA--\n";
    material += QByteArray(kGsaSchemaMarker);
    const QByteArray digest = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
    bool ok = false;
    const quint32 raw = digest.left(8).toUInt(&ok, 16);
    const quint32 folded = (ok ? raw : 0u) % 1000000000u;
    return QString::number(folded);
}

} // namespace

namespace gsaversion {

QString unixTemplate() {
    return QString::fromUtf8(R"GSA(#!/bin/sh
# ZFSMgr GSA Version: __VERSION__
set -eu

PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin:$PATH"
GSA_CONFIG_FILE='__GSA_CONFIG_FILE__'

PROP_ENABLED='org.fc16.gsa:activado'
PROP_RECURSIVE='org.fc16.gsa:recursivo'
PROP_HOURLY='org.fc16.gsa:horario'
PROP_DAILY='org.fc16.gsa:diario'
PROP_WEEKLY='org.fc16.gsa:semanal'
PROP_MONTHLY='org.fc16.gsa:mensual'
PROP_YEARLY='org.fc16.gsa:anual'
PROP_LEVEL='org.fc16.gsa:nivelar'
PROP_DEST='org.fc16.gsa:destino'

[ -r "$GSA_CONFIG_FILE" ] || exit 1
. "$GSA_CONFIG_FILE"
[ -n "${CONFIG_DIR:-}" ] || exit 1
[ -n "${LOG_FILE:-}" ] || exit 1
[ -n "${CONNECTIONS_FILE:-}" ] || exit 1
[ -r "$CONNECTIONS_FILE" ] || exit 1
. "$CONNECTIONS_FILE"
[ -n "${SELF_CONNECTION:-}" ] || exit 1
[ -n "${KNOWN_HOSTS_FILE:-}" ] || exit 1

rotate_logs() {
  mkdir -p "$CONFIG_DIR"
  if [ -f "$LOG_FILE" ]; then
    size="$(wc -c < "$LOG_FILE" 2>/dev/null | tr -d ' ' || printf '0')"
    case "$size" in
      ''|*[!0-9]*) size=0 ;;
    esac
    if [ "$size" -ge 1048576 ]; then
      [ -f "$LOG_FILE.4" ] && rm -f "$LOG_FILE.4"
      [ -f "$LOG_FILE.3" ] && mv -f "$LOG_FILE.3" "$LOG_FILE.4"
      [ -f "$LOG_FILE.2" ] && mv -f "$LOG_FILE.2" "$LOG_FILE.3"
      [ -f "$LOG_FILE.1" ] && mv -f "$LOG_FILE.1" "$LOG_FILE.2"
      mv -f "$LOG_FILE" "$LOG_FILE.1"
    fi
  fi
}

init_logging() {
  rotate_logs
  touch "$LOG_FILE"
  exec >>"$LOG_FILE" 2>&1
}

log() {
  line="$(date '+%Y-%m-%d %H:%M:%S') $*"
  printf '%s\n' "$line" >&2
  if command -v logger >/dev/null 2>&1; then
    logger -t ZFSMgr-GSA -- "$line" 2>/dev/null || true
  fi
}

shq() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\"'\"'/g")"
}

prop_value() {
  zfs get -H -o value "$2" "$1" 2>/dev/null | head -n1 | tr -d '\r'
}

prop_local_value() {
  zfs get -H -o value -s local "$2" "$1" 2>/dev/null | head -n1 | tr -d '\r'
}

bool_on() {
  case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    on|yes|true|1) return 0 ;;
    *) return 1 ;;
  esac
}

int_value() {
  case "$1" in
    ''|*[!0-9]*) printf '0\n' ;;
    *) printf '%s\n' "$1" ;;
  esac
}

has_recursive_gsa_ancestor() {
  probe_ds="$1"
  probe_parent="${probe_ds%/*}"
  while [ "$probe_parent" != "$probe_ds" ] && [ -n "$probe_parent" ]; do
    if bool_on "$(prop_local_value "$probe_parent" "$PROP_ENABLED")" \
       && bool_on "$(prop_value "$probe_parent" "$PROP_RECURSIVE")"; then
      return 0
    fi
    probe_ds="$probe_parent"
    probe_parent="${probe_ds%/*}"
  done
  return 1
}

is_descendant_of_root() {
  ds="$1"
  root="$2"
  case "$ds" in
    "$root"/*) return 0 ;;
    *) return 1 ;;
  esac
}

is_descendant_of_processed_recursive_root() {
  ds="$1"
  [ -n "${PROCESSED_RECURSIVE_ROOTS:-}" ] || return 1
  old_ifs="${IFS:- }"
  IFS='\n'
  for root in $PROCESSED_RECURSIVE_ROOTS; do
    [ -n "$root" ] || continue
    if is_descendant_of_root "$ds" "$root"; then
      IFS="$old_ifs"
      return 0
    fi
  done
  IFS="$old_ifs"
  return 1
}

mark_processed_recursive_root() {
  ds="$1"
  if [ -z "${PROCESSED_RECURSIVE_ROOTS:-}" ]; then
    PROCESSED_RECURSIVE_ROOTS="$ds"
  else
    PROCESSED_RECURSIVE_ROOTS="${PROCESSED_RECURSIVE_ROOTS}
$ds"
  fi
}

build_target_recv_command() {
  ds="$1"
  recv_opts="$2"
  base="zfs recv ${recv_opts} $(shq "$ds")"
  if bool_on "$TARGET_USE_SUDO"; then
    if [ -n "$TARGET_PASS" ]; then
      printf "{ printf '%%s\\n' %s; cat; } | sudo -S -p '' sh -lc %s\n" "$(shq "$TARGET_PASS")" "$(shq "$base")"
    else
      printf "sudo -n sh -lc %s\n" "$(shq "$base")"
    fi
  else
    printf "%s\n" "$base"
  fi
}

build_target_list_command() {
  ds="$1"
  base="zfs list -H -t snapshot -o name -s creation -d 1 $(shq "$ds") 2>/dev/null"
  if bool_on "$TARGET_USE_SUDO"; then
    if [ -n "$TARGET_PASS" ]; then
      printf "printf '%%s\\n' %s | sudo -S -p '' sh -lc %s\n" "$(shq "$TARGET_PASS")" "$(shq "$base")"
    else
      printf "sudo -n sh -lc %s\n" "$(shq "$base")"
    fi
  else
    printf "%s\n" "$base"
  fi
}

run_via_target_ssh() {
  remote_cmd="$1"
  target="${TARGET_USER}@${TARGET_HOST}"
  port_opt=''
  key_opt=''
  known_hosts_opt="-o StrictHostKeyChecking=yes -o UserKnownHostsFile=$(shq "$KNOWN_HOSTS_FILE")"
  [ -r "$KNOWN_HOSTS_FILE" ] || {
    log "GSA level skip: fichero known_hosts no disponible: $KNOWN_HOSTS_FILE"
    return 126
  }
  [ -n "$TARGET_PORT" ] && port_opt="-p $TARGET_PORT"
  [ -n "$TARGET_KEY" ] && key_opt="-i $(shq "$TARGET_KEY")"
  if [ -n "$TARGET_PASS" ]; then
    if ! command -v sshpass >/dev/null 2>&1; then
      log "GSA level skip: sshpass no disponible para conectar a ${target}"
      return 127
    fi
    eval "SSHPASS=$(shq "$TARGET_PASS") sshpass -e ssh -o BatchMode=no -o ConnectTimeout=10 -o LogLevel=ERROR ${known_hosts_opt} ${port_opt} ${key_opt} $(shq "$target") $(shq "$remote_cmd")"
  else
    eval "ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR ${known_hosts_opt} ${port_opt} ${key_opt} $(shq "$target") $(shq "$remote_cmd")"
  fi
}

list_target_snapshots() {
  ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name -s creation -d 1 "$ds" 2>/dev/null
  else
    run_via_target_ssh "$(build_target_list_command "$ds")"
  fi
}

target_dataset_exists() {
  ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -o name "$ds" >/dev/null 2>&1
  else
    run_via_target_ssh "zfs list -H -o name $(shq "$ds") >/dev/null 2>&1"
  fi
}

target_dataset_has_snapshots() {
  ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name -d 1 "$ds" 2>/dev/null | grep -q .
  else
    run_via_target_ssh "zfs list -H -t snapshot -o name -d 1 $(shq "$ds") 2>/dev/null | grep -q ."
  fi
}

due_classes() {
  hour="$(date '+%H')"
  dow="$(date '+%u')"
  dom="$(date '+%d')"
  md="$(date '+%m%d')"
  hourly="$(int_value "$1")"
  daily="$(int_value "$2")"
  weekly="$(int_value "$3")"
  monthly="$(int_value "$4")"
  yearly="$(int_value "$5")"
  [ "$hourly" -gt 0 ] && printf 'hourly\n'
  [ "$daily" -gt 0 ] && [ "$hour" = "00" ] && printf 'daily\n'
  [ "$weekly" -gt 0 ] && [ "$hour" = "00" ] && [ "$dow" = "7" ] && printf 'weekly\n'
  [ "$monthly" -gt 0 ] && [ "$hour" = "00" ] && [ "$dom" = "01" ] && printf 'monthly\n'
  [ "$yearly" -gt 0 ] && [ "$hour" = "00" ] && [ "$md" = "0101" ] && printf 'yearly\n'
}

create_snapshot() {
  ds="$1"
  klass="$2"
  recursive="$3"
  stamp="$(date '+%Y%m%d-%H%M%S')"
  snap_name="GSA-${klass}-${stamp}"
  log "GSA snapshot attempt for $ds: class=$klass recursive=$recursive snap=$snap_name"
  if zfs list -H -t snapshot -o name "${ds}@${snap_name}" >/dev/null 2>&1; then
    log "GSA snapshot skip for $ds: ${snap_name} ya existe"
    printf '%s\n' "$snap_name"
    return 0
  fi
  if bool_on "$recursive"; then
    zfs snapshot -r "${ds}@${snap_name}"
  else
    zfs snapshot "${ds}@${snap_name}"
  fi
  log "GSA snapshot created for $ds: ${snap_name}"
  printf '%s\n' "$snap_name"
}

prune_snapshots() {
  ds="$1"
  klass="$2"
  keep="$3"
  recursive="$4"
  [ "$(int_value "$keep")" -gt 0 ] || return 0
  snaps="$(zfs list -H -t snapshot -o name -s creation -r "$ds" 2>/dev/null | grep "^${ds}@GSA-${klass}-" || true)"
  [ -n "$snaps" ] || return 0
  total="$(printf '%s\n' "$snaps" | sed '/^$/d' | wc -l | tr -d ' ')"
  [ "$total" -gt "$keep" ] || return 0
  remove_count=$((total - keep))
  printf '%s\n' "$snaps" | sed '/^$/d' | head -n "$remove_count" | while IFS= read -r snap; do
    snap_short="${snap#${ds}@}"
    if bool_on "$recursive"; then
      zfs destroy -r "${ds}@${snap_short}" || true
    else
      zfs destroy "${ds}@${snap_short}" || true
    fi
  done
}

latest_target_snapshot() {
  dst_ds="$1"
  list_target_snapshots "$dst_ds" | sed -n "s#^${dst_ds}@##p" | tail -n 1
}

source_has_snapshot() {
  src_ds="$1"
  snap_name="$2"
  [ -n "$snap_name" ] || return 1
  zfs list -H -t snapshot -o name "${src_ds}@${snap_name}" >/dev/null 2>&1
}

level_snapshot() {
  src_ds="$1"
  recursive="$2"
  snap_name="$3"
  dst_spec="$4"
  level_on="$5"
  bool_on "$level_on" || return 0
  [ -n "$dst_spec" ] || return 0
  case "$dst_spec" in
    *::*/*) : ;;
    *) log "GSA level skip for $src_ds: invalid destination $dst_spec"; return 0 ;;
  esac
  dst_conn="${dst_spec%%::*}"
  dst_dataset="${dst_spec#*::}"
  if ! resolve_target_connection "$dst_conn"; then
    log "GSA level skip for $src_ds: destination connection not resolvable ($dst_conn)"
    return 0
  fi
  target_exists='0'
  if target_dataset_exists "$dst_dataset"; then
    target_exists='1'
  fi
  base_snap=''
  recv_opts='-u'
  send_opts='-wLEc'
  if bool_on "$recursive"; then
    recv_opts='-u -s'
    send_opts='-wLEcR'
  fi
  if [ "$target_exists" = "1" ] && target_dataset_has_snapshots "$dst_dataset"; then
    base_snap="$(latest_target_snapshot "$dst_dataset")"
    if [ -z "$base_snap" ]; then
      log "GSA level skip for $src_ds: destination $dst_dataset has snapshots but latest snapshot could not be determined"
      return 0
    fi
    case "$base_snap" in
      GSA-*) : ;;
      *)
        log "GSA level error for $src_ds: Destino tiene snapshots manuales ($dst_dataset@$base_snap)"
        return 1
        ;;
    esac
    if ! source_has_snapshot "$src_ds" "$base_snap"; then
      log "GSA level skip for $src_ds: latest destination snapshot $dst_dataset@$base_snap does not exist in source"
      return 0
    fi
    recv_opts='-u -F'
    if bool_on "$recursive"; then
      recv_opts='-u -s -F'
    fi
  fi
  recv_cmd="$(build_target_recv_command "$dst_dataset" "$recv_opts")"
  if [ "$TARGET_MODE" = "local" ]; then
    if [ -n "$base_snap" ]; then
      zfs send ${send_opts} -i "@${base_snap}" "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
    else
      zfs send ${send_opts} "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
    fi
  else
    if ! run_via_target_ssh "true" >/dev/null 2>&1; then
      log "GSA level skip for $src_ds: SSH no disponible hacia $dst_conn"
      return 0
    fi
    if [ -n "$base_snap" ]; then
      zfs send ${send_opts} -i "@${base_snap}" "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
    else
      zfs send ${send_opts} "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
    fi
  fi
}

main() {
  init_logging
  log "GSA start version __VERSION__"
  datasets="$(zfs list -H -o name -t filesystem 2>/dev/null || true)"
  [ -n "$datasets" ] || exit 0
  PROCESSED_RECURSIVE_ROOTS=''
  while IFS= read -r ds; do
    [ -n "$ds" ] || continue
    enabled="$(prop_local_value "$ds" "$PROP_ENABLED")"
    bool_on "$enabled" || continue
    if is_descendant_of_processed_recursive_root "$ds"; then
      continue
    fi
    recursive="$(prop_value "$ds" "$PROP_RECURSIVE")"
    if has_recursive_gsa_ancestor "$ds"; then
      continue
    fi
    hourly="$(prop_value "$ds" "$PROP_HOURLY")"
    daily="$(prop_value "$ds" "$PROP_DAILY")"
    weekly="$(prop_value "$ds" "$PROP_WEEKLY")"
    monthly="$(prop_value "$ds" "$PROP_MONTHLY")"
    yearly="$(prop_value "$ds" "$PROP_YEARLY")"
    level_on="$(prop_value "$ds" "$PROP_LEVEL")"
    dst_spec="$(prop_value "$ds" "$PROP_DEST")"
    due="$(due_classes "$hourly" "$daily" "$weekly" "$monthly" "$yearly" || true)"
    log "GSA evaluate $ds: recursive=$recursive hourly=$hourly daily=$daily weekly=$weekly monthly=$monthly yearly=$yearly due=$(printf '%s' "$due" | tr '\n' ',' | sed 's/,$//')"
    [ -n "$due" ] || continue
    printf '%s\n' "$due" | while IFS= read -r klass; do
      [ -n "$klass" ] || continue
      snap_name="$(create_snapshot "$ds" "$klass" "$recursive")"
      case "$klass" in
        hourly) keep="$hourly" ;;
        daily) keep="$daily" ;;
        weekly) keep="$weekly" ;;
        monthly) keep="$monthly" ;;
        yearly) keep="$yearly" ;;
        *) keep=0 ;;
      esac
      prune_snapshots "$ds" "$klass" "$keep" "$recursive"
      level_snapshot "$ds" "$recursive" "$snap_name" "$dst_spec" "$level_on" || true
    done
    if bool_on "$recursive"; then
      mark_processed_recursive_root "$ds"
    fi
  done <<EOF
$datasets
