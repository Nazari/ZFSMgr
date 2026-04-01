#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "mainwindow_ui_logic.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QHeaderView>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QStackedWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QJsonDocument>
#include <QJsonObject>

#include <QtConcurrent/QtConcurrent>

namespace {
constexpr int kConnIdxRole = Qt::UserRole + 10;
constexpr int kIsConnectionRootRole = Qt::UserRole + 36;
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr const char* kGsaTaskName = "ZFSMgr-GSA";
constexpr const char* kGsaUnixScriptPath = "/usr/local/libexec/zfsmgr-gsa.sh";
constexpr const char* kGsaUnixConfigDirPath = "/etc/zfsmgr";
constexpr const char* kGsaLinuxRuntimeDirPath = "/var/lib/zfsmgr";
constexpr const char* kGsaFreeBsdRuntimeDirPath = "/var/db/zfsmgr";
constexpr const char* kGsaUnixConfigPath = "/etc/zfsmgr/gsa.conf";
constexpr const char* kGsaUnixConnectionsPath = "/etc/zfsmgr/gsa-connections.conf";
constexpr const char* kGsaUnixKnownHostsPath = "/etc/zfsmgr/gsa_known_hosts";
constexpr const char* kGsaMacPlistPath = "/Library/LaunchDaemons/org.zfsmgr.gsa.plist";
constexpr const char* kGsaLinuxServicePath = "/etc/systemd/system/zfsmgr-gsa.service";
constexpr const char* kGsaLinuxTimerPath = "/etc/systemd/system/zfsmgr-gsa.timer";
constexpr const char* kGsaFreeBsdCronMarkerBegin = "# BEGIN ZFSMgr GSA";
constexpr const char* kGsaFreeBsdCronMarkerEnd = "# END ZFSMgr GSA";
struct ConnTreeNavSnapshot {
    QSet<QString> expandedKeys;
    QString selectedKey;
};

QString connContentStateTokenForTree(QTreeWidget* tree) {
    if (!tree) {
        return QString();
    }
    auto tokenFromItem = [](QTreeWidgetItem* item) -> QString {
        if (!item) {
            return QString();
        }
        QTreeWidgetItem* owner = item;
        while (owner && owner->data(0, Qt::UserRole).toString().isEmpty()
               && !owner->data(0, kIsPoolRootRole).toBool()) {
            owner = owner->parent();
        }
        if (!owner) {
            return QString();
        }
        const int connIdx = owner->data(0, kConnIdxRole).toInt();
        const QString poolName = owner->data(0, kPoolNameRole).toString().trimmed();
        if (connIdx < 0 || poolName.isEmpty()) {
            return QString();
        }
        return QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    };
    if (QTreeWidgetItem* current = tree->currentItem()) {
        const QString token = tokenFromItem(current);
        if (!token.isEmpty()) {
            return token;
        }
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = tree->topLevelItem(i);
        if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
            continue;
        }
        const QString token = tokenFromItem(root);
        if (!token.isEmpty()) {
            return token;
        }
    }
    return QString();
}

QString mergedConnectionCommandErrorText(const QString& out, const QString& err, int rc) {
    QStringList parts;
    const QString trimmedErr = err.trimmed();
    const QString trimmedOut = out.trimmed();
    if (!trimmedErr.isEmpty()) {
        parts << trimmedErr;
    }
    if (!trimmedOut.isEmpty()) {
        parts << trimmedOut;
    }
    if (parts.isEmpty()) {
        return QStringLiteral("exit %1").arg(rc);
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString stripLeadingSudoForExecution(QString cmd) {
    cmd = cmd.trimmed();
    if (cmd.startsWith(QStringLiteral("sudo "))) {
        cmd = cmd.mid(5).trimmed();
    }
    cmd.replace(QStringLiteral("&& sudo "), QStringLiteral("&& "));
    cmd.replace(QStringLiteral("; sudo "), QStringLiteral("; "));
    cmd.replace(QStringLiteral("\n sudo "), QStringLiteral("\n "));
    return cmd.trimmed();
}

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
    if (m.captured(4).isEmpty()) {
        out << 999999;
    } else {
        out << m.captured(4).toInt();
    }
    out << (m.captured(5).isEmpty() ? 0 : m.captured(5).toInt());
    return out;
}

int compareAppVersions(const QString& a, const QString& b) {
    const QVector<int> ka = versionOrderingKey(a);
    const QVector<int> kb = versionOrderingKey(b);
    if (ka.isEmpty() || kb.isEmpty()) {
        return QString::compare(a.trimmed(), b.trimmed(), Qt::CaseInsensitive);
    }
    for (int i = 0; i < qMin(ka.size(), kb.size()); ++i) {
        if (ka[i] < kb[i]) {
            return -1;
        }
        if (ka[i] > kb[i]) {
            return 1;
        }
    }
    return 0;
}

QString escapePsSingleQuoted(QString s) {
    return s.replace('\'', QStringLiteral("''"));
}

QString gsaScriptVersion() {
    return QStringLiteral(ZFSMGR_APP_VERSION) + QStringLiteral(".") + QStringLiteral(ZFSMGR_GSA_VERSION_SUFFIX);
}

QString gsaUnixMainConfigPayload(const QString& selfConnectionName, const QString& runtimeConfigDir) {
    QString payload;
    payload += QStringLiteral("SELF_CONNECTION=%1\n").arg(mwhelpers::shSingleQuote(selfConnectionName));
    payload += QStringLiteral("CONFIG_DIR=%1\n").arg(mwhelpers::shSingleQuote(runtimeConfigDir));
    payload += QStringLiteral("LOG_FILE=%1\n").arg(mwhelpers::shSingleQuote(runtimeConfigDir + QStringLiteral("/GSA.log")));
    payload += QStringLiteral("KNOWN_HOSTS_FILE=%1\n").arg(mwhelpers::shSingleQuote(QString::fromLatin1(kGsaUnixKnownHostsPath)));
    payload += QStringLiteral("CONNECTIONS_FILE=%1\n").arg(mwhelpers::shSingleQuote(QString::fromLatin1(kGsaUnixConnectionsPath)));
    return payload;
}

QString gsaUnixScriptPayload() {
    QString script = QString::fromUtf8(R"GSA(#!/bin/sh
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
  IFS='
'
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
  target_list_ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name -s creation -d 1 "$target_list_ds" 2>/dev/null
  else
    run_via_target_ssh "$(build_target_list_command "$target_list_ds")"
  fi
}

target_dataset_exists() {
  target_exists_ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -o name "$target_exists_ds" >/dev/null 2>&1
  else
    run_via_target_ssh "zfs list -H -o name $(shq "$target_exists_ds") >/dev/null 2>&1"
  fi
}

target_dataset_has_snapshots() {
  target_snap_ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name -d 1 "$target_snap_ds" 2>/dev/null | grep -q .
  else
    run_via_target_ssh "zfs list -H -t snapshot -o name -d 1 $(shq "$target_snap_ds") 2>/dev/null | grep -q ."
  fi
}

target_has_snapshot_name() {
  target_snap_ds="$1"
  target_snap_name="$2"
  [ -n "$target_snap_name" ] || return 1
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name "${target_snap_ds}@${target_snap_name}" >/dev/null 2>&1
  else
    run_via_target_ssh "zfs list -H -t snapshot -o name $(shq "${target_snap_ds}@${target_snap_name}") >/dev/null 2>&1"
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
  latest_dst_ds="$1"
  list_target_snapshots "$latest_dst_ds" | sed -n "s#^${latest_dst_ds}@##p" | tail -n 1
}

source_has_snapshot() {
  source_probe_ds="$1"
  source_probe_snap_name="$2"
  [ -n "$source_probe_snap_name" ] || return 1
  zfs list -H -t snapshot -o name "${source_probe_ds}@${source_probe_snap_name}" >/dev/null 2>&1
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
  send_opts='-wLec'
  if bool_on "$recursive"; then
    recv_opts='-u -s'
    send_opts='-wLecR'
  fi
  if [ "$target_exists" = "1" ] && target_dataset_has_snapshots "$dst_dataset"; then
    if target_has_snapshot_name "$dst_dataset" "$snap_name"; then
      log "GSA level skip for $src_ds: destination snapshot $dst_dataset@$snap_name already exists"
      return 0
    fi
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
EOF
}

main "$@"
)GSA");
    script.replace(QStringLiteral("__VERSION__"), gsaScriptVersion());
    script.replace(QStringLiteral("__GSA_CONFIG_FILE__"), QString::fromLatin1(kGsaUnixConfigPath));
    return script;
}

QString gsaWindowsScriptPayload(const QString& selfConnectionName) {
    QString script = QString::fromUtf8(R"GSA(# ZFSMgr GSA Version: __VERSION__
$ErrorActionPreference = 'Stop'

$SelfConnection = '__SELF_CONNECTION__'
$ConfigDir = '__CONFIG_DIR__'
$LogFile = Join-Path $ConfigDir 'GSA.log'
$PropEnabled = 'org.fc16.gsa:activado'
$PropRecursive = 'org.fc16.gsa:recursivo'
$PropHourly = 'org.fc16.gsa:horario'
$PropDaily = 'org.fc16.gsa:diario'
$PropWeekly = 'org.fc16.gsa:semanal'
$PropMonthly = 'org.fc16.gsa:mensual'
$PropYearly = 'org.fc16.gsa:anual'
$PropLevel = 'org.fc16.gsa:nivelar'
$PropDest = 'org.fc16.gsa:destino'

function Rotate-GsaLog {
  New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
  if (Test-Path -LiteralPath $LogFile) {
    $size = (Get-Item -LiteralPath $LogFile).Length
    if ($size -ge 1MB) {
      if (Test-Path -LiteralPath ($LogFile + '.4')) { Remove-Item -Force -LiteralPath ($LogFile + '.4') }
      if (Test-Path -LiteralPath ($LogFile + '.3')) { Move-Item -Force -LiteralPath ($LogFile + '.3') -Destination ($LogFile + '.4') }
      if (Test-Path -LiteralPath ($LogFile + '.2')) { Move-Item -Force -LiteralPath ($LogFile + '.2') -Destination ($LogFile + '.3') }
      if (Test-Path -LiteralPath ($LogFile + '.1')) { Move-Item -Force -LiteralPath ($LogFile + '.1') -Destination ($LogFile + '.2') }
      Move-Item -Force -LiteralPath $LogFile -Destination ($LogFile + '.1')
    }
  }
}

function Write-NativeLog([string]$Message, [string]$Level = 'INFORMATION') {
  $msg = $Message.Trim()
  if ([string]::IsNullOrWhiteSpace($msg)) { return }
  $exe = Get-Command eventcreate.exe -ErrorAction SilentlyContinue
  if (-not $exe) { return }
  $type = switch ($Level.Trim().ToUpperInvariant()) {
    'ERROR' { 'ERROR' }
    'WARNING' { 'WARNING' }
    default { 'INFORMATION' }
  }
  try {
    & $exe.Source /T $type /ID 1000 /L APPLICATION /SO ZFSMgr /D $msg | Out-Null
  } catch {}
}

function Write-GsaLog([string]$Message, [string]$Level = 'INFORMATION') {
  $line = ('{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message.Trim())
  Write-Output $line
  Write-NativeLog $line $Level
}

function Get-PropValue([string]$Dataset, [string]$Prop) {
  try {
    return ((& zfs get -H -o value $Prop $Dataset 2>$null) | Select-Object -First 1).Trim()
  } catch {
    return ''
  }
}

function Get-PropLocalValue([string]$Dataset, [string]$Prop) {
  try {
    return ((& zfs get -H -o value -s local $Prop $Dataset 2>$null) | Select-Object -First 1).Trim()
  } catch {
    return ''
  }
}

function Test-On([string]$Value) {
  switch ($Value.Trim().ToLowerInvariant()) {
    'on' { return $true }
    'yes' { return $true }
    'true' { return $true }
    '1' { return $true }
    default { return $false }
  }
}

function Get-IntValue([string]$Value) {
  $n = 0
  [void][int]::TryParse($Value, [ref]$n)
  return $n
}

function Test-HasRecursiveGsaAncestor([string]$Dataset) {
  $current = $Dataset
  while ($true) {
    $idx = $current.LastIndexOf('/')
    if ($idx -lt 0) { return $false }
    $current = $current.Substring(0, $idx)
    if ((Test-On (Get-PropLocalValue $current $PropEnabled)) -and (Test-On (Get-PropValue $current $PropRecursive))) {
      return $true
    }
  }
}

function Test-IsDescendantOfRoot([string]$Dataset, [string]$Root) {
  return $Dataset.StartsWith($Root + '/', [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-IsDescendantOfProcessedRecursiveRoot([string]$Dataset, [System.Collections.Generic.List[string]]$ProcessedRoots) {
  foreach ($root in $ProcessedRoots) {
    if ([string]::IsNullOrWhiteSpace($root)) { continue }
    if (Test-IsDescendantOfRoot $Dataset $root) { return $true }
  }
  return $false
}

function Get-DueClasses([int]$Hourly, [int]$Daily, [int]$Weekly, [int]$Monthly, [int]$Yearly) {
  $now = Get-Date
  $out = New-Object System.Collections.Generic.List[string]
  if ($Hourly -gt 0) { $out.Add('hourly') }
  if ($Daily -gt 0 -and $now.Hour -eq 0) { $out.Add('daily') }
  if ($Weekly -gt 0 -and $now.Hour -eq 0 -and $now.DayOfWeek -eq [DayOfWeek]::Sunday) { $out.Add('weekly') }
  if ($Monthly -gt 0 -and $now.Hour -eq 0 -and $now.Day -eq 1) { $out.Add('monthly') }
  if ($Yearly -gt 0 -and $now.Hour -eq 0 -and $now.Day -eq 1 -and $now.Month -eq 1) { $out.Add('yearly') }
  return $out
}

function New-GsaSnapshot([string]$Dataset, [string]$Class, [bool]$Recursive) {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $snapName = "GSA-$Class-$stamp"
  Write-GsaLog ("GSA snapshot attempt for " + $Dataset + ": class=" + $Class + " recursive=" + $Recursive + " snap=" + $snapName)
  try {
    & zfs list -H -t snapshot -o name "$Dataset@$snapName" | Out-Null
    Write-GsaLog ("GSA snapshot skip for " + $Dataset + ": " + $snapName + " ya existe")
    return $snapName
  } catch {}
  if ($Recursive) {
    & zfs snapshot -r "$Dataset@$snapName" | Out-Null
  } else {
    & zfs snapshot "$Dataset@$snapName" | Out-Null
  }
  Write-GsaLog ("GSA snapshot created for " + $Dataset + ": " + $snapName)
  return $snapName
}

function Prune-GsaSnapshots([string]$Dataset, [string]$Class, [int]$Keep, [bool]$Recursive) {
  if ($Keep -le 0) { return }
  $pattern = "^" + [regex]::Escape($Dataset + '@GSA-' + $Class + '-')
  $snaps = @((& zfs list -H -t snapshot -o name -s creation -r $Dataset 2>$null) | Where-Object { $_ -match $pattern })
  if ($snaps.Count -le $Keep) { return }
  $remove = $snaps | Select-Object -First ($snaps.Count - $Keep)
  foreach ($snap in $remove) {
    $snapShort = $snap.Substring($Dataset.Length + 1)
    try {
      if ($Recursive) {
        & zfs destroy -r "$Dataset@$snapShort" | Out-Null
      } else {
        & zfs destroy "$Dataset@$snapShort" | Out-Null
      }
    } catch {}
  }
}

function Get-LatestTargetSnapshot([string]$DstDataset) {
  $snaps = @((& zfs list -H -t snapshot -o name -s creation -d 1 $DstDataset 2>$null) |
    Where-Object { $_ -like "$DstDataset@*" } |
    ForEach-Object { $_.Substring($DstDataset.Length + 1) })
  if ($snaps.Count -le 0) { return $null }
  return $snaps[-1]
}

function Test-SourceHasSnapshot([string]$SrcDataset, [string]$SnapName) {
  if ([string]::IsNullOrWhiteSpace($SnapName)) { return $false }
  try {
    & zfs list -H -t snapshot -o name "$SrcDataset@$SnapName" | Out-Null
    return $true
  } catch {
    return $false
  }
}

function Test-TargetHasSnapshotName([string]$DstDataset, [string]$SnapName) {
  if ([string]::IsNullOrWhiteSpace($SnapName)) { return $false }
  try {
    & zfs list -H -t snapshot -o name "$DstDataset@$SnapName" | Out-Null
    return $true
  } catch {
    return $false
  }
}

function Invoke-GsaLevel([string]$SrcDataset, [bool]$Recursive, [string]$SnapName, [string]$DestSpec, [bool]$LevelOn) {
  if (-not $LevelOn -or [string]::IsNullOrWhiteSpace($DestSpec)) { return }
  if ($DestSpec -notmatch '^(?<conn>[^:]+)::(?<dataset>.+)$') { return }
  $dstConn = $Matches['conn']
  $dstDataset = $Matches['dataset']
  if ($dstConn -ne $SelfConnection -and $dstConn -ne 'Local') { return }
  $targetExists = $true
  try {
    & zfs list -H -o name $dstDataset | Out-Null
  } catch {
    $targetExists = $false
  }
  $baseSnap = $null
  $recvOpts = if ($Recursive) { "-u -s" } else { "-u" }
  $sendOpts = if ($Recursive) { "-wLecR" } else { "-wLec" }
  if ($targetExists) {
    $dstHasSnapshots = $false
    try {
      $dstHasSnapshots = @((& zfs list -H -t snapshot -o name -d 1 $DstDataset 2>$null)).Count -gt 0
    } catch {
      $dstHasSnapshots = $false
    }
    if ($dstHasSnapshots) {
      if (Test-TargetHasSnapshotName $DstDataset $SnapName) {
        Write-GsaLog ("GSA level skip for " + $SrcDataset + ": destination snapshot " + $DstDataset + "@" + $SnapName + " already exists")
        return
      }
      $baseSnap = Get-LatestTargetSnapshot $DstDataset
      if (-not $baseSnap) {
        Write-GsaLog ("GSA level skip for " + $SrcDataset + ": destination " + $DstDataset + " has snapshots but latest snapshot could not be determined")
        return
      }
      if (-not $baseSnap.StartsWith('GSA-', [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-GsaLog ("GSA level error for " + $SrcDataset + ": Destino tiene snapshots manuales (" + $DstDataset + "@" + $baseSnap + ")")
        throw "Destino tiene snapshots manuales"
      }
      if (-not (Test-SourceHasSnapshot $SrcDataset $baseSnap)) {
        Write-GsaLog ("GSA level skip for " + $SrcDataset + ": latest destination snapshot " + $DstDataset + "@" + $baseSnap + " does not exist in source")
        return
      }
      $recvOpts = if ($Recursive) { "-u -s -F" } else { "-u -F" }
    }
  }
  $recvCmd = "zfs recv $recvOpts '$DstDataset'"
  if ($baseSnap) {
    & powershell -NoProfile -Command "& zfs send $sendOpts -i '@$baseSnap' '$SrcDataset@$SnapName' | $recvCmd" | Out-Null
  } else {
    & powershell -NoProfile -Command "& zfs send $sendOpts '$SrcDataset@$SnapName' | $recvCmd" | Out-Null
  }
try {
  Rotate-GsaLog
  Start-Transcript -LiteralPath $LogFile -Append | Out-Null
  Write-GsaLog ("GSA start version " + '__VERSION__')
  $datasets = @((& zfs list -H -o name -t filesystem 2>$null))
  $processedRecursiveRoots = New-Object 'System.Collections.Generic.List[string]'
  foreach ($ds in $datasets) {
    if ([string]::IsNullOrWhiteSpace($ds)) { continue }
    if (-not (Test-On (Get-PropLocalValue $ds $PropEnabled))) { continue }
    if (Test-IsDescendantOfProcessedRecursiveRoot $ds $processedRecursiveRoots) {
      continue
    }
    if (Test-HasRecursiveGsaAncestor $ds) {
      continue
    }
    $recursive = Test-On (Get-PropValue $ds $PropRecursive)
    $hourly = Get-IntValue (Get-PropValue $ds $PropHourly)
    $daily = Get-IntValue (Get-PropValue $ds $PropDaily)
    $weekly = Get-IntValue (Get-PropValue $ds $PropWeekly)
    $monthly = Get-IntValue (Get-PropValue $ds $PropMonthly)
    $yearly = Get-IntValue (Get-PropValue $ds $PropYearly)
    $levelOn = Test-On (Get-PropValue $ds $PropLevel)
    $destSpec = Get-PropValue $ds $PropDest
    $dueClasses = @(Get-DueClasses $hourly $daily $weekly $monthly $yearly)
    Write-GsaLog ("GSA evaluate " + $ds + ": recursive=" + $recursive + " hourly=" + $hourly + " daily=" + $daily + " weekly=" + $weekly + " monthly=" + $monthly + " yearly=" + $yearly + " due=" + ($dueClasses -join ','))
    foreach ($klass in $dueClasses) {
      $snapName = New-GsaSnapshot $ds $klass $recursive
      switch ($klass) {
        'hourly' { $keep = $hourly }
        'daily' { $keep = $daily }
        'weekly' { $keep = $weekly }
        'monthly' { $keep = $monthly }
        'yearly' { $keep = $yearly }
        default { $keep = 0 }
      }
      Prune-GsaSnapshots $ds $klass $keep $recursive
      Invoke-GsaLevel $ds $recursive $snapName $destSpec $levelOn
    }
    if ($recursive) { $processedRecursiveRoots.Add($ds) | Out-Null }
  }
} finally {
  Write-GsaLog 'GSA end'
  try { Stop-Transcript | Out-Null } catch {}
}
)GSA");
    script.replace(QStringLiteral("__VERSION__"), gsaScriptVersion());
    script.replace(QStringLiteral("__SELF_CONNECTION__"), escapePsSingleQuoted(selfConnectionName));
    return script;
}

QString gsaKnownHostsPayload(const QVector<ConnectionProfile>& profiles, int selfIdx, const QSet<QString>& requiredConnNames) {
    QSet<QString> seen;
    QStringList lines;
    for (int i = 0; i < profiles.size(); ++i) {
        if (i == selfIdx) {
            continue;
        }
        const ConnectionProfile& cp = profiles[i];
        const QString connName = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
        if (!requiredConnNames.contains(connName)) {
            continue;
        }
        if (cp.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString host = cp.host.trimmed();
        if (host.isEmpty()) {
            continue;
        }
        const int port = cp.port > 0 ? cp.port : 22;
        const QString scanKey = QStringLiteral("%1:%2").arg(host).arg(port);
        if (seen.contains(scanKey)) {
            continue;
        }
        seen.insert(scanKey);

        QProcess proc;
        QStringList args;
        args << QStringLiteral("-T") << QStringLiteral("5")
             << QStringLiteral("-p") << QString::number(port)
             << host;
        proc.start(QStringLiteral("ssh-keyscan"), args);
        if (!proc.waitForFinished(8000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            continue;
        }
        const QString out = QString::fromUtf8(proc.readAllStandardOutput());
        for (const QString& line : out.split(QLatin1Char('\n'))) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
                continue;
            }
            lines << trimmed;
        }
    }
    lines.removeDuplicates();
    return lines.join(QStringLiteral("\n")) + (lines.isEmpty() ? QString() : QStringLiteral("\n"));
}

QString connTreeNodeKey(QTreeWidgetItem* n) {
    if (!n) {
        return QString();
    }
    const QString pool = n->data(0, kPoolNameRole).toString().trimmed();
    if (n->data(0, kIsPoolRootRole).toBool()) {
        return pool.isEmpty() ? QString() : QStringLiteral("pool:%1").arg(pool);
    }
    const QString marker = n->data(0, kConnPropKeyRole).toString();
    if (marker == QStringLiteral("__pool_block_info__")) {
        return pool.isEmpty() ? QString() : QStringLiteral("info:%1").arg(pool);
    }
    const QString ds = n->data(0, Qt::UserRole).toString().trimmed();
    if (!ds.isEmpty()) {
        const QString snap = n->data(1, Qt::UserRole).toString().trimmed();
        return pool.isEmpty() ? QStringLiteral("ds:%1@%2").arg(ds, snap)
                              : QStringLiteral("ds:%1:%2@%3").arg(pool, ds, snap);
    }
    return QString();
}

QString datasetLeafNameConn(const QString& datasetName) {
    return datasetName.contains('/') ? datasetName.section('/', -1, -1) : datasetName;
}

void applySnapshotVisualStateConn(QTreeWidgetItem* item) {
    if (!item) {
        return;
    }
    const QString ds = item->data(0, Qt::UserRole).toString().trimmed();
    if (ds.isEmpty()) {
        return;
    }
    const QString snap = item->data(1, Qt::UserRole).toString().trimmed();
    item->setText(0, snap.isEmpty() ? datasetLeafNameConn(ds)
                                    : QStringLiteral("%1@%2").arg(datasetLeafNameConn(ds), snap));
    const bool hideChildren = !snap.isEmpty();
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* ch = item->child(i);
        if (!ch) {
            continue;
        }
        const bool isDatasetNode = !ch->data(0, Qt::UserRole).toString().trimmed().isEmpty();
        if (isDatasetNode) {
            ch->setHidden(hideChildren);
        }
    }
}

struct SnapshotKeyParts {
    QString pool;
    QString dataset;
    QString snapshot;
};

SnapshotKeyParts parseSnapshotKey(const QString& key) {
    SnapshotKeyParts out;
    if (!key.startsWith(QStringLiteral("ds:"))) {
        return out;
    }
    QString body = key.mid(3);
    const int at = body.indexOf('@');
    out.snapshot = (at >= 0) ? body.mid(at + 1).trimmed() : QString();
    QString left = (at >= 0) ? body.left(at) : body;
    const int c = left.indexOf(':');
    if (c > 0 && !left.left(c).contains('/')) {
        out.pool = left.left(c).trimmed();
        out.dataset = left.mid(c + 1).trimmed();
    } else {
        out.dataset = left.trimmed();
    }
    return out;
}

QTreeWidgetItem* findDatasetNode(QTreeWidget* tree, const QString& pool, const QString& dataset) {
    if (!tree || dataset.isEmpty()) {
        return nullptr;
    }
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> rec = [&](QTreeWidgetItem* n) {
        if (!n || found) {
            return;
        }
        const QString ds = n->data(0, Qt::UserRole).toString().trimmed();
        if (!ds.isEmpty() && ds == dataset) {
            const QString nodePool = n->data(0, kPoolNameRole).toString().trimmed();
            if (pool.isEmpty() || pool == nodePool) {
                found = n;
                return;
            }
        }
        for (int i = 0; i < n->childCount(); ++i) {
            rec(n->child(i));
            if (found) {
                return;
            }
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        rec(tree->topLevelItem(i));
        if (found) {
            break;
        }
    }
    return found;
}

ConnTreeNavSnapshot captureConnTreeNavSnapshot(QTreeWidget* tree) {
    ConnTreeNavSnapshot snap;
    if (!tree) {
        return snap;
    }
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString k = connTreeNodeKey(n);
        if (!k.isEmpty() && n->isExpanded()) {
            snap.expandedKeys.insert(k);
        }
        for (int i = 0; i < n->childCount(); ++i) {
            collect(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        collect(tree->topLevelItem(i));
    }
    if (QTreeWidgetItem* cur = tree->currentItem()) {
        QTreeWidgetItem* owner = cur;
        while (owner && connTreeNodeKey(owner).isEmpty()) {
            owner = owner->parent();
        }
        if (owner) {
            snap.selectedKey = connTreeNodeKey(owner);
        }
    }
    return snap;
}

QTreeWidgetItem* findConnNodeByKey(QTreeWidget* tree, const QString& wantedKey) {
    if (!tree || wantedKey.isEmpty()) {
        return nullptr;
    }
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> recFind = [&](QTreeWidgetItem* n) {
        if (!n || found) {
            return;
        }
        if (connTreeNodeKey(n) == wantedKey) {
            found = n;
            return;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            recFind(n->child(i));
            if (found) {
                return;
            }
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        recFind(tree->topLevelItem(i));
        if (found) {
            break;
        }
    }
    return found;
}

QTreeWidgetItem* findParentNodeForDeletedDataset(QTreeWidget* tree, const QString& selectedKey) {
    if (!tree || !selectedKey.startsWith(QStringLiteral("ds:"))) {
        return nullptr;
    }
    QString rest = selectedKey.mid(3);
    QString poolPrefix;
    const int firstColon = rest.indexOf(':');
    if (firstColon > 0 && rest.left(firstColon).compare(QStringLiteral("pool"), Qt::CaseInsensitive) != 0) {
        const QString maybePool = rest.left(firstColon);
        if (!maybePool.contains('/')) {
            poolPrefix = maybePool;
            rest = rest.mid(firstColon + 1);
        }
    }
    const int at = rest.indexOf('@');
    QString ds = (at >= 0) ? rest.left(at) : rest;
    while (!ds.isEmpty()) {
        const int slash = ds.lastIndexOf('/');
        if (slash <= 0) {
            break;
        }
        ds = ds.left(slash);
        const QString parentKey = poolPrefix.isEmpty()
                                      ? QStringLiteral("ds:%1@").arg(ds)
                                      : QStringLiteral("ds:%1:%2@").arg(poolPrefix, ds);
        if (QTreeWidgetItem* sel = findConnNodeByKey(tree, parentKey)) {
            return sel;
        }
        const QString parentKeyNoAt = poolPrefix.isEmpty()
                                          ? QStringLiteral("ds:%1").arg(ds)
                                          : QStringLiteral("ds:%1:%2").arg(poolPrefix, ds);
        if (QTreeWidgetItem* sel = findConnNodeByKey(tree, parentKeyNoAt)) {
            return sel;
        }
    }
    return nullptr;
}

void restoreConnTreeNavSnapshot(QTreeWidget* tree, const ConnTreeNavSnapshot& snap) {
    if (!tree) {
        return;
    }
    const QSignalBlocker treeBlocker(tree);
    std::unique_ptr<QSignalBlocker> selectionBlocker;
    if (QItemSelectionModel* selectionModel = tree->selectionModel()) {
        selectionBlocker = std::make_unique<QSignalBlocker>(selectionModel);
    }
    QTreeWidgetItem* selectedItem = nullptr;
    std::function<void(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* n) {
        if (!n) {
            return;
        }
        const QString k = connTreeNodeKey(n);
        if (!k.isEmpty() && snap.expandedKeys.contains(k)) {
            n->setExpanded(true);
        }
        if (!snap.selectedKey.isEmpty() && k == snap.selectedKey) {
            selectedItem = n;
        }
        for (int i = 0; i < n->childCount(); ++i) {
            apply(n->child(i));
        }
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        apply(tree->topLevelItem(i));
    }
    if (!selectedItem) {
        selectedItem = findParentNodeForDeletedDataset(tree, snap.selectedKey);
    }
    if (!selectedItem && tree->topLevelItemCount() > 0) {
        selectedItem = tree->topLevelItem(0);
    }
    if (selectedItem) {
        tree->setCurrentItem(selectedItem);
    }
}

void restoreSnapshotSelectionInTree(QTreeWidget* tree, const ConnTreeNavSnapshot& nav) {
    if (!tree) {
        return;
    }
    const SnapshotKeyParts sk = parseSnapshotKey(nav.selectedKey);
    if (sk.snapshot.isEmpty()) {
        return;
    }
    if (QTreeWidgetItem* dsNode = findDatasetNode(tree, sk.pool, sk.dataset)) {
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(dsNode, 1))) {
            const int idx = cb->findText(sk.snapshot);
            if (idx > 0) {
                const QSignalBlocker b(cb);
                cb->setCurrentIndex(idx);
                dsNode->setData(1, Qt::UserRole, sk.snapshot);
                applySnapshotVisualStateConn(dsNode);
            }
        }
    }
}

void expandPoolRootsIfNoNav(QTreeWidget* tree, const ConnTreeNavSnapshot& nav) {
    if (!tree || !nav.expandedKeys.isEmpty() || !nav.selectedKey.trimmed().isEmpty()) {
        return;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (item && item->data(0, kIsPoolRootRole).toBool()) {
            item->setExpanded(true);
        }
    }
}

QString normalizeHostToken(QString host) {
    host = host.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']') && host.size() > 2) {
        host = host.mid(1, host.size() - 2);
    }
    while (host.endsWith('.')) {
        host.chop(1);
    }
    return host;
}

bool isLocalHostForUi(const QString& host) {
    const QString h = normalizeHostToken(host);
    if (h.isEmpty()) {
        return false;
    }
    if (h == QStringLiteral("localhost")
        || h == QStringLiteral("127.0.0.1")
        || h == QStringLiteral("::1")) {
        return true;
    }

    static const QSet<QString> aliases = []() {
        QSet<QString> s;
        s.insert(QStringLiteral("localhost"));
        s.insert(QStringLiteral("127.0.0.1"));
        s.insert(QStringLiteral("::1"));
        const QString local = normalizeHostToken(QSysInfo::machineHostName());
        if (!local.isEmpty()) {
            s.insert(local);
            s.insert(local + QStringLiteral(".local"));
            const int dot = local.indexOf('.');
            if (dot > 0) {
                const QString shortName = local.left(dot);
                s.insert(shortName);
                s.insert(shortName + QStringLiteral(".local"));
            }
        }
        return s;
    }();
    return aliases.contains(h);
}

} // namespace

int MainWindow::currentConnectionIndexFromUnifiedTree() const {
    if (!m_connContentTree
        || !m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()
        || !m_connContentTree->isVisible()) {
        return -1;
    }
    QTreeWidgetItem* item = m_connContentTree->currentItem();
    while (item && !item->data(0, kIsConnectionRootRole).toBool()
           && !item->data(0, kIsPoolRootRole).toBool()) {
        item = item->parent();
    }
    if (!item) {
        return -1;
    }
    return item->data(0, kConnIdxRole).toInt();
}

int MainWindow::currentConnectionIndexFromUi() const {
    const int treeIdx = currentConnectionIndexFromUnifiedTree();
    if (treeIdx >= 0 && treeIdx < m_profiles.size()) {
        return treeIdx;
    }
    return -1;
}

void MainWindow::setCurrentConnectionInUi(int connIdx) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    if (m_connContentTree
        && m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
        for (int i = 0; i < m_connContentTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* root = m_connContentTree->topLevelItem(i);
            if (!root || !root->data(0, kIsConnectionRootRole).toBool()) {
                continue;
            }
            if (root->data(0, kConnIdxRole).toInt() == connIdx) {
                m_connContentTree->setCurrentItem(root);
                break;
            }
        }
    }
}

QColor MainWindow::connectionStateRowColor(int connIdx) const {
    const QColor baseColor = palette().base().color();
    if (connIdx < 0 || connIdx >= m_states.size()) {
        return baseColor;
    }
    if (isConnectionDisconnected(connIdx)) {
        return QColor(QStringLiteral("#eef1f4"));
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    const QString status = st.status.trimmed().toUpper();
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = rx.match(st.zfsVersion.trimmed());
    const bool zfsTooOld = m.hasMatch()
                           && m.captured(1).toInt() == 2
                           && (m.captured(2).toInt() < 3
                               || (m.captured(2).toInt() == 3
                                   && (m.captured(3).isEmpty() ? 0 : m.captured(3).toInt()) < 3));
    if (status == QStringLiteral("OK")) {
        if (zfsTooOld) {
            return QColor(QStringLiteral("#f9dfdf"));
        }
        return st.missingUnixCommands.isEmpty() ? QColor(QStringLiteral("#e4f4e4"))
                                                : QColor(QStringLiteral("#fff1d9"));
    }
    if (!status.isEmpty()) {
        return QColor(QStringLiteral("#f9dfdf"));
    }
    return baseColor;
}

QString MainWindow::connectionStateColorReason(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        return QString();
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    if (isConnectionDisconnected(connIdx)) {
        return trk(QStringLiteral("t_conn_color_reason_off_001"),
                   QStringLiteral("La conexión está marcada como desconectada."),
                   QStringLiteral("The connection is marked as disconnected."),
                   QStringLiteral("该连接已标记为断开。"));
    }
    const QString stUp = st.status.trimmed().toUpper();
    if (stUp != QStringLiteral("OK")) {
        return st.detail.trimmed().isEmpty()
                   ? trk(QStringLiteral("t_conn_color_reason_err_001"),
                         QStringLiteral("La validación de la conexión ha fallado."),
                         QStringLiteral("Connection validation failed."),
                         QStringLiteral("连接校验失败。"))
                   : st.detail.trimmed();
    }
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = rx.match(st.zfsVersion.trimmed());
    const bool zfsTooOld = m.hasMatch()
                           && m.captured(1).toInt() == 2
                           && (m.captured(2).toInt() < 3
                               || (m.captured(2).toInt() == 3
                                   && (m.captured(3).isEmpty() ? 0 : m.captured(3).toInt()) < 3));
    if (zfsTooOld) {
        return trk(QStringLiteral("t_conn_color_reason_zfs_old_001"),
                   QStringLiteral("La versión de OpenZFS es demasiado antigua (mínimo recomendado: 2.3.3)."),
                   QStringLiteral("The OpenZFS version is too old (recommended minimum: 2.3.3)."),
                   QStringLiteral("OpenZFS 版本过旧（建议至少 2.3.3）。"));
    }
    if (!st.missingUnixCommands.isEmpty()) {
        return trk(QStringLiteral("t_conn_color_reason_cmds_001"),
                   QStringLiteral("Faltan comandos auxiliares requeridos: %1"),
                   QStringLiteral("Required helper commands are missing: %1"),
                   QStringLiteral("缺少必需的辅助命令：%1"))
            .arg(st.missingUnixCommands.join(QStringLiteral(", ")));
    }
    return QString();
}

QString MainWindow::connectionStateTooltipHtml(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        return QString();
    }
    const ConnectionProfile& p = m_profiles[connIdx];
    const ConnectionRuntimeState& st = m_states[connIdx];
    const bool disconnected = isConnectionDisconnected(connIdx);
    const QString osHint = (p.osType + QStringLiteral(" ") + st.osLine).trimmed().toLower();
    const bool windowsSshConn =
        p.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
        && osHint.contains(QStringLiteral("windows"));
    QStringList lines;
    lines << QStringLiteral("Host: %1").arg(p.host);
    lines << QStringLiteral("Port: %1").arg(p.port);
    lines << QStringLiteral("Estado: %1").arg(st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed());
    const QString colorReason = connectionStateColorReason(connIdx);
    if (!colorReason.isEmpty()) {
        lines << QStringLiteral("Motivo del color: %1").arg(colorReason);
    }
    lines << QStringLiteral("Sistema operativo: %1")
                 .arg(st.osLine.trimmed().isEmpty() ? QStringLiteral("-") : st.osLine.trimmed());
    lines << QStringLiteral("Método de conexión: %1")
                 .arg(st.connectionMethod.trimmed().isEmpty() ? p.connType : st.connectionMethod.trimmed());
    lines << QStringLiteral("OpenZFS: %1")
                 .arg(st.zfsVersionFull.trimmed().isEmpty()
                          ? (st.zfsVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                               : QStringLiteral("OpenZFS %1").arg(st.zfsVersion.trimmed()))
                          : st.zfsVersionFull.trimmed());
    lines << QStringLiteral("GSA: %1")
                 .arg(!st.gsaInstalled ? QStringLiteral("no instalado")
                                       : QStringLiteral("%1 | %2 | %3")
                                             .arg(st.gsaVersion.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                    : st.gsaVersion.trimmed(),
                                                  st.gsaScheduler.trimmed().isEmpty() ? QStringLiteral("-")
                                                                                      : st.gsaScheduler.trimmed(),
                                                  st.gsaActive ? QStringLiteral("activo")
                                                               : QStringLiteral("inactivo")));
    lines << QStringLiteral("Conexiones dadas de alta en GSA: %1")
                 .arg(st.gsaKnownConnections.isEmpty()
                          ? QStringLiteral("(ninguna)")
                          : st.gsaKnownConnections.join(QStringLiteral(", ")));
    lines << QStringLiteral("Conexiones requeridas por GSA: %1")
                 .arg(st.gsaRequiredConnections.isEmpty()
                          ? QStringLiteral("(ninguna)")
                          : st.gsaRequiredConnections.join(QStringLiteral(", ")));
    if (st.gsaNeedsAttention && !st.gsaAttentionReasons.isEmpty()) {
        lines << QStringLiteral("Atención GSA: %1")
                     .arg(st.gsaAttentionReasons.join(QStringLiteral(", ")));
    }
    lines << QStringLiteral("Comandos detectados: %1")
                 .arg(st.detectedUnixCommands.isEmpty() ? QStringLiteral("(ninguno)")
                                                        : st.detectedUnixCommands.join(QStringLiteral(", ")));
    lines << QStringLiteral("Comandos no detectados: %1")
                 .arg(st.missingUnixCommands.isEmpty() ? QStringLiteral("(ninguno)")
                                                       : st.missingUnixCommands.join(QStringLiteral(", ")));
    lines << QStringLiteral("Plataforma instalación auxiliar: %1")
                 .arg(st.helperPlatformLabel.trimmed().isEmpty() ? QStringLiteral("-")
                                                                 : st.helperPlatformLabel.trimmed());
    lines << QStringLiteral("Gestor de paquetes: %1")
                 .arg(st.helperPackageManagerLabel.trimmed().isEmpty()
                          ? QStringLiteral("-")
                          : QStringLiteral("%1%2")
                                .arg(st.helperPackageManagerLabel.trimmed(),
                                     st.helperPackageManagerDetected ? QStringLiteral(" (detectado)")
                                                                     : QStringLiteral(" (no detectado)")));
    lines << QStringLiteral("Instalación asistida: %1")
                 .arg(st.helperInstallSupported ? QStringLiteral("sí") : QStringLiteral("no"));
    lines << QStringLiteral("Comandos instalables desde ZFSMgr: %1")
                 .arg(st.helperInstallableCommands.isEmpty()
                          ? QStringLiteral("(ninguno)")
                          : st.helperInstallableCommands.join(QStringLiteral(", ")));
    lines << QStringLiteral("Comandos no soportados por instalador: %1")
                 .arg(st.helperUnsupportedCommands.isEmpty()
                          ? QStringLiteral("(ninguno)")
                          : st.helperUnsupportedCommands.join(QStringLiteral(", ")));
    if (!st.helperInstallReason.trimmed().isEmpty()) {
        lines << QStringLiteral("Motivo instalación asistida: %1").arg(st.helperInstallReason.trimmed());
    }
    if (st.commandsLayer.trimmed().compare(QStringLiteral("Powershell"), Qt::CaseInsensitive) == 0
        && !st.powershellFallbackCommands.isEmpty()) {
        lines << QStringLiteral("Comandos PowerShell usados: %1")
                     .arg(st.powershellFallbackCommands.join(QStringLiteral(", ")));
    }
    if (windowsSshConn && !disconnected
        && st.status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) != 0) {
        lines << QString();
        lines << QStringLiteral("PowerShell para habilitar OpenSSH Server:");
        lines << QStringLiteral("# Install OpenSSH Server");
        lines << QStringLiteral("Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0");
        lines << QString();
        lines << QStringLiteral("# Start and set to Automatic");
        lines << QStringLiteral("Start-Service sshd");
        lines << QStringLiteral("Set-Service -Name sshd -StartupType 'Automatic'");
    }
    QStringList nonImportable;
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        if (poolName.isEmpty()) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp == QStringLiteral("ONLINE") && !actionTxt.isEmpty()) {
            continue;
        }
        QString reason = pool.reason.trimmed();
        if (reason.isEmpty()) {
            reason = QStringLiteral("-");
        }
        nonImportable << QStringLiteral("%1").arg(poolName);
        nonImportable << QStringLiteral("  Motivo: %1").arg(reason);
    }
    lines << QStringLiteral("Pools no importables:");
    if (nonImportable.isEmpty()) {
        lines << QStringLiteral("  (ninguno)");
    } else {
        lines += nonImportable;
    }
    const QString plain = lines.join(QStringLiteral("\n")).toHtmlEscaped();
    return QStringLiteral("<pre style=\"font-family:'SF Mono','Menlo','Monaco','Consolas','Liberation Mono',monospace; white-space:pre;\">%1</pre>").arg(plain);
}

QString MainWindow::connectionPersistKey(int idx) const {
    if (idx < 0 || idx >= m_profiles.size()) {
        return QString();
    }
    const QString id = m_profiles[idx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return m_profiles[idx].name.trimmed().toLower();
}

bool MainWindow::isConnectionDisconnected(int idx) const {
    const QString key = connectionPersistKey(idx);
    return !key.isEmpty() && m_disconnectedConnectionKeys.contains(key);
}

void MainWindow::setConnectionDisconnected(int idx, bool disconnected) {
    const QString key = connectionPersistKey(idx);
    if (key.isEmpty()) {
        return;
    }
    if (disconnected) {
        m_disconnectedConnectionKeys.insert(key);
    } else {
        m_disconnectedConnectionKeys.remove(key);
    }
    saveUiSettings();
}

bool MainWindow::isConnectionRedirectedToLocal(int idx) const {
    if (idx < 0 || idx >= m_profiles.size() || idx >= m_states.size()) {
        return false;
    }
    if (isLocalConnection(idx)) {
        return false;
    }
    const ConnectionRuntimeState& st = m_states[idx];
    if (st.status.trimmed().toUpper() != QStringLiteral("OK")) {
        return false;
    }

    QString localUuid = m_localMachineUuid.trimmed().toLower();
    if (localUuid.isEmpty()) {
        for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
            if (!isLocalConnection(i)) {
                continue;
            }
            const QString cand = m_states[i].machineUuid.trimmed().toLower();
            if (!cand.isEmpty()) {
                localUuid = cand;
                break;
            }
        }
    }
    const QString remoteUuid = st.machineUuid.trimmed().toLower();
    if (!localUuid.isEmpty() && !remoteUuid.isEmpty()) {
        return localUuid == remoteUuid;
    }
    return isLocalHostForUi(m_profiles[idx].host);
}

namespace {
int connectionIndexForRow(const QTableWidget* table, int row) {
    if (!table || row < 0 || row >= table->rowCount()) {
        return -1;
    }
    for (int col = table->columnCount() - 1; col >= 0; --col) {
        const QTableWidgetItem* it = table->item(row, col);
        if (!it) {
            continue;
        }
        bool ok = false;
        const int idx = it->data(Qt::UserRole).toInt(&ok);
        if (ok) {
            return idx;
        }
    }
    return -1;
}

int selectedConnectionRow(const QTableWidget* table) {
    if (!table) {
        return -1;
    }
    int row = table->currentRow();
    if (row >= 0) {
        return connectionIndexForRow(table, row);
    }
    const auto ranges = table->selectedRanges();
    if (!ranges.isEmpty()) {
        return connectionIndexForRow(table, ranges.first().topRow());
    }
    return -1;
}

int rowForConnectionIndex(const QTableWidget* table, int connIdx) {
    if (!table || connIdx < 0) {
        return -1;
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        if (connectionIndexForRow(table, row) == connIdx) {
            return row;
        }
    }
    return -1;
}

QString connPersistKeyFromProfiles(const QVector<ConnectionProfile>& profiles, int connIdx) {
    if (connIdx < 0 || connIdx >= profiles.size()) {
        return QString();
    }
    const QString id = profiles[connIdx].id.trimmed();
    if (!id.isEmpty()) {
        return id.toLower();
    }
    return profiles[connIdx].name.trimmed().toLower();
}

struct ConnectivityProbeResult {
    QString text;
    QString tooltip;
    QString detail;
    bool ok{false};
};

QString connectivityMatrixRemoteProbe(const ConnectionProfile& target, bool verbose = false) {
    if (target.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    const QString unixPathPrefix =
        QStringLiteral("PATH=\"/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin:$PATH\"; export PATH; ");
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(QStringLiteral("echo ZFSMGR_CONNECT_OK"));
    QString sshBase = mwhelpers::sshBaseCommand(target);
    if (verbose) {
        sshBase.replace(QStringLiteral("-o LogLevel=ERROR"),
                        QStringLiteral("-o LogLevel=DEBUG3 -vvv"));
    }
    if (!target.password.trimmed().isEmpty()) {
        return unixPathPrefix + QStringLiteral(
                   "if command -v sshpass >/dev/null 2>&1; then "
                   "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
                   "-o PreferredAuthentications=password,keyboard-interactive,publickey "
                   "-o NumberOfPasswordPrompts=1 %3 %4; "
                   "else echo %5 >&2; exit 127; fi")
            .arg(mwhelpers::shSingleQuote(target.password),
                 sshBase,
                 targetHost,
                 targetCmd,
                 mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible para esta prueba")));
    }
    return unixPathPrefix + sshBase + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd;
}

QString connectivityMatrixRsyncProbe(const ConnectionProfile& target) {
    if (target.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    const QString unixPathPrefix =
        QStringLiteral("PATH=\"/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/local/zfs/bin:/usr/sbin:/sbin:/usr/bin:/bin:$PATH\"; export PATH; ");
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(
        QStringLiteral("if command -v rsync >/dev/null 2>&1; then echo ZFSMGR_RSYNC_OK; else echo ZFSMGR_RSYNC_MISSING >&2; exit 127; fi"));
    const QString baseProbe = target.password.trimmed().isEmpty()
        ? unixPathPrefix + mwhelpers::sshBaseCommand(target) + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd
        : unixPathPrefix + QStringLiteral(
              "if command -v sshpass >/dev/null 2>&1; then "
              "SSHPASS=%1 sshpass -e %2 -o BatchMode=no "
              "-o PreferredAuthentications=password,keyboard-interactive,publickey "
              "-o NumberOfPasswordPrompts=1 %3 %4; "
              "else echo %5 >&2; exit 127; fi")
              .arg(mwhelpers::shSingleQuote(target.password),
                   mwhelpers::sshBaseCommand(target),
                   targetHost,
                   targetCmd,
                   mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: sshpass no disponible para esta prueba")));

    return QStringLiteral(
               "if command -v rsync >/dev/null 2>&1; then "
               "%1; "
               "else echo %2 >&2; exit 127; fi")
        .arg(baseProbe,
             mwhelpers::shSingleQuote(QStringLiteral("ZFSMgr: rsync no disponible en origen para esta prueba")));
}
}

QString MainWindow::connectionDisplayModeForIndex(int connIdx) const {
    if (connIdx < 0) {
        return QString();
    }
    return (connIdx == m_topDetailConnIdx) ? QStringLiteral("source") : QString();
}

int MainWindow::connectionIndexByNameOrId(const QString& value) const {
    const QString wanted = value.trimmed();
    if (wanted.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const ConnectionProfile& p = m_profiles[i];
        if (p.name.trimmed().compare(wanted, Qt::CaseInsensitive) == 0
            || p.id.trimmed().compare(wanted, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

bool MainWindow::connectionsReferToSameMachine(int a, int b) const {
    if (a < 0 || a >= m_profiles.size() || b < 0 || b >= m_profiles.size()) {
        return false;
    }
    QString ua = m_profiles[a].machineUid.trimmed().toLower();
    QString ub = m_profiles[b].machineUid.trimmed().toLower();
    if (ua.isEmpty() && a < m_states.size()) {
        ua = m_states[a].machineUuid.trimmed().toLower();
    }
    if (ub.isEmpty() && b < m_states.size()) {
        ub = m_states[b].machineUuid.trimmed().toLower();
    }
    return !ua.isEmpty() && !ub.isEmpty() && ua == ub;
}

void MainWindow::withConnContentContext(QTreeWidget* tree,
                                        const QString& token,
                                        const std::function<void()>& fn) {
    if (!fn) {
        return;
    }
    QTreeWidget* prevTree = m_connContentTree;
    const QString prevToken = m_connContentToken;
    if (tree) {
        m_connContentTree = tree;
    }
    if (!token.isNull()) {
        m_connContentToken = token;
    }
    fn();
    m_connContentTree = prevTree;
    m_connContentToken = prevToken;
}

int MainWindow::equivalentSshForLocal(int localIdx) const {
    if (localIdx < 0 || localIdx >= m_profiles.size() || !isLocalConnection(localIdx)) {
        return -1;
    }
    QString localUid = m_profiles[localIdx].machineUid.trimmed().toLower();
    if (localUid.isEmpty() && localIdx < m_states.size()) {
        localUid = m_states[localIdx].machineUuid.trimmed().toLower();
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (i == localIdx || isLocalConnection(i)) {
            continue;
        }
        const ConnectionProfile& candidate = m_profiles[i];
        if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString candUid = candidate.machineUid.trimmed().toLower();
        if (!localUid.isEmpty() && !candUid.isEmpty() && candUid == localUid) {
            return i;
        }
        if (isConnectionRedirectedToLocal(i)) {
            return i;
        }
    }
    return -1;
}

bool MainWindow::canSshBetweenConnections(int rowIdx, int colIdx, QString* errorOut, int* effectiveDstIdxOut) {
    if (effectiveDstIdxOut) {
        *effectiveDstIdxOut = -1;
    }
    auto fail = [errorOut](const QString& msg) {
        if (errorOut) {
            *errorOut = msg;
        }
        return false;
    };
    if (rowIdx < 0 || rowIdx >= m_profiles.size() || colIdx < 0 || colIdx >= m_profiles.size()) {
        return fail(QStringLiteral("indices inválidos"));
    }
    const bool srcOk = rowIdx < m_states.size() && m_states[rowIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
    const bool dstOk = colIdx < m_states.size() && m_states[colIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
    if (!srcOk || !dstOk) {
        return fail(trk(QStringLiteral("t_connectivity_notready_001"),
                        QStringLiteral("La conexión origen o destino no está en estado OK."),
                        QStringLiteral("The source or target connection is not in OK state."),
                        QStringLiteral("源连接或目标连接不是 OK 状态。")));
    }
    int effectiveIdx = colIdx;
    if (isLocalConnection(colIdx)) {
        const int sshIdx = equivalentSshForLocal(colIdx);
        if (sshIdx < 0) {
            return fail(trk(QStringLiteral("t_connectivity_local_no_ssh_001"),
                            QStringLiteral("Local no tiene una conexión SSH equivalente para comprobarla remotamente."),
                            QStringLiteral("Local has no equivalent SSH connection for remote probing."),
                            QStringLiteral("本地连接没有可用于远程探测的等效 SSH 连接。")));
        }
        effectiveIdx = sshIdx;
    }
    if (effectiveDstIdxOut) {
        *effectiveDstIdxOut = effectiveIdx;
    }
    if (rowIdx == colIdx || rowIdx == effectiveIdx || connectionsReferToSameMachine(rowIdx, colIdx) || connectionsReferToSameMachine(rowIdx, effectiveIdx)) {
        if (errorOut) {
            errorOut->clear();
        }
        return true;
    }
    const ConnectionProfile& src = m_profiles[rowIdx];
    const ConnectionProfile& effectiveDst = m_profiles[effectiveIdx];
    if (effectiveDst.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
        return fail(trk(QStringLiteral("t_connectivity_unsupported_target_001"),
                        QStringLiteral("Solo se comprueba conectividad SSH hacia conexiones SSH/Local."),
                        QStringLiteral("Only SSH connectivity to SSH/Local connections is checked."),
                        QStringLiteral("只检查到 SSH/本地连接的 SSH 连通性。")));
    }
    if (src.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        return fail(trk(QStringLiteral("t_connectivity_unsupported_source_001"),
                        QStringLiteral("No se comprueba conectividad saliente desde conexiones PSRP."),
                        QStringLiteral("Outgoing connectivity is not checked from PSRP connections."),
                        QStringLiteral("不检查来自 PSRP 连接的出站连通性。")));
    }
    const QString sshCmd = connectivityMatrixRemoteProbe(effectiveDst);
    if (sshCmd.trimmed().isEmpty()) {
        return fail(QStringLiteral("probe SSH vacío"));
    }
    QString sshMerged;
    QString sshDetail;
    const bool sshOk = fetchConnectionProbeOutput(rowIdx,
                                                  QStringLiteral("Probe SSH"),
                                                  sshCmd,
                                                  &sshMerged,
                                                  &sshDetail,
                                                  12000);
    const bool sshProbeOk = sshOk && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));
    if (!sshProbeOk) {
        return fail((sshDetail.isEmpty() ? sshMerged : sshDetail).left(300));
    }
    if (errorOut) {
        errorOut->clear();
    }
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    Q_UNUSED(event);
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::openConnectivityMatrixDialog() {
    if (m_profiles.isEmpty()) {
        QMessageBox::information(this,
                                 trk(QStringLiteral("t_connectivity_title_001"),
                                     QStringLiteral("Conectividad"),
                                     QStringLiteral("Connectivity"),
                                     QStringLiteral("连通性")),
                                 trk(QStringLiteral("t_connectivity_empty_001"),
                                     QStringLiteral("No hay conexiones definidas."),
                                     QStringLiteral("There are no defined connections."),
                                     QStringLiteral("没有已定义的连接。")));
        return;
    }

    auto sameMachine = [this](int a, int b) -> bool {
        if (a < 0 || a >= m_profiles.size() || b < 0 || b >= m_profiles.size()) {
            return false;
        }
        const QString ua = m_profiles[a].machineUid.trimmed().toLower();
        const QString ub = m_profiles[b].machineUid.trimmed().toLower();
        return !ua.isEmpty() && !ub.isEmpty() && ua == ub;
    };
    auto equivalentSshForLocal = [this](int localIdx) -> int {
        if (localIdx < 0 || localIdx >= m_profiles.size() || !isLocalConnection(localIdx)) {
            return -1;
        }
        QString localUid = m_profiles[localIdx].machineUid.trimmed().toLower();
        if (localUid.isEmpty() && localIdx < m_states.size()) {
            localUid = m_states[localIdx].machineUuid.trimmed().toLower();
        }
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == localIdx || isLocalConnection(i)) {
                continue;
            }
            const ConnectionProfile& candidate = m_profiles[i];
            if (candidate.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString candUid = candidate.machineUid.trimmed().toLower();
            if (!localUid.isEmpty() && !candUid.isEmpty() && candUid == localUid) {
                return i;
            }
            if (isConnectionRedirectedToLocal(i)) {
                return i;
            }
        }
        return -1;
    };
    auto probeConnectivity = [&](int rowIdx, int colIdx) -> ConnectivityProbeResult {
        ConnectivityProbeResult result;
        auto composeText = [](const QString& sshState, const QString& rsyncState) -> QString {
            return QStringLiteral("SSH:%1\nrsync:%2").arg(sshState, rsyncState);
        };
        auto explainFailure = [this](const QString& raw, int rc) -> QString {
            const QString merged = raw.trimmed();
            if (merged.contains(QStringLiteral("sshpass no disponible"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_sshpass_001"),
                           QStringLiteral("Motivo: en la conexión origen no está instalado sshpass y el destino requiere autenticación por contraseña."),
                           QStringLiteral("Reason: sshpass is not installed on the source connection and the target requires password authentication."),
                           QStringLiteral("原因：源连接未安装 sshpass，而目标需要密码认证。"));
            }
            if (merged.contains(QStringLiteral("rsync no disponible"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("ZFSMGR_RSYNC_MISSING"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_rsync_001"),
                           QStringLiteral("Motivo: rsync no está disponible en origen o destino."),
                           QStringLiteral("Reason: rsync is not available on source or target."),
                           QStringLiteral("原因：源端或目标端不可用 rsync。"));
            }
            if (merged.contains(QStringLiteral("permission denied"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("publickey"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("authentication"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_auth_001"),
                           QStringLiteral("Motivo: fallo de autenticación SSH hacia el destino."),
                           QStringLiteral("Reason: SSH authentication to the target failed."),
                           QStringLiteral("原因：到目标的 SSH 认证失败。"));
            }
            if (merged.contains(QStringLiteral("could not resolve hostname"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("name or service not known"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_dns_001"),
                           QStringLiteral("Motivo: no se puede resolver el nombre del host destino."),
                           QStringLiteral("Reason: the target hostname cannot be resolved."),
                           QStringLiteral("原因：无法解析目标主机名。"));
            }
            if (merged.contains(QStringLiteral("connection refused"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_refused_001"),
                           QStringLiteral("Motivo: el puerto SSH destino rechaza la conexión."),
                           QStringLiteral("Reason: the target SSH port refused the connection."),
                           QStringLiteral("原因：目标 SSH 端口拒绝了连接。"));
            }
            if (merged.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)
                || merged.contains(QStringLiteral("operation timed out"), Qt::CaseInsensitive)) {
                return trk(QStringLiteral("t_connectivity_reason_timeout_001"),
                           QStringLiteral("Motivo: tiempo de espera agotado al conectar con el destino."),
                           QStringLiteral("Reason: timed out while connecting to the target."),
                           QStringLiteral("原因：连接目标时超时。"));
            }
            if (!merged.isEmpty()) {
                return trk(QStringLiteral("t_connectivity_reason_raw_001"),
                           QStringLiteral("Motivo: %1"),
                           QStringLiteral("Reason: %1"),
                           QStringLiteral("原因：%1"))
                    .arg(merged.left(300));
            }
            return trk(QStringLiteral("t_connectivity_reason_exit_001"),
                       QStringLiteral("Motivo: la comprobación terminó con código %1."),
                       QStringLiteral("Reason: the probe finished with exit code %1."),
                       QStringLiteral("原因：探测以退出码 %1 结束。"))
                .arg(rc);
        };
        if (rowIdx < 0 || rowIdx >= m_profiles.size() || colIdx < 0 || colIdx >= m_profiles.size()) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            return result;
        }
        const ConnectionProfile& src = m_profiles[rowIdx];
        const ConnectionProfile& dst = m_profiles[colIdx];
        const bool srcOk = rowIdx < m_states.size() && m_states[rowIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
        const bool dstOk = colIdx < m_states.size() && m_states[colIdx].status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
        if (!srcOk || !dstOk) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_notready_001"),
                                 QStringLiteral("La conexión origen o destino no está en estado OK."),
                                 QStringLiteral("The source or target connection is not in OK state."),
                                 QStringLiteral("源连接或目标连接不是 OK 状态。"));
            result.detail = result.tooltip;
            return result;
        }
        if (rowIdx == colIdx || sameMachine(rowIdx, colIdx)) {
            result.text = composeText(QStringLiteral("✓"), QStringLiteral("✓"));
            result.tooltip = trk(QStringLiteral("t_connectivity_same_machine_001"),
                                 QStringLiteral("Misma máquina."),
                                 QStringLiteral("Same machine."),
                                 QStringLiteral("同一台机器。"));
            result.detail = result.tooltip;
            result.ok = true;
            return result;
        }
        ConnectionProfile effectiveDst = dst;
        QString targetLabel = dst.name;
        if (dst.connType.trimmed().compare(QStringLiteral("LOCAL"), Qt::CaseInsensitive) == 0) {
            const int sshIdx = equivalentSshForLocal(colIdx);
            if (sshIdx >= 0) {
                effectiveDst = m_profiles[sshIdx];
                targetLabel = m_profiles[sshIdx].name;
                if (rowIdx == sshIdx || sameMachine(rowIdx, sshIdx)) {
                    result.text = composeText(QStringLiteral("✓"), QStringLiteral("✓"));
                    result.tooltip = trk(QStringLiteral("t_connectivity_same_machine_001"),
                                         QStringLiteral("Misma máquina."),
                                         QStringLiteral("Same machine."),
                                         QStringLiteral("同一台机器。"));
                    result.detail = result.tooltip;
                    result.ok = true;
                    return result;
                }
            } else {
                result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
                result.tooltip = trk(QStringLiteral("t_connectivity_local_no_ssh_001"),
                                     QStringLiteral("Local no tiene una conexión SSH equivalente para comprobarla remotamente."),
                                     QStringLiteral("Local has no equivalent SSH connection for remote probing."),
                                     QStringLiteral("本地连接没有可用于远程探测的等效 SSH 连接。"));
                result.detail = result.tooltip;
                return result;
            }
        }
        if (effectiveDst.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_unsupported_target_001"),
                                 QStringLiteral("Solo se comprueba conectividad SSH hacia conexiones SSH/Local."),
                                 QStringLiteral("Only SSH connectivity to SSH/Local connections is checked."),
                                 QStringLiteral("只检查到 SSH/本地连接的 SSH 连通性。"));
            result.detail = result.tooltip;
            return result;
        }
        if (src.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_unsupported_source_001"),
                                 QStringLiteral("No se comprueba conectividad saliente desde conexiones PSRP."),
                                 QStringLiteral("Outgoing connectivity is not checked from PSRP connections."),
                                 QStringLiteral("不检查来自 PSRP 连接的出站连通性。"));
            result.detail = result.tooltip;
            return result;
        }
        const QString sshCmd = connectivityMatrixRemoteProbe(effectiveDst);
        if (sshCmd.trimmed().isEmpty()) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            return result;
        }
        QString sshMerged;
        QString sshDetail;
        const bool sshOk = fetchConnectionProbeOutput(rowIdx,
                                                      QStringLiteral("Probe SSH"),
                                                      sshCmd,
                                                      &sshMerged,
                                                      &sshDetail,
                                                      12000);
        const bool sshProbeOk = sshOk && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));

        QString rsyncState = QStringLiteral("-");
        QStringList tooltipLines;
        if (sshProbeOk) {
            tooltipLines << trk(QStringLiteral("t_connectivity_ok_001"),
                                QStringLiteral("Conectividad SSH verificada hacia %1."),
                                QStringLiteral("SSH connectivity verified to %1."),
                                QStringLiteral("到 %1 的 SSH 连通性已验证。"))
                                .arg(targetLabel);
            const QString rsyncCmd = connectivityMatrixRsyncProbe(effectiveDst);
            if (!rsyncCmd.trimmed().isEmpty()) {
                QString rsyncMerged;
                QString rsyncDetail;
                const bool rsyncOk = fetchConnectionProbeOutput(rowIdx,
                                                                QStringLiteral("Probe rsync"),
                                                                rsyncCmd,
                                                                &rsyncMerged,
                                                                &rsyncDetail,
                                                                12000);
                if (rsyncOk && rsyncMerged.contains(QStringLiteral("ZFSMGR_RSYNC_OK"))) {
                    rsyncState = QStringLiteral("✓");
                    tooltipLines << trk(QStringLiteral("t_connectivity_rsync_ok_001"),
                                        QStringLiteral("rsync disponible en origen y destino."),
                                        QStringLiteral("rsync available on source and target."),
                                        QStringLiteral("源端和目标端均可用 rsync。"));
                } else {
                    rsyncState = QStringLiteral("✗");
                    tooltipLines << explainFailure(rsyncDetail.isEmpty() ? rsyncMerged : rsyncDetail, rsyncOk ? 0 : -1);
                }
            }
            result.text = composeText(QStringLiteral("✓"), rsyncState);
            result.tooltip = tooltipLines.join(QStringLiteral("\n"));
            result.detail = result.tooltip;
            result.ok = true;
            return result;
        }
        result.text = composeText(QStringLiteral("✗"), QStringLiteral("-"));
        result.tooltip = explainFailure(sshDetail.isEmpty() ? sshMerged : sshDetail, sshOk ? 0 : -1);
        QString sshVerboseMerged;
        QString sshVerboseDetail;
        const QString sshVerboseCmd = connectivityMatrixRemoteProbe(effectiveDst, true);
        if (!sshVerboseCmd.trimmed().isEmpty()) {
            fetchConnectionProbeOutput(rowIdx,
                                       QStringLiteral("Probe SSH verbose"),
                                       sshVerboseCmd,
                                       &sshVerboseMerged,
                                       &sshVerboseDetail,
                                       12000);
        }
        result.detail = QStringList{
                            result.tooltip,
                            QString(),
                            trk(QStringLiteral("t_connectivity_probe_log_001"),
                                QStringLiteral("Log del probe:"),
                                QStringLiteral("Probe log:"),
                                QStringLiteral("探测日志：")),
                            (sshDetail.isEmpty() ? sshMerged : sshDetail).trimmed(),
                            QString(),
                            QStringLiteral("ssh -vvv:"),
                            (sshVerboseDetail.isEmpty() ? sshVerboseMerged : sshVerboseDetail).trimmed()
                        }.join(QStringLiteral("\n"));
        return result;
    };

    QDialog dlg(this);
    dlg.setWindowTitle(trk(QStringLiteral("t_connectivity_title_001"),
                           QStringLiteral("Conectividad"),
                           QStringLiteral("Connectivity"),
                           QStringLiteral("连通性")));
    dlg.resize(760, 460);
    auto* layout = new QVBoxLayout(&dlg);
    auto* matrix = new QTableWidget(&dlg);
    matrix->setColumnCount(m_profiles.size());
    matrix->setRowCount(m_profiles.size());
    QStringList labels;
    for (const ConnectionProfile& p : m_profiles) {
        labels << (p.name.trimmed().isEmpty() ? p.id : p.name);
    }
    matrix->setHorizontalHeaderLabels(labels);
    matrix->setVerticalHeaderLabels(labels);
    matrix->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    matrix->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
    matrix->setSelectionMode(QAbstractItemView::SingleSelection);
    matrix->setSelectionBehavior(QAbstractItemView::SelectItems);
    matrix->setAlternatingRowColors(false);
    auto* detailLabel = new QLabel(
        trk(QStringLiteral("t_connectivity_detail_title_001"),
            QStringLiteral("Detalle de la casilla seleccionada"),
            QStringLiteral("Selected cell detail"),
            QStringLiteral("所选单元格详情")),
        &dlg);
    auto* detailView = new QPlainTextEdit(&dlg);
    detailView->setReadOnly(true);
    detailView->setMaximumBlockCount(2000);
    detailView->setPlaceholderText(
        trk(QStringLiteral("t_connectivity_detail_ph_001"),
            QStringLiteral("Seleccione una celda para ver su detalle."),
            QStringLiteral("Select a cell to view its detail."),
            QStringLiteral("选择一个单元格以查看详情。")));
    beginUiBusy();
    m_connectivityMatrixInProgress = true;
    updateConnectivityMatrixButtonState();
    for (int r = 0; r < m_profiles.size(); ++r) {
        for (int c = 0; c < m_profiles.size(); ++c) {
            const ConnectivityProbeResult probe = probeConnectivity(r, c);
            auto* item = new QTableWidgetItem(probe.text);
            item->setTextAlignment(Qt::AlignCenter);
            item->setToolTip(probe.tooltip);
            item->setData(Qt::UserRole, probe.detail);
            if (probe.ok) {
                item->setForeground(QBrush(QColor(QStringLiteral("#1d6f42"))));
            } else if (probe.text.contains(QStringLiteral("✗"))) {
                item->setForeground(QBrush(QColor(QStringLiteral("#8b1e1e"))));
                item->setBackground(QBrush(QColor(QStringLiteral("#fde7e7"))));
            }
            matrix->setItem(r, c, item);
            qApp->processEvents();
        }
    }
    m_connectivityMatrixInProgress = false;
    updateConnectivityMatrixButtonState();
    endUiBusy();
    layout->addWidget(matrix, 1);
    layout->addWidget(detailLabel);
    layout->addWidget(detailView);
    connect(matrix, &QTableWidget::currentItemChanged, &dlg, [detailView](QTableWidgetItem* current, QTableWidgetItem*) {
        if (!detailView) {
            return;
        }
        detailView->setPlainText(current ? current->data(Qt::UserRole).toString() : QString());
    });
    if (matrix->rowCount() > 0 && matrix->columnCount() > 0) {
        matrix->setCurrentCell(0, 0);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::showConnectionContextMenu(int connIdx, const QPoint& globalPos) {
    const auto endBusy = [this]() { endUiBusy(); };
    const bool hasConn = (connIdx >= 0 && connIdx < m_profiles.size());
    if (hasConn) {
        setCurrentConnectionInUi(connIdx);
    }
    const bool isDisconnected = hasConn && isConnectionDisconnected(connIdx);
    const bool hasWindowsUnixLayerReady =
        hasConn
        && connIdx < m_states.size()
        && isWindowsConnection(connIdx)
        && m_states[connIdx].unixFromMsysOrMingw
        && m_states[connIdx].missingUnixCommands.isEmpty()
        && !m_states[connIdx].detectedUnixCommands.isEmpty();
    const bool canManageGsa =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && gsaMenuLabelForConnection(connIdx).compare(
               trk(QStringLiteral("t_gsa_ok_001"),
                   QStringLiteral("GSA actualizado y funcionando"),
                   QStringLiteral("GSA updated and running"),
                   QStringLiteral("GSA 已更新并运行中")),
               Qt::CaseInsensitive) != 0;
    const bool canUninstallGsa =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && m_states[connIdx].gsaInstalled;
    const zfsmgr::uilogic::ConnectionContextMenuState menuState =
        zfsmgr::uilogic::buildConnectionContextMenuState(
            hasConn,
            isDisconnected,
            actionsLocked(),
            hasConn && isLocalConnection(connIdx),
            hasConn && isConnectionRedirectedToLocal(connIdx),
            hasConn && isWindowsConnection(connIdx),
            hasWindowsUnixLayerReady,
            canManageGsa,
            canUninstallGsa);

    QMenu menu(this);
    QAction* aNewConn = menu.addAction(
        trk(QStringLiteral("t_new_conn_ctx001"),
            QStringLiteral("Nueva Conexión"),
            QStringLiteral("New Connection"),
            QStringLiteral("新建连接")));
    menu.addSeparator();
    QAction* aConnect = menu.addAction(
        trk(QStringLiteral("t_connect_ctx_001"),
            QStringLiteral("Conectar"),
            QStringLiteral("Connect"),
            QStringLiteral("连接")));
    QAction* aDisconnect = menu.addAction(
        trk(QStringLiteral("t_disconnect_ctx001"),
            QStringLiteral("Desconectar"),
            QStringLiteral("Disconnect"),
            QStringLiteral("断开连接")));
    QAction* aInstallMsys = menu.addAction(
        trk(QStringLiteral("t_install_msys_ctx001"),
            QStringLiteral("Instalar MSYS2"),
            QStringLiteral("Install MSYS2"),
            QStringLiteral("安装 MSYS2")));
    QAction* aInstallHelpers = menu.addAction(
        trk(QStringLiteral("t_install_helpers_ctx001"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")));
    QMenu* refreshMenu = menu.addMenu(
        trk(QStringLiteral("t_refresh_conn_ctx001"),
            QStringLiteral("Refrescar"),
            QStringLiteral("Refresh"),
            QStringLiteral("刷新")));
    QAction* aRefresh = refreshMenu->addAction(
        trk(QStringLiteral("t_refresh_this_conn_001"),
            QStringLiteral("Esta conexión"),
            QStringLiteral("This connection"),
            QStringLiteral("此连接")));
    QAction* aRefreshAll = refreshMenu->addAction(
        trk(QStringLiteral("t_refresh_all_001"),
            QStringLiteral("Todas las conexiones"),
            QStringLiteral("All connections"),
            QStringLiteral("所有连接")));
    QMenu* gsaMenu = menu.addMenu(QStringLiteral("GSA"));
    QAction* aManageGsa = gsaMenu->addAction(hasConn ? gsaMenuLabelForConnection(connIdx)
                                                     : trk(QStringLiteral("t_gsa_install_001"),
                                                           QStringLiteral("Instalar gestor de snapshots"),
                                                           QStringLiteral("Install snapshot manager"),
                                                           QStringLiteral("安装快照管理器")));
    QAction* aUninstallGsa = gsaMenu->addAction(
        trk(QStringLiteral("t_gsa_uninstall_001"),
            QStringLiteral("Desinstalar el GSA"),
            QStringLiteral("Uninstall GSA"),
            QStringLiteral("卸载 GSA")));
    QAction* aEdit = menu.addAction(
        trk(QStringLiteral("t_edit_conn_ctx001"),
            QStringLiteral("Editar"),
            QStringLiteral("Edit"),
            QStringLiteral("编辑")));
    QAction* aDelete = menu.addAction(
        trk(QStringLiteral("t_del_conn_ctx001"),
            QStringLiteral("Borrar"),
            QStringLiteral("Delete"),
            QStringLiteral("删除")));
    QAction* aNewPool = menu.addAction(
        trk(QStringLiteral("t_new_pool_ctx_001"),
            QStringLiteral("Nuevo Pool"),
            QStringLiteral("New Pool"),
            QStringLiteral("新建存储池")));
    aConnect->setEnabled(menuState.canConnect);
    aDisconnect->setEnabled(menuState.canDisconnect);
    aInstallMsys->setEnabled(menuState.canInstallMsys);
    const bool canInstallHelpers =
        hasConn && !actionsLocked() && !isDisconnected
        && connIdx < m_states.size()
        && m_states[connIdx].helperInstallSupported;
    aInstallHelpers->setEnabled(canInstallHelpers);
    aManageGsa->setEnabled(menuState.canManageGsa);
    aUninstallGsa->setEnabled(menuState.canUninstallGsa);
    gsaMenu->setEnabled(menuState.gsaSubmenuEnabled);
    aRefresh->setEnabled(menuState.canRefreshThis);
    aRefreshAll->setEnabled(menuState.canRefreshAll);
    aEdit->setEnabled(menuState.canEditDelete);
    aDelete->setEnabled(menuState.canEditDelete);
    aNewConn->setEnabled(menuState.canNewConnection);
    aNewPool->setEnabled(menuState.canNewPool);

    endBusy();
    QAction* chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }
    if (chosen == aConnect && hasConn) {
        logUiAction(QStringLiteral("Conectar conexión (menú conexiones)"));
        beginTransientUiBusy(
            trk(QStringLiteral("t_connecting_conn_busy_001"),
                QStringLiteral("Conectando %1..."),
                QStringLiteral("Connecting %1..."),
                QStringLiteral("正在连接 %1...")).arg(m_profiles[connIdx].name));
        setConnectionDisconnected(connIdx, false);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como conectada: %1").arg(m_profiles[connIdx].name));
        rebuildConnectionsTable();
        populateAllPoolsTables();
        refreshConnectionByIndex(connIdx);
        endTransientUiBusy();
    } else if (chosen == aDisconnect && hasConn) {
        setConnectionDisconnected(connIdx, true);
        appLog(QStringLiteral("NORMAL"), QStringLiteral("Conexión marcada como desconectada: %1").arg(m_profiles[connIdx].name));
        rebuildConnectionsTable();
        populateAllPoolsTables();
    } else if (chosen == aRefresh) {
        logUiAction(QStringLiteral("Refrescar conexión (menú conexiones)"));
        refreshSelectedConnection();
    } else if (chosen == aEdit) {
        logUiAction(QStringLiteral("Editar conexión (menú conexiones)"));
        editConnection();
    } else if (chosen == aDelete) {
        logUiAction(QStringLiteral("Borrar conexión (menú conexiones)"));
        deleteConnection();
    } else if (chosen == aInstallMsys) {
        logUiAction(QStringLiteral("Instalar MSYS2 (menú conexiones)"));
        installMsysForSelectedConnection();
    } else if (chosen == aInstallHelpers) {
        logUiAction(QStringLiteral("Instalar comandos auxiliares (menú conexiones)"));
        installHelperCommandsForSelectedConnection();
    } else if (chosen == aManageGsa && hasConn) {
        logUiAction(QStringLiteral("Gestionar GSA (menú conexiones)"));
        installOrUpdateGsaForConnection(connIdx);
    } else if (chosen == aUninstallGsa && hasConn) {
        logUiAction(QStringLiteral("Desinstalar GSA (menú conexiones)"));
        uninstallGsaForConnection(connIdx);
    } else if (chosen == aRefreshAll) {
        logUiAction(QStringLiteral("Refrescar todas las conexiones (menú conexiones)"));
        refreshAllConnections();
    } else if (chosen == aNewConn) {
        logUiAction(QStringLiteral("Nueva conexión (menú conexiones)"));
        createConnection();
    } else if (chosen == aNewPool) {
        logUiAction(QStringLiteral("Nuevo pool (menú conexiones)"));
        createPoolForSelectedConnection();
    }
}

void MainWindow::syncConnectionDisplaySelectors() {
    // Árbol unificado: ya no hay selectores O/D en la tabla.
}

void MainWindow::applyConnectionDisplayMode(int connIdx, const QString& modeRaw) {
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString mode = modeRaw.trimmed().toLower();
    if (isConnectionDisconnected(connIdx)) {
        return;
    }

    if (mode != QStringLiteral("source") && mode != QStringLiteral("both")) {
        return;
    }
    const int prevTop = m_topDetailConnIdx;
    if (prevTop >= 0 && prevTop != connIdx) {
        saveTopTreeStateForConnection(prevTop);
        const QString topTreeToken = connContentTokenForTree(m_connContentTree);
        if (!topTreeToken.isEmpty()) {
            saveConnContentTreeState(m_connContentTree, topTreeToken);
        }
    }
    m_topDetailConnIdx = connIdx;
    m_forceRestoreTopStateConnIdx = connIdx;
    m_userSelectedConnectionKey = m_profiles[connIdx].id.trimmed().toLower();
    if (m_userSelectedConnectionKey.isEmpty()) {
        m_userSelectedConnectionKey = m_profiles[connIdx].name.trimmed().toLower();
    }
    rebuildConnectionEntityTabs();
    refreshConnectionNodeDetails();
    updateConnectionActionsState();
}

void MainWindow::refreshAllConnections() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_refrescar__7f8af2"),
               QStringLiteral("Refrescar todas las conexiones"),
               QStringLiteral("Refresh all connections"),
               QStringLiteral("刷新所有连接")));
    if (m_profiles.isEmpty()) {
        if (!m_initialRefreshCompleted) {
            m_initialRefreshCompleted = true;
        }
        m_refreshInProgress = false;
        updateBusyCursor();
        updateStatus(QString());
        updateConnectivityMatrixButtonState();
        rebuildConnectionsTable();
        populateAllPoolsTables();
        return;
    }
    const int generation = ++m_refreshGeneration;
    int refreshable = 0;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (!isConnectionDisconnected(i)) {
            ++refreshable;
        }
    }
    m_refreshPending = refreshable;
    m_refreshTotal = refreshable;
    m_refreshInProgress = (m_refreshPending > 0);
    updateBusyCursor();
    updateConnectivityMatrixButtonState();
    if (refreshable <= 0) {
        if (!m_initialRefreshCompleted) {
            m_initialRefreshCompleted = true;
        }
        rebuildConnectionsTable();
        populateAllPoolsTables();
        updateStatus(QString());
        return;
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        if (isConnectionDisconnected(i)) {
            continue;
        }
        const ConnectionProfile profile = m_profiles[i];
        (void)QtConcurrent::run([this, generation, i, profile]() {
            const ConnectionRuntimeState state = refreshConnection(profile);
            QMetaObject::invokeMethod(this, [this, generation, i, state, profile]() {
                onAsyncRefreshResult(generation, i, profile.id, state);
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::refreshSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"),
               trk(QStringLiteral("t_acci_n_en__6facd2"),
                   QStringLiteral("Acción en curso: refresh bloqueado"),
                   QStringLiteral("Action in progress: refresh blocked"),
                   QStringLiteral("操作进行中：刷新被阻止")));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    const int generation = ++m_refreshGeneration;
    m_refreshPending = 1;
    m_refreshTotal = 1;
    m_refreshInProgress = true;
    updateBusyCursor();
    updateConnectivityMatrixButtonState();
    const ConnectionProfile profile = m_profiles[idx];
    (void)QtConcurrent::run([this, generation, idx, profile]() {
        const ConnectionRuntimeState state = refreshConnection(profile);
        QMetaObject::invokeMethod(this, [this, generation, idx, state, profile]() {
            onAsyncRefreshResult(generation, idx, profile.id, state);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onAsyncRefreshResult(int generation, int idx, const QString& connId, const ConnectionRuntimeState& state) {
    if (generation != m_refreshGeneration) {
        return;
    }
    int targetIdx = -1;
    if (idx >= 0 && idx < m_profiles.size() && m_profiles[idx].id == connId) {
        targetIdx = idx;
    } else if (!connId.trimmed().isEmpty()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id == connId) {
                targetIdx = i;
                break;
            }
        }
    }
    if (targetIdx < 0 || targetIdx >= m_states.size()) {
        if (m_refreshPending > 0) {
            --m_refreshPending;
        }
        if (m_refreshPending == 0) {
            onAsyncRefreshDone(generation);
        }
        return;
    }
    const int selectedIdx = currentConnectionIndexFromUi();
    if (state.status.trimmed().compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0
        && !state.machineUuid.trimmed().isEmpty()
        && !isLocalConnection(targetIdx)
        && (m_profiles[targetIdx].connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
            || m_profiles[targetIdx].connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0)) {
        const QString newMachineUid = state.machineUuid.trimmed();
        if (m_profiles[targetIdx].machineUid.trimmed().compare(newMachineUid, Qt::CaseInsensitive) != 0) {
            ConnectionProfile persisted = m_profiles[targetIdx];
            persisted.machineUid = newMachineUid;
            QString persistErr;
            if (m_store.upsertConnection(persisted, persistErr)) {
                m_profiles[targetIdx].machineUid = newMachineUid;
                appLog(QStringLiteral("INFO"),
                       QStringLiteral("machine_uid persistido para %1: %2")
                           .arg(m_profiles[targetIdx].name,
                                newMachineUid));
            } else {
                appLog(QStringLiteral("WARN"),
                       QStringLiteral("No se pudo persistir machine_uid para %1: %2")
                           .arg(m_profiles[targetIdx].name,
                                persistErr.simplified()));
            }
        }
    }
    m_states[targetIdx] = state;
    invalidatePoolDetailsCacheForConnection(targetIdx);
    invalidatePoolAutoSnapshotInfoForConnection(targetIdx);
    cachePoolStatusTextsForConnection(targetIdx, state);
    rebuildConnInfoFor(targetIdx);
    preloadPoolAutoSnapshotInfoForConnection(targetIdx);
    rebuildConnectionsTable();
    if (selectedIdx >= 0) {
        setCurrentConnectionInUi(selectedIdx);
    }
    populateAllPoolsTables();
    if (m_refreshPending > 0) {
        --m_refreshPending;
    }
    if (m_refreshPending == 0) {
        onAsyncRefreshDone(generation);
    }
}

void MainWindow::onAsyncRefreshDone(int generation) {
    if (generation != m_refreshGeneration) {
        return;
    }
    if (!m_initialRefreshCompleted) {
        m_initialRefreshCompleted = true;
    }
    if (currentConnectionIndexFromUi() < 0) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                setCurrentConnectionInUi(i);
                break;
            }
        }
    }
    refreshConnectionNodeDetails();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    m_refreshInProgress = false;
    updateBusyCursor();
    updateStatus(QString());
    updateConnectivityMatrixButtonState();
    if (m_busyOnImportRefresh) {
        m_busyOnImportRefresh = false;
        endUiBusy();
    }
}

void MainWindow::onConnectionSelectionChanged() {
    if (m_connContentTree
        && m_connContentTree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
        updatePoolManagementBoxTitle();
        return;
    }
    QWidget* paintRoot = m_poolDetailTabs ? m_poolDetailTabs : static_cast<QWidget*>(m_rightTabs);
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(false);
    }

    QString selectionKey;
    int idx = m_topDetailConnIdx;
    if (idx < 0) {
        idx = currentConnectionIndexFromUi();
    }
    if (idx >= 0 && isConnectionDisconnected(idx)) {
        idx = -1;
    }
    selectionKey = QStringLiteral("%1").arg(idx);
    if (!selectionKey.isEmpty() && selectionKey == m_lastConnectionSelectionKey) {
        // Evita reconstrucciones redundantes (y consultas SSH) cuando el usuario
        // vuelve a pinchar la misma conexión/tab ya cargada.
        auto detailLoadedFor = [this](int connIdx, QTreeWidget* tree) -> bool {
            if (connIdx < 0) {
                return true;
            }
            if (!tree) {
                return false;
            }
            return tree->topLevelItemCount() > 0;
        };
        const bool topLoaded = detailLoadedFor(m_topDetailConnIdx, m_connContentTree);
        if (topLoaded) {
            updatePoolManagementBoxTitle();
            if (paintRoot) {
                paintRoot->setUpdatesEnabled(true);
                paintRoot->update();
            }
            return;
        }
        // Si falta contenido (p.ej. tras refresh), repoblar una sola vez.
        rebuildConnectionEntityTabs();
        refreshConnectionNodeDetails();
        updatePoolManagementBoxTitle();
        if (paintRoot) {
            paintRoot->setUpdatesEnabled(true);
            paintRoot->update();
        }
        return;
    }
    m_lastConnectionSelectionKey = selectionKey;
    rebuildConnectionEntityTabs();
    populateAllPoolsTables();
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(true);
        paintRoot->update();
    }
}

void MainWindow::rebuildConnContentDetailTree(QTreeWidget* tree,
                                              int connIdx,
                                              bool& rebuildingFlag,
                                              int* forceRestoreConnIdx,
                                              const std::function<void(int)>& saveTreeState,
                                              const std::function<void()>& clearPendingState) {
    if (!tree) {
        return;
    }
    QScopedValueRollback<bool> rebuildingGuard(rebuildingFlag, true);
    const QSignalBlocker blockTree(tree);
    const QString savedStateToken = connContentStateTokenForTree(tree);
    if (!savedStateToken.isEmpty()) {
        saveConnContentTreeStateFor(tree, savedStateToken);
    }
    if (clearPendingState) {
        clearPendingState();
    }
    if (forceRestoreConnIdx && connIdx >= 0 && *forceRestoreConnIdx == connIdx) {
        *forceRestoreConnIdx = -1;
    }
    tree->clear();
    const bool unifiedTree = tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
    if (!unifiedTree
        && (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()
            || isConnectionDisconnected(connIdx))) {
        syncConnContentPropertyColumnsFor(tree, connContentTokenForTree(tree));
        return;
    }
    if (unifiedTree) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            const ConnectionRuntimeState state =
                (i < m_states.size()) ? m_states[i] : ConnectionRuntimeState{};
            populateConnectionPoolsIntoTree(tree, i, state);
        }
    } else {
        const ConnectionRuntimeState st = m_states[connIdx];
        populateConnectionPoolsIntoTree(tree, connIdx, st);
    }
    if (tree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        tree->addTopLevelItem(noPools);
    }
    const QString restoreStateToken = !savedStateToken.isEmpty()
                                          ? savedStateToken
                                          : connContentStateTokenForTree(tree);
    if (!restoreStateToken.isEmpty()) {
        restoreConnContentTreeStateFor(tree, restoreStateToken);
    } else if (tree->topLevelItemCount() > 0) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (item && item->data(0, kIsPoolRootRole).toBool()) {
                item->setExpanded(true);
            }
        }
    }
    if (saveTreeState) {
        saveTreeState(connIdx);
    }
    QPointer<QTreeWidget> safeTree(tree);
    QTimer::singleShot(0, this, [this, safeTree, connIdx]() {
        QTreeWidget* tree = safeTree.data();
        if (!tree) {
            return;
        }
        QString token;
        if (tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool()) {
            token = connContentTokenForTree(tree);
        } else if (connIdx >= 0 && connIdx < m_profiles.size()) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem* root = tree->topLevelItem(i);
                if (!root || !root->data(0, kIsPoolRootRole).toBool()) {
                    continue;
                }
                const int rootConnIdx = root->data(0, Qt::UserRole + 10).toInt();
                const QString poolName = root->data(0, Qt::UserRole + 11).toString().trimmed();
                if (rootConnIdx == connIdx && !poolName.isEmpty()) {
                    token = QStringLiteral("%1::%2").arg(rootConnIdx).arg(poolName);
                    break;
                }
            }
        }
        if (token.isEmpty()) {
            return;
        }
        syncConnContentPoolColumnsFor(tree, token);
    });
}

void MainWindow::updateSecondaryConnectionDetail() {
    // Árbol inferior eliminado en el rediseño global.
}

void MainWindow::saveTopTreeStateForConnection(int connIdx) {
    if (connIdx < 0 || !m_connContentTree) {
        return;
    }
    const ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_connContentTree);
    m_savedTopExpandedKeysByConn[connIdx] = nav.expandedKeys;
    m_savedTopSelectedKeyByConn[connIdx] = nav.selectedKey;
}

void MainWindow::saveBottomTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
}

void MainWindow::restoreTopTreeStateForConnection(int connIdx) {
    if (connIdx < 0 || !m_connContentTree) {
        return;
    }
    ConnTreeNavSnapshot nav;
    nav.expandedKeys = m_savedTopExpandedKeysByConn.value(connIdx);
    nav.selectedKey = m_savedTopSelectedKeyByConn.value(connIdx);
    restoreConnTreeNavSnapshot(m_connContentTree, nav);
}

void MainWindow::restoreBottomTreeStateForConnection(int connIdx) {
    Q_UNUSED(connIdx);
}

void MainWindow::rebuildConnectionEntityTabs() {
    rebuildConnContentDetailTree(m_connContentTree,
                                 m_topDetailConnIdx,
                                 m_rebuildingTopConnContentTree,
                                 &m_forceRestoreTopStateConnIdx,
                                 [this](int connIdx) { saveTopTreeStateForConnection(connIdx); });
}

void MainWindow::populateConnectionPoolsIntoTree(QTreeWidget* tree,
                                                 int connIdx,
                                                 const ConnectionRuntimeState& st) {
    if (!tree || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const bool unifiedTree = tree->property("zfsmgr.groupPoolsByConnectionRoots").toBool();
    if (unifiedTree) {
        QTreeWidgetItem* connRoot = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = tree->topLevelItem(i);
            if (!item || !item->data(0, kIsConnectionRootRole).toBool()) {
                continue;
            }
            if (item->data(0, kConnIdxRole).toInt() == connIdx) {
                connRoot = item;
                break;
            }
        }
        if (!connRoot) {
            connRoot = new QTreeWidgetItem();
            connRoot->setData(0, kIsConnectionRootRole, true);
            connRoot->setData(0, kConnIdxRole, connIdx);
            connRoot->setFlags(connRoot->flags() & ~Qt::ItemIsUserCheckable);
            connRoot->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            tree->addTopLevelItem(connRoot);
        }
        QString connName = m_profiles[connIdx].name.trimmed().isEmpty()
                               ? m_profiles[connIdx].id.trimmed()
                               : m_profiles[connIdx].name.trimmed();
        if (connIdx < m_states.size() && m_states[connIdx].gsaNeedsAttention) {
            connName += QStringLiteral(" (*)");
        }
        connRoot->setText(0, connName);
        connRoot->setBackground(0, QBrush(connectionStateRowColor(connIdx)));
        connRoot->setToolTip(0, connectionStateTooltipHtml(connIdx));
        QFont f = connRoot->font(0);
        f.setItalic(isConnectionDisconnected(connIdx));
        connRoot->setFont(0, f);
        connRoot->setExpanded(true);
        if (isConnectionDisconnected(connIdx)) {
            return;
        }
    }
    const DatasetTreeRenderOptions options =
        datasetTreeRenderOptionsForTree(tree, DatasetTreeContext::ConnectionContentMulti);
    auto addPoolTree = [this, tree, &options](int cidx, const QString& poolName, bool allowRemoteLoadIfMissing) {
        appendDatasetTreeForPool(tree,
                                 cidx,
                                 poolName,
                                 DatasetTreeContext::ConnectionContentMulti,
                                 options,
                                 allowRemoteLoadIfMissing);
    };
    QSet<QString> seenPools;
    for (const PoolImported& pool : st.importedPools) {
        const QString poolName = pool.pool.trimmed();
        const QString poolKey = poolName.toLower();
        if (poolName.isEmpty() || seenPools.contains(poolKey)) {
            continue;
        }
        seenPools.insert(poolKey);
        addPoolTree(connIdx, poolName, true);
    }
    for (const PoolImportable& pool : st.importablePools) {
        const QString poolName = pool.pool.trimmed();
        const QString poolKey = poolName.toLower();
        if (poolName.isEmpty() || seenPools.contains(poolKey)) {
            continue;
        }
        const QString stateUp = pool.state.trimmed().toUpper();
        const QString actionTxt = pool.action.trimmed();
        if (stateUp != QStringLiteral("ONLINE") || actionTxt.isEmpty()) {
            continue;
        }
        seenPools.insert(poolKey);
        addPoolTree(connIdx, poolName, false);
    }
    for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (!item || !item->data(0, kIsPoolRootRole).toBool()) {
            continue;
        }
        const QString poolKey = item->data(0, kPoolNameRole).toString().trimmed().toLower();
        bool seenEarlier = false;
        for (int j = 0; j < i; ++j) {
            QTreeWidgetItem* prev = tree->topLevelItem(j);
            if (!prev || !prev->data(0, kIsPoolRootRole).toBool()) {
                continue;
            }
            if (prev->data(0, kPoolNameRole).toString().trimmed().toLower() == poolKey) {
                seenEarlier = true;
                break;
            }
        }
        if (seenEarlier) {
            delete tree->takeTopLevelItem(i);
        }
    }
    tree->expandToDepth(0);
    attachDatasetTreeSnapshotCombos(tree, DatasetTreeContext::ConnectionContent);
}

void restoreSnapshotSelectionInTree(QTreeWidget* tree, const ConnTreeNavSnapshot& nav) {
    if (!tree) {
        return;
    }
    const SnapshotKeyParts sk = parseSnapshotKey(nav.selectedKey);
    if (sk.snapshot.isEmpty()) {
        return;
    }
    if (QTreeWidgetItem* dsNode = findDatasetNode(tree, sk.pool, sk.dataset)) {
        if (QComboBox* cb = qobject_cast<QComboBox*>(tree->itemWidget(dsNode, 1))) {
            const int idx = cb->findText(sk.snapshot);
            if (idx > 0) {
                const QSignalBlocker b(cb);
                cb->setCurrentIndex(idx);
                dsNode->setData(1, Qt::UserRole, sk.snapshot);
                applySnapshotVisualStateConn(dsNode);
            }
        }
    }
}

void expandPoolRootsIfNoNav(QTreeWidget* tree, const ConnTreeNavSnapshot& nav) {
    if (!tree || !nav.expandedKeys.isEmpty() || !nav.selectedKey.trimmed().isEmpty()) {
        return;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (item && item->data(0, kIsPoolRootRole).toBool()) {
            item->setExpanded(true);
        }
    }
}

void MainWindow::refreshConnectionNodeDetails() {
    auto setConnectionActionButtonsVisible = [this](bool visible) {
        Q_UNUSED(visible);
    };
    auto setPoolActionButtonsVisible = [this](bool visible) {
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setVisible(visible);
        }
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setVisible(visible);
        }
        if (m_poolStatusExportBtn) {
            m_poolStatusExportBtn->setVisible(visible);
        }
        if (m_poolStatusScrubBtn) {
            m_poolStatusScrubBtn->setVisible(visible);
        }
        if (m_poolStatusDestroyBtn) {
            m_poolStatusDestroyBtn->setVisible(visible);
        }
    };

    auto resetPoolActionButtons = [this]() {
        if (m_poolStatusImportBtn) {
            m_poolStatusImportBtn->setEnabled(false);
        }
        if (m_poolStatusRefreshBtn) {
            m_poolStatusRefreshBtn->setProperty("zfsmgr_can_refresh", false);
            m_poolStatusRefreshBtn->setEnabled(false);
        }
        if (m_poolStatusExportBtn) {
            m_poolStatusExportBtn->setEnabled(false);
        }
        if (m_poolStatusScrubBtn) {
            m_poolStatusScrubBtn->setEnabled(false);
        }
        if (m_poolStatusDestroyBtn) {
            m_poolStatusDestroyBtn->setEnabled(false);
        }
    };

    int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        if (m_connContentTree) {
            const QString token = connContentTokenForTree(m_connContentTree);
            if (!token.isEmpty()) {
                saveConnContentTreeState(m_connContentTree, token);
            }
        }
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_poolPropsTable) {
            setTablePopulationMode(m_poolPropsTable, true);
            m_poolPropsTable->setRowCount(0);
            setTablePopulationMode(m_poolPropsTable, false);
        }
        if (m_poolStatusText) {
            m_poolStatusText->clear();
        }
        resetPoolActionButtons();
        if (m_connContentTree) {
            m_connContentTree->clear();
            syncConnContentPropertyColumnsFor(m_connContentTree, connContentTokenForTree(m_connContentTree));
        }
        if (m_connContentPropsTable) {
            setTablePopulationMode(m_connContentPropsTable, true);
            m_connContentPropsTable->setRowCount(0);
            setTablePopulationMode(m_connContentPropsTable, false);
        }
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    // Modo sin tabs de pools: el contenido se muestra directamente en el árbol
    // (múltiples raíces de pool). No vaciar ni repoblar aquí por "pool activo".
    if (m_connPropsStack && m_connContentPage) {
        m_connPropsStack->setCurrentWidget(m_connContentPage);
    }
    setConnectionActionButtonsVisible(false);
    setPoolActionButtonsVisible(false);
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
    return;
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    // No-op: la vista de detalle ya no usa subpestañas internas ocultas.
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    const int idx = currentConnectionIndexFromUi();
    if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
        return idx;
    }
    return -1;
}

void MainWindow::updatePoolManagementBoxTitle() {
    const int idx = selectedConnectionIndexForPoolManagement();
    const QString connText = (idx >= 0 && idx < m_profiles.size())
                                 ? m_profiles[idx].name
                                 : trk(QStringLiteral("t_empty_brkt_01"), QStringLiteral("[vacío]"), QStringLiteral("[empty]"), QStringLiteral("[空]"));
    if (m_poolMgmtBox) {
        m_poolMgmtBox->setTitle(
            trk(QStringLiteral("t_pool_mgmt_of01"),
                QStringLiteral("Gestión de Pools de %1"),
                QStringLiteral("Pool Management of %1"),
                QStringLiteral("%1 的池管理"))
                .arg(connText));
    }
}

void MainWindow::refreshConnectionByIndex(int idx) {
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    const QString topTreeToken = connContentTokenForTree(m_connContentTree);
    if (!topTreeToken.isEmpty() && m_connContentTree) {
        saveConnContentTreeState(m_connContentTree, topTreeToken);
    }
    // Al refrescar una conexión, invalidar toda la caché asociada a todos sus pools.
    {
        const QString connPrefix = QStringLiteral("%1::").arg(idx);
        auto dsIt = m_poolDatasetCache.begin();
        while (dsIt != m_poolDatasetCache.end()) {
            if (dsIt.key().startsWith(connPrefix)) {
                dsIt = m_poolDatasetCache.erase(dsIt);
            } else {
                ++dsIt;
            }
        }
        // Reutiliza la invalidación existente para detalle de pool + props de dataset.
        invalidatePoolDetailsCacheForConnection(idx);
        invalidatePoolAutoSnapshotInfoForConnection(idx);
        // Limpiar caché de propiedades inline del árbol de contenido para esta conexión.
        const QString uiPrefix = QStringLiteral("%1::").arg(QString::number(idx));
        auto valIt = m_connContentPropValuesByObject.begin();
        while (valIt != m_connContentPropValuesByObject.end()) {
            if (valIt.key().startsWith(uiPrefix)) {
                valIt = m_connContentPropValuesByObject.erase(valIt);
            } else {
                ++valIt;
            }
        }
    }
    m_states[idx] = refreshConnection(m_profiles[idx]);
    cachePoolStatusTextsForConnection(idx, m_states[idx]);
    rebuildConnInfoFor(idx);
    preloadPoolAutoSnapshotInfoForConnection(idx);
    rebuildConnectionsTable();
    populateAllPoolsTables();
}
void MainWindow::loadConnections() {
    QString prevSelectedConnId;
    {
        const int prevIdx = currentConnectionIndexFromUi();
        if (prevIdx >= 0 && prevIdx < m_profiles.size()) {
            prevSelectedConnId = m_profiles[prevIdx].id.trimmed();
        }
    }

    QMap<QString, ConnectionRuntimeState> prevById;
    QMap<QString, ConnectionRuntimeState> prevByName;
    const int oldCount = qMin(m_profiles.size(), m_states.size());
    for (int i = 0; i < oldCount; ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty()) {
            prevById[idKey] = m_states[i];
        }
        if (!nameKey.isEmpty()) {
            prevByName[nameKey] = m_states[i];
        }
    }

    const LoadResult loaded = m_store.loadConnections();
    m_profiles = loaded.profiles;
    {
        QSet<QString> validKeys;
        for (int i = 0; i < m_profiles.size(); ++i) {
            const QString key = connectionPersistKey(i);
            if (!key.isEmpty()) {
                validKeys.insert(key);
            }
        }
        for (auto it = m_disconnectedConnectionKeys.begin(); it != m_disconnectedConnectionKeys.end();) {
            if (!validKeys.contains(*it)) {
                it = m_disconnectedConnectionKeys.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_states.clear();
    m_states.resize(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) {
        const QString idKey = m_profiles[i].id.trimmed().toLower();
        const QString nameKey = m_profiles[i].name.trimmed().toLower();
        if (!idKey.isEmpty() && prevById.contains(idKey)) {
            m_states[i] = prevById.value(idKey);
            continue;
        }
        if (!nameKey.isEmpty() && prevByName.contains(nameKey)) {
            m_states[i] = prevByName.value(nameKey);
        }
    }
    rebuildConnInfoModel();

    rebuildConnectionsTable();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }

    int targetConnIdx = -1;
    if (!prevSelectedConnId.trimmed().isEmpty()) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].id.trimmed().compare(prevSelectedConnId, Qt::CaseInsensitive) == 0) {
                targetConnIdx = i;
                break;
            }
        }
    }
    if (targetConnIdx < 0 && m_initialRefreshCompleted) {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                targetConnIdx = i;
                break;
            }
        }
    }
    if (targetConnIdx >= 0) {
        setCurrentConnectionInUi(targetConnIdx);
    }

    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionsTable() {
    auto connIdxFromPersistedKey = [this](const QString& wantedKey) -> int {
        const QString wanted = wantedKey.trimmed().toLower();
        if (wanted.isEmpty()) {
            return -1;
        }
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (isConnectionDisconnected(i)) {
                continue;
            }
            const QString key = connPersistKeyFromProfiles(m_profiles, i);
            if (!key.isEmpty() && key == wanted) {
                return i;
            }
        }
        return -1;
    };
    auto firstConnectedIndex = [this]() -> int {
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (!isConnectionDisconnected(i)) {
                return i;
            }
        }
        return -1;
    };
    if (!m_connSelectorDefaultsInitialized) {
        if (m_topDetailConnIdx < 0) {
            m_topDetailConnIdx = connIdxFromPersistedKey(m_persistedTopDetailConnectionKey);
        }
        if (m_topDetailConnIdx < 0 || m_topDetailConnIdx >= m_profiles.size()
            || isConnectionDisconnected(m_topDetailConnIdx)) {
            m_topDetailConnIdx = firstConnectedIndex();
        }
        m_bottomDetailConnIdx = -1;
        m_connSelectorDefaultsInitialized = true;
    } else {
        if (m_topDetailConnIdx < 0 || m_topDetailConnIdx >= m_profiles.size()
            || isConnectionDisconnected(m_topDetailConnIdx)) {
            m_topDetailConnIdx = -1;
            m_topDetailConnIdx = firstConnectedIndex();
        }
    }

    rebuildConnectionEntityTabs();
    if (m_topDetailConnIdx >= 0) {
        setCurrentConnectionInUi(m_topDetailConnIdx);
    }
    syncConnectionLogTabs();
    refreshConnectionNodeDetails();
    updatePoolManagementBoxTitle();
}

void MainWindow::createConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: nueva conexión bloqueada"));
        return;
    }
    ConnectionDialog dlg(m_language, this);
    ConnectionProfile p;
    p.connType = QStringLiteral("SSH");
    p.osType = QStringLiteral("Linux");
    p.port = 22;
    dlg.setProfile(p);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile created = dlg.profile();
    {
        const QString newName = created.name.trimmed();
        for (const ConnectionProfile& cp : m_profiles) {
            if (cp.name.trimmed().compare(newName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
    QString createdId = created.id.trimmed();
    if (createdId.isEmpty()) {
        createdId = created.name.trimmed().toLower();
        createdId.replace(' ', '_');
        createdId.replace(':', '_');
        createdId.replace('/', '_');
    }
    QString err;
    if (!m_store.upsertConnection(created, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_conn_create_er1"),
                                  QStringLiteral("No se pudo crear conexión:\n%1"),
                                  QStringLiteral("Could not create connection:\n%1"),
                                  QStringLiteral("无法创建连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
    refreshInstalledGsaAfterConnectionChange(created.name.trimmed());
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == createdId) {
            setCurrentConnectionInUi(i);
            refreshSelectedConnection();
            break;
        }
    }
}

void MainWindow::editConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: edición bloqueada"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_01"),
                QStringLiteral("La conexión local integrada no se puede editar."),
                QStringLiteral("Built-in local connection cannot be edited."),
                QStringLiteral("内置本地连接不可编辑。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l2"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede editar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be edited."),
                QStringLiteral("该连接已重定向到“本地”，不可编辑。")));
        return;
    }
    ConnectionDialog dlg(m_language, this);
    dlg.setProfile(m_profiles[idx]);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    ConnectionProfile edited = dlg.profile();
    edited.id = m_profiles[idx].id;
    {
        const QString editedName = edited.name.trimmed();
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (i == idx) {
                continue;
            }
            if (m_profiles[i].name.trimmed().compare(editedName, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_conn_name_unique_01"),
                                         QStringLiteral("El nombre de conexión ya existe. Debe ser único."),
                                         QStringLiteral("Connection name already exists. It must be unique."),
                                         QStringLiteral("连接名称已存在，必须唯一。")));
                return;
            }
        }
    }
    QString err;
    if (!m_store.upsertConnection(edited, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_conn_update_er"),
                                  QStringLiteral("No se pudo actualizar conexión:\n%1"),
                                  QStringLiteral("Could not update connection:\n%1"),
                                  QStringLiteral("无法更新连接：\n%1")).arg(err));
        return;
    }
    loadConnections();
    refreshInstalledGsaAfterConnectionChange(edited.name.trimmed());
}

void MainWindow::installMsysForSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acci\xf3n en curso: instalaci\xf3n de MSYS2 bloqueada"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    if (!isWindowsConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_windows_only_01"),
                QStringLiteral("Esta acci\xf3n solo est\xe1 disponible para conexiones Windows."),
                QStringLiteral("This action is only available for Windows connections."),
                QStringLiteral("此操作仅适用于 Windows 连接。")));
        return;
    }
    if (isConnectionDisconnected(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_conn_disc_01"),
                QStringLiteral("La conexi\xf3n est\xe1 desconectada."),
                QStringLiteral("The connection is disconnected."),
                QStringLiteral("该连接已断开。")));
        return;
    }

    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_msys_install_title_01"),
            QStringLiteral("Instalar MSYS2"),
            QStringLiteral("Install MSYS2"),
            QStringLiteral("安装 MSYS2")),
        trk(QStringLiteral("t_msys_install_q_01"),
            QStringLiteral("Se comprobará MSYS2 en \"%1\" y, si falta, se intentará instalar mediante winget junto con paquetes base (tar, gzip, zstd, rsync, grep, sed, gawk).\n\n¿Continuar?"),
            QStringLiteral("ZFSMgr will check MSYS2 on \"%1\" and, if missing, try to install it with winget plus base packages (tar, gzip, zstd, rsync, grep, sed, gawk).\n\nContinue?"),
            QStringLiteral("ZFSMgr 将检查 \"%1\" 上的 MSYS2，如缺失则尝试使用 winget 安装，并补齐基础包（tar、gzip、zstd、rsync、grep、sed、gawk）。\n\n是否继续？"))
            .arg(p.name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }

    const WindowsCommandMode psMode = WindowsCommandMode::PowerShellNative;
    const QString detectCmd = QStringLiteral(
        "$roots=@('C:\\\\msys64\\\\usr\\\\bin','C:\\\\msys64\\\\mingw64\\\\bin','C:\\\\msys64\\\\mingw32\\\\bin','C:\\\\MinGW\\\\bin','C:\\\\mingw64\\\\bin'); "
        "$bashCandidates=@('C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe','C:\\\\msys64\\\\usr\\\\bin\\\\sh.exe','C:\\\\msys64\\\\mingw64\\\\bin\\\\bash.exe','C:\\\\msys64\\\\mingw32\\\\bin\\\\bash.exe','C:\\\\MinGW\\\\msys\\\\1.0\\\\bin\\\\sh.exe'); "
        "$bash=$bashCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1; "
        "if($bash){ Write-Output ('BASH:' + $bash) } else { Write-Output 'BASH:' }; "
        "$cmds='tar gzip zstd rsync grep sed gawk'.Split(' '); "
        "foreach($c in $cmds){ "
        "  $ok=$false; foreach($r in $roots){ if(Test-Path -LiteralPath (Join-Path $r ($c + '.exe'))){ $ok=$true; break } }; "
        "  if($ok){ Write-Output ('OK:' + $c) } else { Write-Output ('KO:' + $c) } "
        "}");

    auto detectState = [&](QString& bashPath, QStringList& missing, QString* detailOut = nullptr) -> bool {
        bashPath.clear();
        missing.clear();
        QString out;
        QString detail;
        if (!fetchConnectionCommandOutput(idx, QStringLiteral("Detectar MSYS2"), detectCmd, &out, &detail, 30000, psMode)) {
            if (detailOut) {
                *detailOut = detail.simplified().left(220);
            }
            return false;
        }
        const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QStringLiteral("BASH:"))) {
                bashPath = line.mid(5).trimmed();
            } else if (line.startsWith(QStringLiteral("KO:"))) {
                missing << line.mid(3).trimmed();
            }
        }
        if (detailOut) {
            *detailOut = out;
        }
        return true;
    };

    QString bashPath;
    QStringList missingPackages;
    QString detectDetail;
    if (!detectState(bashPath, missingPackages, &detectDetail)) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_detect_fail_01"),
                QStringLiteral("No se pudo comprobar MSYS2 en \"%1\".\n\n%2"),
                QStringLiteral("Could not check MSYS2 on \"%1\".\n\n%2"),
                QStringLiteral("无法检查 \"%1\" 上的 MSYS2。\n\n%2"))
                .arg(p.name, detectDetail));
        return;
    }

    if (!bashPath.isEmpty() && missingPackages.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_already_ok_01"),
                QStringLiteral("MSYS2 ya est\xe1 disponible en \"%1\"."),
                QStringLiteral("MSYS2 is already available on \"%1\"."),
                QStringLiteral("\"%1\" 上已提供 MSYS2。"))
                .arg(p.name));
        refreshConnectionByIndex(idx);
        return;
    }

    QString installCmd;
    if (bashPath.isEmpty()) {
        installCmd = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "$msysRoot='C:\\\\msys64'; "
            "$bashCandidates=@('C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe','C:\\\\msys64\\\\usr\\\\bin\\\\sh.exe'); "
            "$bash=$bashCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1; "
            "if(-not $bash){ "
            "  $winget = Get-Command winget.exe -ErrorAction SilentlyContinue; "
            "  $installed=$false; "
            "  if($winget){ "
            "    & $winget.Source install --exact --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements --disable-interactivity --silent; "
            "    if($LASTEXITCODE -eq 0){ $installed=$true } "
            "  }; "
            "  if(-not $installed){ "
            "    $tmp = Join-Path $env:TEMP 'zfsmgr-msys2-base-x86_64-latest.sfx.exe'; "
            "    $url = 'https://github.com/msys2/msys2-installer/releases/latest/download/msys2-base-x86_64-latest.sfx.exe'; "
            "    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tmp; "
            "    if(-not (Test-Path -LiteralPath $tmp)){ throw 'No se pudo descargar el instalador base de MSYS2' }; "
            "    & $tmp '-y' '-oC:\\'; "
            "    if($LASTEXITCODE -ne 0){ throw ('instalador base MSYS2 fall\xf3 con exit ' + $LASTEXITCODE) } "
            "  }; "
            "}; "
            "$bashCandidates=@('C:\\\\msys64\\\\usr\\\\bin\\\\bash.exe','C:\\\\msys64\\\\usr\\\\bin\\\\sh.exe'); "
            "$bash=$bashCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1; "
            "if(-not $bash){ throw 'MSYS2 instalado pero bash.exe no encontrado en C:\\\\msys64' }; "
            "& $bash -lc 'true'; "
            "& $bash -lc 'pacman --noconfirm -Sy --needed tar gzip zstd rsync grep sed gawk'; "
            "if($LASTEXITCODE -ne 0){ throw ('pacman fall\xf3 con exit ' + $LASTEXITCODE) }");
    } else {
        QString escapedBash = bashPath;
        escapedBash.replace('\'', QStringLiteral("''"));
        installCmd = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "$bash='%1'; "
            "& $bash -lc 'pacman --noconfirm -Sy --needed tar gzip zstd rsync grep sed gawk'; "
            "if($LASTEXITCODE -ne 0){ throw ('pacman fall\xf3 con exit ' + $LASTEXITCODE) }")
                         .arg(escapedBash);
    }

    beginUiBusy();
    updateStatus(trk(QStringLiteral("t_msys_install_progress_01"),
                     QStringLiteral("Instalando/verificando MSYS2 en %1..."),
                     QStringLiteral("Installing/checking MSYS2 on %1..."),
                     QStringLiteral("正在 %1 上安装/检查 MSYS2...")).arg(p.name));
    QString installDetail;
    const bool ok = executeConnectionCommand(idx, QStringLiteral("Instalar MSYS2"), installCmd, 900000, &installDetail, psMode);
    endUiBusy();

    if (!ok) {
        const QString detail = installDetail.simplified().left(400);
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_install_fail_01"),
                QStringLiteral("No se pudo instalar/preparar MSYS2 en \"%1\".\n\n%2"),
                QStringLiteral("Could not install/prepare MSYS2 on \"%1\".\n\n%2"),
                QStringLiteral("无法在 \"%1\" 上安装/准备 MSYS2。\n\n%2"))
                .arg(p.name, detail));
        refreshConnectionByIndex(idx);
        return;
    }

    bashPath.clear();
    missingPackages.clear();
    detectDetail.clear();
    detectState(bashPath, missingPackages, &detectDetail);
    refreshConnectionByIndex(idx);

    if (!bashPath.isEmpty() && missingPackages.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_msys_install_ok_01"),
                QStringLiteral("MSYS2 preparado correctamente en \"%1\"."),
                QStringLiteral("MSYS2 was prepared successfully on \"%1\"."),
                QStringLiteral("已在 \"%1\" 上成功准备 MSYS2。"))
                .arg(p.name));
        return;
    }

    QMessageBox::warning(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_msys_install_partial_01"),
            QStringLiteral("La instalaci\xf3n termin\xf3, pero faltan comandos en \"%1\": %2"),
            QStringLiteral("The installation finished, but commands are still missing on \"%1\": %2"),
            QStringLiteral("安装已完成，但 \"%1\" 上仍缺少命令：%2"))
            .arg(p.name, missingPackages.join(QStringLiteral(", "))));
}

void MainWindow::installHelperCommandsForSelectedConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: instalación de comandos auxiliares bloqueada"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    const ConnectionProfile& p = m_profiles[idx];
    if (isConnectionDisconnected(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_conn_disc_01"),
                QStringLiteral("La conexión está desconectada."),
                QStringLiteral("The connection is disconnected."),
                QStringLiteral("该连接已断开。")));
        return;
    }
    if (idx >= m_states.size()) {
        return;
    }
    const ConnectionRuntimeState& st = m_states[idx];
    if (isWindowsConnection(idx)) {
        installMsysForSelectedConnection();
        return;
    }
    if (st.missingUnixCommands.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_none_missing_01"),
                QStringLiteral("No faltan comandos auxiliares en esta conexión."),
                QStringLiteral("No helper commands are missing on this connection."),
                QStringLiteral("该连接不缺少辅助命令。")));
        return;
    }
    if (!st.helperInstallSupported || st.helperInstallCommandPreview.trimmed().isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_not_supported_01"),
                QStringLiteral("No hay instalación asistida disponible para esta conexión.\n\n%1"),
                QStringLiteral("Assisted installation is not available for this connection.\n\n%1"),
                QStringLiteral("此连接不可用辅助安装。\n\n%1"))
                .arg(st.helperInstallReason.trimmed().isEmpty() ? QStringLiteral("-") : st.helperInstallReason.trimmed()));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(
        trk(QStringLiteral("t_helper_install_title_01"),
            QStringLiteral("Instalar comandos auxiliares"),
            QStringLiteral("Install helper commands"),
            QStringLiteral("安装辅助命令")));
    dlg.resize(760, 520);

    auto* layout = new QVBoxLayout(&dlg);
    auto* summary = new QLabel(
        trk(QStringLiteral("t_helper_install_summary_01"),
            QStringLiteral("Se va a preparar la conexión \"%1\" para instalar los comandos auxiliares que faltan."),
            QStringLiteral("Connection \"%1\" will be prepared to install the missing helper commands."),
            QStringLiteral("将为连接 \"%1\" 准备缺失的辅助命令安装。")).arg(p.name),
        &dlg);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    QStringList metaLines;
    metaLines << QStringLiteral("Plataforma: %1").arg(st.helperPlatformLabel.trimmed().isEmpty()
                                                          ? QStringLiteral("-")
                                                          : st.helperPlatformLabel.trimmed());
    metaLines << QStringLiteral("Gestor de paquetes: %1").arg(st.helperPackageManagerLabel.trimmed().isEmpty()
                                                                  ? QStringLiteral("-")
                                                                  : st.helperPackageManagerLabel.trimmed());
    metaLines << QStringLiteral("Comandos faltantes: %1").arg(st.missingUnixCommands.join(QStringLiteral(", ")));
    metaLines << QStringLiteral("Comandos instalables: %1").arg(st.helperInstallableCommands.join(QStringLiteral(", ")));
    metaLines << QStringLiteral("Paquetes a instalar: %1").arg(st.helperInstallPackages.join(QStringLiteral(", ")));
    if (!st.helperUnsupportedCommands.isEmpty()) {
        metaLines << QStringLiteral("Comandos no soportados en esta fase: %1")
                         .arg(st.helperUnsupportedCommands.join(QStringLiteral(", ")));
    }
    if (!st.helperInstallReason.trimmed().isEmpty()) {
        metaLines << QStringLiteral("Notas: %1").arg(st.helperInstallReason.trimmed());
    }

    auto* metaBox = new QPlainTextEdit(metaLines.join(QStringLiteral("\n")), &dlg);
    metaBox->setReadOnly(true);
    metaBox->setMaximumBlockCount(200);
    layout->addWidget(metaBox);

    auto* previewLabel = new QLabel(
        trk(QStringLiteral("t_helper_cmd_preview_01"),
            QStringLiteral("Comando remoto previsto"),
            QStringLiteral("Planned remote command"),
            QStringLiteral("计划执行的远程命令")),
        &dlg);
    layout->addWidget(previewLabel);

    auto* preview = new QPlainTextEdit(st.helperInstallCommandPreview, &dlg);
    preview->setReadOnly(true);
    layout->addWidget(preview, 1);

    auto* refreshAfter = new QCheckBox(
        trk(QStringLiteral("t_helper_refresh_after_01"),
            QStringLiteral("Refrescar conexión al terminar"),
            QStringLiteral("Refresh connection when finished"),
            QStringLiteral("完成后刷新连接")),
        &dlg);
    refreshAfter->setChecked(true);
    layout->addWidget(refreshAfter);

    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* cancelBtn = buttons->addButton(
        trk(QStringLiteral("t_helper_cancel_01"),
            QStringLiteral("Cancelar"),
            QStringLiteral("Cancel"),
            QStringLiteral("取消")),
        QDialogButtonBox::RejectRole);
    QPushButton* installBtn = buttons->addButton(
        trk(QStringLiteral("t_helper_install_btn_01"),
            QStringLiteral("Instalar"),
            QStringLiteral("Install"),
            QStringLiteral("安装")),
        QDialogButtonBox::AcceptRole);
    Q_UNUSED(cancelBtn);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    const QString installCmd = withSudo(p, stripLeadingSudoForExecution(st.helperInstallCommandPreview));
    beginUiBusy();
    updateStatus(
        trk(QStringLiteral("t_helper_install_busy_01"),
            QStringLiteral("Instalando comandos auxiliares en %1..."),
            QStringLiteral("Installing helper commands on %1..."),
            QStringLiteral("正在 %1 上安装辅助命令...")).arg(p.name));
    QString detail;
    const bool ok = executeConnectionCommand(idx, QStringLiteral("Instalar auxiliares"), installCmd, 1800000, &detail);
    endUiBusy();

    if (!ok) {
        detail = detail.left(1200);
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_helper_install_fail_01"),
                QStringLiteral("No se pudieron instalar los comandos auxiliares en \"%1\".\n\n%2"),
                QStringLiteral("Could not install helper commands on \"%1\".\n\n%2"),
                QStringLiteral("无法在 \"%1\" 上安装辅助命令。\n\n%2"))
                .arg(p.name, detail));
        if (refreshAfter->isChecked()) {
            refreshConnectionByIndex(idx);
        }
        return;
    }

    if (refreshAfter->isChecked()) {
        refreshConnectionByIndex(idx);
    }

    QMessageBox::information(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_helper_install_ok_01"),
            QStringLiteral("La instalación terminó correctamente en \"%1\"."),
            QStringLiteral("The installation finished successfully on \"%1\"."),
            QStringLiteral("\"%1\" 上的安装已成功完成。"))
            .arg(p.name));
}

QString MainWindow::gsaMenuLabelForConnection(int connIdx) const {
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()) {
        return trk(QStringLiteral("t_gsa_install_001"),
                   QStringLiteral("Instalar gestor de snapshots"),
                   QStringLiteral("Install snapshot manager"),
                   QStringLiteral("安装快照管理器"));
    }
    const ConnectionRuntimeState& st = m_states[connIdx];
    if (!st.gsaInstalled) {
        return trk(QStringLiteral("t_gsa_install_001"),
                   QStringLiteral("Instalar gestor de snapshots"),
                   QStringLiteral("Install snapshot manager"),
                   QStringLiteral("安装快照管理器"));
    }
    if (st.gsaNeedsAttention || st.gsaVersion.trimmed() != gsaScriptVersion().trimmed()) {
        return trk(QStringLiteral("t_gsa_update_001"),
                   QStringLiteral("Actualizar versión del Gestor de snapshots"),
                   QStringLiteral("Update snapshot manager version"),
                   QStringLiteral("更新快照管理器版本"));
    }
    if (!st.gsaActive) {
        return trk(QStringLiteral("t_gsa_enable_001"),
                   QStringLiteral("Activar GSA"),
                   QStringLiteral("Enable GSA"),
                   QStringLiteral("启用 GSA"));
    }
    return trk(QStringLiteral("t_gsa_ok_001"),
               QStringLiteral("GSA actualizado y funcionando"),
               QStringLiteral("GSA updated and running"),
               QStringLiteral("GSA 已更新并运行中"));
}

bool MainWindow::installOrUpdateGsaForConnection(int idx) {
    return installOrUpdateGsaForConnectionInternal(idx, true);
}

bool MainWindow::installOrUpdateGsaForConnectionInternal(int idx, bool interactive) {
    if (actionsLocked()) {
        return false;
    }
    if (idx < 0 || idx >= m_profiles.size()) {
        return false;
    }
    if (isConnectionDisconnected(idx)) {
        if (interactive) {
            QMessageBox::information(this,
                                     QStringLiteral("ZFSMgr"),
                                     trk(QStringLiteral("t_gsa_conn_disc_001"),
                                         QStringLiteral("La conexión está desconectada."),
                                         QStringLiteral("The connection is disconnected."),
                                         QStringLiteral("该连接已断开。")));
        }
        return false;
    }
    const ConnectionProfile& p = m_profiles[idx];
    const QString selfConnName = p.name.trimmed().isEmpty() ? p.id.trimmed() : p.name.trimmed();
    const QString actionLabel = gsaMenuLabelForConnection(idx);

    auto profileDisplayName = [this](int connIdx) {
        const ConnectionProfile& cp = m_profiles[connIdx];
        return cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
    };
    auto unixConfigDirForProfile = [this](const ConnectionProfile& cp, bool isMac, bool isFreeBsd) {
        if (!isMac) {
            if (isFreeBsd) {
                return QString::fromLatin1(kGsaFreeBsdRuntimeDirPath);
            }
            return QString::fromLatin1(kGsaLinuxRuntimeDirPath);
        }
        if (isLocalConnection(cp)) {
            return m_store.configDir();
        }
        const QString user = cp.username.trimmed().isEmpty() ? QStringLiteral("root") : cp.username.trimmed();
        if (isMac) {
            return (user == QStringLiteral("root"))
                       ? QStringLiteral("/var/root/.config/ZFSMgr")
                       : QStringLiteral("/Users/%1/.config/ZFSMgr").arg(user);
        }
        return (user == QStringLiteral("root"))
                   ? QStringLiteral("/root/.config/ZFSMgr")
                   : QStringLiteral("/home/%1/.config/ZFSMgr").arg(user);
    };
    auto windowsConfigDirForProfile = [this](const ConnectionProfile& cp) {
        Q_UNUSED(this);
        QString user = cp.username.trimmed();
        const int slash = qMax(user.lastIndexOf(QLatin1Char('\\')), user.lastIndexOf(QLatin1Char('/')));
        if (slash >= 0) {
            user = user.mid(slash + 1);
        }
        if (user.isEmpty()) {
            user = QStringLiteral("Default");
        }
        return QStringLiteral("C:\\Users\\%1\\.config\\ZFSMgr").arg(user);
    };
    auto buildUnixConnectionsPayload = [this, idx, &profileDisplayName](const QSet<QString>& requiredConnNames) {
        QString payload;
        payload += QStringLiteral("resolve_target_connection() {\n");
        payload += QStringLiteral("  TARGET_MODE=''\n");
        payload += QStringLiteral("  TARGET_HOST=''\n");
        payload += QStringLiteral("  TARGET_PORT=''\n");
        payload += QStringLiteral("  TARGET_USER=''\n");
        payload += QStringLiteral("  TARGET_PASS=''\n");
        payload += QStringLiteral("  TARGET_KEY=''\n");
        payload += QStringLiteral("  TARGET_USE_SUDO='0'\n");
        payload += QStringLiteral("  case \"$1\" in\n");
        for (int i = 0; i < m_profiles.size(); ++i) {
            const QString connName = profileDisplayName(i);
            if (connName.isEmpty() || !requiredConnNames.contains(connName)) {
                continue;
            }
            int effectiveIdx = i;
            bool localMode = (i == idx) || connectionsReferToSameMachine(idx, i);
            if (isLocalConnection(i)) {
                const int sshIdx = equivalentSshForLocal(i);
                if (!localMode && sshIdx < 0) {
                    continue;
                }
                if (!localMode && sshIdx >= 0) {
                    effectiveIdx = sshIdx;
                    localMode = connectionsReferToSameMachine(idx, sshIdx);
                }
            }
            const ConnectionProfile& effective = m_profiles[effectiveIdx];
            if (!localMode && effective.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            payload += QStringLiteral("    %1)\n").arg(mwhelpers::shSingleQuote(connName));
            if (localMode) {
                payload += QStringLiteral("      TARGET_MODE='local'\n");
            } else {
                payload += QStringLiteral("      TARGET_MODE='ssh'\n");
                payload += QStringLiteral("      TARGET_HOST=%1\n").arg(mwhelpers::shSingleQuote(effective.host.trimmed()));
                payload += QStringLiteral("      TARGET_PORT=%1\n").arg(mwhelpers::shSingleQuote(effective.port > 0 ? QString::number(effective.port) : QString()));
                payload += QStringLiteral("      TARGET_USER=%1\n").arg(mwhelpers::shSingleQuote(effective.username.trimmed()));
                payload += QStringLiteral("      TARGET_PASS=%1\n").arg(mwhelpers::shSingleQuote(effective.password));
                payload += QStringLiteral("      TARGET_KEY=%1\n").arg(mwhelpers::shSingleQuote(effective.keyPath.trimmed()));
                payload += QStringLiteral("      TARGET_USE_SUDO=%1\n")
                               .arg(mwhelpers::shSingleQuote(effective.useSudo ? QStringLiteral("1") : QStringLiteral("0")));
            }
            payload += QStringLiteral("      return 0\n");
            payload += QStringLiteral("      ;;\n");
        }
        payload += QStringLiteral("    *) return 1 ;;\n");
        payload += QStringLiteral("  esac\n");
        payload += QStringLiteral("}\n");
        return payload;
    };
    QStringList routeWarnings;
    QSet<QString> requiredDestinationConnNames;
    QSet<QString> candidatePools;
    for (auto it = m_poolDatasetCache.cbegin(); it != m_poolDatasetCache.cend(); ++it) {
        const QStringList parts = it.key().split(QStringLiteral("::"));
        if (parts.size() != 2) {
            continue;
        }
        bool okConn = false;
        const int poolConnIdx = parts.at(0).toInt(&okConn);
        const QString poolName = parts.at(1).trimmed();
        if (okConn && poolConnIdx == idx && !poolName.isEmpty()) {
            candidatePools.insert(poolName);
        }
    }
    if (const ConnInfo* connInfo = findConnInfo(idx)) {
        for (auto itPool = connInfo->poolsByStableId.cbegin(); itPool != connInfo->poolsByStableId.cend(); ++itPool) {
            if (!itPool->key.poolName.trimmed().isEmpty()) {
                candidatePools.insert(itPool->key.poolName.trimmed());
            }
        }
    }
    auto boolOnString = [](const QString& raw) {
        const QString v = raw.trimmed().toLower();
        return v == QStringLiteral("on")
               || v == QStringLiteral("yes")
               || v == QStringLiteral("true")
               || v == QStringLiteral("1");
    };
    for (const QString& poolName : candidatePools) {
        QString out;
        QString detail;
        const bool isWin = isWindowsConnection(p);
        const QString cmd = withSudo(
            p,
            isWin
                ? QStringLiteral("zfs get -H -o name,property,value -r %1 %2")
                      .arg(QStringLiteral("'org.fc16.gsa:activado','org.fc16.gsa:nivelar','org.fc16.gsa:destino'"),
                           mwhelpers::shSingleQuote(poolName))
                : mwhelpers::withUnixSearchPathCommand(
                      QStringLiteral("zfs get -j -r %1 %2")
                          .arg(QStringLiteral("org.fc16.gsa:activado,org.fc16.gsa:nivelar,org.fc16.gsa:destino"),
                               mwhelpers::shSingleQuote(poolName))));
        if (!fetchConnectionCommandOutput(idx,
                                          QStringLiteral("Leer GSA datasets"),
                                          cmd,
                                          &out,
                                          &detail,
                                          20000)) {
            continue;
        }
        QMap<QString, QMap<QString, QString>> propsByDataset;
        if (!isWin) {
            const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
            const QJsonObject datasets = doc.object().value(QStringLiteral("datasets")).toObject();
            for (auto dsIt = datasets.constBegin(); dsIt != datasets.constEnd(); ++dsIt) {
                const QString datasetName = dsIt.key().trimmed();
                if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@'))) {
                    continue;
                }
                const QJsonObject props = dsIt.value().toObject()
                                          .value(QStringLiteral("properties")).toObject();
                for (auto pIt = props.constBegin(); pIt != props.constEnd(); ++pIt) {
                    const QString propName = pIt.key().trimmed();
                    const QString propValue = pIt.value().toObject()
                                              .value(QStringLiteral("value")).toString().trimmed();
                    if (propName.isEmpty()) {
                        continue;
                    }
                    propsByDataset[datasetName].insert(propName, propValue);
                }
            }
        } else {
            const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString& line : lines) {
                const QStringList parts = line.split('\t');
                if (parts.size() < 3) {
                    continue;
                }
                const QString datasetName = parts.at(0).trimmed();
                const QString propName = parts.at(1).trimmed();
                const QString propValue = parts.at(2).trimmed();
                if (datasetName.isEmpty() || datasetName.contains(QLatin1Char('@')) || propName.isEmpty()) {
                    continue;
                }
                propsByDataset[datasetName].insert(propName, propValue);
            }
        }
        for (auto dsIt = propsByDataset.cbegin(); dsIt != propsByDataset.cend(); ++dsIt) {
            const QMap<QString, QString>& props = dsIt.value();
            if (!boolOnString(props.value(QStringLiteral("org.fc16.gsa:activado")))) {
                continue;
            }
            if (!boolOnString(props.value(QStringLiteral("org.fc16.gsa:nivelar")))) {
                continue;
            }
            const QString dest = props.value(QStringLiteral("org.fc16.gsa:destino")).trimmed();
            const QString destConnName = dest.section(QStringLiteral("::"), 0, 0).trimmed();
            if (!destConnName.isEmpty()) {
                requiredDestinationConnNames.insert(destConnName);
            }
        }
    }
    if (const ConnInfo* connInfo = findConnInfo(idx)) {
        for (auto itPool = connInfo->poolsByStableId.cbegin(); itPool != connInfo->poolsByStableId.cend(); ++itPool) {
            const QString poolName = itPool->key.poolName.trimmed();
            if (poolName.isEmpty()) {
                continue;
            }
            for (auto itDs = itPool->objectsByFullName.cbegin(); itDs != itPool->objectsByFullName.cend(); ++itDs) {
                const QString datasetName = itDs.key().trimmed();
                if (datasetName.isEmpty()
                    || itDs->runtime.datasetType.trimmed().compare(QStringLiteral("filesystem"), Qt::CaseInsensitive) != 0) {
                    continue;
                }
                QMap<QString, QString> props;
                for (const DatasetPropCacheRow& row : itDs->runtime.propertyRows) {
                    props.insert(row.prop, row.value);
                }
                const QString token = QStringLiteral("%1::%2").arg(idx).arg(poolName);
                const QString liveKey = QStringLiteral("%1|%2").arg(token, datasetName);
                const auto liveIt = m_connContentPropValuesByObject.constFind(liveKey);
                if (liveIt != m_connContentPropValuesByObject.cend()) {
                    for (auto vit = liveIt->cbegin(); vit != liveIt->cend(); ++vit) {
                        props[vit.key()] = vit.value();
                    }
                }
                const QString enabled = props.value(QStringLiteral("org.fc16.gsa:activado")).trimmed().toLower();
                if (!boolOnString(enabled)) {
                    continue;
                }
                const QString levelOn = props.value(QStringLiteral("org.fc16.gsa:nivelar")).trimmed().toLower();
                if (!boolOnString(levelOn)) {
                    continue;
                }
                const QString dest = props.value(QStringLiteral("org.fc16.gsa:destino")).trimmed();
                if (dest.isEmpty()) {
                    continue;
                }
                const QString destConnName = dest.section(QStringLiteral("::"), 0, 0).trimmed();
                if (!destConnName.isEmpty()) {
                    requiredDestinationConnNames.insert(destConnName);
                }
                const int destIdx = connectionIndexByNameOrId(destConnName);
                if (destIdx < 0) {
                    routeWarnings << trk(QStringLiteral("t_gsa_route_missing_conn_001"),
                                         QStringLiteral("%1 -> %2: la conexión destino no existe."),
                                         QStringLiteral("%1 -> %2: the destination connection does not exist."),
                                         QStringLiteral("%1 -> %2：目标连接不存在。")).arg(datasetName, destConnName);
                    continue;
                }
                QString routeErr;
                int effectiveDstIdx = -1;
                if (!canSshBetweenConnections(idx, destIdx, &routeErr, &effectiveDstIdx)) {
                    routeWarnings << trk(QStringLiteral("t_gsa_route_warn_001"),
                                         QStringLiteral("%1 -> %2: la interconexión SSH no está OK en la matriz de conectividad (%3)."),
                                         QStringLiteral("%1 -> %2: SSH interconnection is not OK in the connectivity matrix (%3)."),
                                         QStringLiteral("%1 -> %2：连通性矩阵中的 SSH 互连不是 OK（%3）。")).arg(datasetName, destConnName, routeErr);
                    continue;
                }
                if (isWindowsConnection(idx) && effectiveDstIdx >= 0 && effectiveDstIdx < m_profiles.size()
                    && !m_profiles[effectiveDstIdx].password.trimmed().isEmpty()
                    && !connectionsReferToSameMachine(idx, effectiveDstIdx)) {
                    routeWarnings << trk(QStringLiteral("t_gsa_route_windows_auth_warn_001"),
                                         QStringLiteral("%1 -> %2: GSA en Windows requiere autenticación SSH no interactiva en el origen remoto. Este destino usa password y puede no ejecutarse sin clave SSH."),
                                         QStringLiteral("%1 -> %2: GSA on Windows requires non-interactive SSH authentication on the remote source. This target uses a password and may not run without an SSH key."),
                                         QStringLiteral("%1 -> %2：Windows 上的 GSA 需要远端源主机上的非交互式 SSH 认证。该目标使用密码，没有 SSH 密钥时可能无法执行。")).arg(datasetName, destConnName);
                }
            }
        }
    }
    if (interactive && !routeWarnings.isEmpty()) {
        const auto warnAnswer = QMessageBox::warning(
            this,
            actionLabel,
            trk(QStringLiteral("t_gsa_route_warn_title_001"),
                QStringLiteral("Hay nivelaciones GSA configuradas que no se podrán ejecutar directamente:\n\n%1\n\nSi continúas, GSA quedará instalado/actualizado, pero esas interconexiones seguirán fallando mientras no aparezcan como SSH OK en la matriz de conectividad.\n\n¿Continuar?"),
                QStringLiteral("There are configured GSA leveling routes that will not run directly:\n\n%1\n\nIf you continue, GSA will be installed/updated, but those interconnections will still fail until they show up as SSH OK in the connectivity matrix.\n\nContinue?"),
                QStringLiteral("存在已配置的 GSA 同步路径无法直接执行：\n\n%1\n\n如果继续，GSA 会安装/更新，但这些互连仍会失败，直到它们在连通性矩阵中显示为 SSH OK。\n\n是否继续？"))
                .arg(routeWarnings.join(QStringLiteral("\n"))),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (warnAnswer != QMessageBox::Yes) {
            return false;
        }
    }

    if (interactive) {
        const auto confirm = QMessageBox::question(
            this,
            actionLabel,
            trk(QStringLiteral("t_gsa_confirm_001"),
                QStringLiteral("ZFSMgr instalará o actualizará el GSA y lo programará cada hora usando el scheduler nativo de \"%1\".\n\n¿Continuar?"),
                QStringLiteral("ZFSMgr will install or update GSA and schedule it hourly using the native scheduler on \"%1\".\n\nContinue?"),
                QStringLiteral("ZFSMgr 将安装或更新 GSA，并使用 \"%1\" 上的原生计划任务每小时执行一次。\n\n是否继续？"))
                .arg(p.name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (confirm != QMessageBox::Yes) {
            return false;
        }
    }

    QString remoteCmd;
    WindowsCommandMode winMode = WindowsCommandMode::Auto;
    if (isWindowsConnection(idx)) {
        winMode = WindowsCommandMode::PowerShellNative;
        QString payload = gsaWindowsScriptPayload(selfConnName);
        payload.replace(QStringLiteral("__CONFIG_DIR__"), windowsConfigDirForProfile(p));
        const QString taskName = QString::fromLatin1(kGsaTaskName);
        remoteCmd = QStringLiteral(
            "$dir='C:\\ProgramData\\ZFSMgr'; "
            "New-Item -ItemType Directory -Force -Path $dir | Out-Null; "
            "$script=Join-Path $dir 'gsa.ps1'; "
            "@'\n%1\n'@ | Set-Content -LiteralPath $script -Encoding UTF8; "
            "$taskCmd='powershell -NoProfile -ExecutionPolicy Bypass -File \"' + $script + '\"'; "
            "schtasks /Create /F /TN '%2' /SC HOURLY /MO 1 /RU SYSTEM /TR $taskCmd | Out-Null; "
            "schtasks /Change /TN '%2' /ENABLE | Out-Null")
                        .arg(payload, taskName);
    } else {
        const QString osHint = (p.osType + QStringLiteral(" ")
                                + ((idx < m_states.size()) ? m_states[idx].osLine : QString())
                                + QStringLiteral(" ")
                                + ((idx < m_states.size()) ? m_states[idx].gsaScheduler : QString()))
                                   .trimmed()
                                   .toLower();
        const bool isMac = osHint.contains(QStringLiteral("darwin"))
                           || osHint.contains(QStringLiteral("macos"))
                           || osHint.contains(QStringLiteral("launchd"));
        const bool isFreeBsd = osHint.contains(QStringLiteral("freebsd"))
                               || osHint.contains(QStringLiteral("cron"));
        const QString runtimeConfigDir = unixConfigDirForProfile(p, isMac, isFreeBsd);
        const QString scriptPayload = gsaUnixScriptPayload();
        const QString mainConfigPayload = gsaUnixMainConfigPayload(selfConnName, runtimeConfigDir);
        const QString connectionsPayload = buildUnixConnectionsPayload(requiredDestinationConnNames);
        const QString knownHostsPayload = gsaKnownHostsPayload(m_profiles, idx, requiredDestinationConnNames);
        if (isMac) {
            const QString plistPayload = QString::fromUtf8(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>org.zfsmgr.gsa</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/libexec/zfsmgr-gsa.sh</string>
  </array>
  <key>StartCalendarInterval</key>
  <array>
    <dict><key>Minute</key><integer>0</integer></dict>
  </array>
  <key>RunAtLoad</key><true/>
</dict>
</plist>
)");
            remoteCmd = QStringLiteral(
                "mkdir -p /usr/local/libexec %5; "
                "cat > %1 <<'EOF_GSA'\n%2\nEOF_GSA\n"
                "cat > %6 <<'EOF_GSA_CONF'\n%7\nEOF_GSA_CONF\n"
                "cat > %8 <<'EOF_GSA_CONN'\n%9\nEOF_GSA_CONN\n"
                "cat > %10 <<'EOF_GSA_KNOWN'\n%11\nEOF_GSA_KNOWN\n"
                "chmod 700 %1; "
                "chmod 600 %6 %8 %10; "
                "chmod 700 %5; "
                "cat > %3 <<'EOF_GSA_PLIST'\n%4\nEOF_GSA_PLIST\n"
                "chmod 644 %3; "
                "launchctl bootout system/org.zfsmgr.gsa >/dev/null 2>&1 || true; "
                "launchctl bootstrap system %3; "
                "launchctl enable system/org.zfsmgr.gsa; "
                "launchctl kickstart -k system/org.zfsmgr.gsa >/dev/null 2>&1 || true")
                            .arg(QString::fromLatin1(kGsaUnixScriptPath),
                                 scriptPayload,
                                 QString::fromLatin1(kGsaMacPlistPath),
                                 plistPayload,
                                 QString::fromLatin1(kGsaUnixConfigDirPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 mainConfigPayload,
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 connectionsPayload,
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 knownHostsPayload);
        } else if (isFreeBsd) {
            remoteCmd = QStringLiteral(
                "if ! command -v crontab >/dev/null 2>&1; then echo 'cron not available' >&2; exit 1; fi; "
                "mkdir -p /usr/local/libexec %7 %14; "
                "cat > %1 <<'EOF_GSA'\n%2\nEOF_GSA\n"
                "cat > %8 <<'EOF_GSA_CONF'\n%9\nEOF_GSA_CONF\n"
                "cat > %10 <<'EOF_GSA_CONN'\n%11\nEOF_GSA_CONN\n"
                "cat > %12 <<'EOF_GSA_KNOWN'\n%13\nEOF_GSA_KNOWN\n"
                "chmod 700 %1; "
                "chmod 600 %8 %10 %12; "
                "chmod 700 %7 %14; "
                "chown root:wheel %1 %8 %10 %12; "
                "chown root:wheel %7 %14; "
                "tmp=$(mktemp); "
                "{ crontab -l 2>/dev/null || true; } | "
                "awk -v begin=%3 -v end=%4 'BEGIN { skip=0 } "
                "$0==begin { skip=1; next } "
                "$0==end { skip=0; next } "
                "skip==0 { print }' > \"$tmp\"; "
                "printf '%s\n' %3 >> \"$tmp\"; "
                "printf '%s\n' '0 * * * * /usr/local/libexec/zfsmgr-gsa.sh' >> \"$tmp\"; "
                "printf '%s\n' %4 >> \"$tmp\"; "
                "crontab \"$tmp\"; "
                "rm -f \"$tmp\"")
                            .arg(QString::fromLatin1(kGsaUnixScriptPath),
                                 scriptPayload,
                                 mwhelpers::shSingleQuote(QString::fromLatin1(kGsaFreeBsdCronMarkerBegin)),
                                 mwhelpers::shSingleQuote(QString::fromLatin1(kGsaFreeBsdCronMarkerEnd)),
                                 QString(),
                                 QString(),
                                 QString::fromLatin1(kGsaUnixConfigDirPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 mainConfigPayload,
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 connectionsPayload,
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 knownHostsPayload,
                                 runtimeConfigDir);
        } else {
            const QString servicePayload = QString::fromUtf8(R"([Unit]
Description=ZFSMgr automatic snapshot manager

[Service]
Type=oneshot
User=root
Group=root
ExecStart=/usr/local/libexec/zfsmgr-gsa.sh
)");
            const QString timerPayload = QString::fromUtf8(R"([Unit]
Description=Run ZFSMgr automatic snapshot manager hourly

[Timer]
OnCalendar=hourly
Persistent=true
Unit=zfsmgr-gsa.service

[Install]
WantedBy=timers.target
)");
            remoteCmd = QStringLiteral(
                "if ! command -v systemctl >/dev/null 2>&1; then echo 'systemd not available' >&2; exit 1; fi; "
                "mkdir -p /usr/local/libexec %7 %14; "
                "cat > %1 <<'EOF_GSA'\n%2\nEOF_GSA\n"
                "cat > %8 <<'EOF_GSA_CONF'\n%9\nEOF_GSA_CONF\n"
                "cat > %10 <<'EOF_GSA_CONN'\n%11\nEOF_GSA_CONN\n"
                "cat > %12 <<'EOF_GSA_KNOWN'\n%13\nEOF_GSA_KNOWN\n"
                "chmod 700 %1; "
                "chmod 600 %8 %10 %12; "
                "chmod 700 %7 %14; "
                "cat > %3 <<'EOF_GSA_SERVICE'\n%4\nEOF_GSA_SERVICE\n"
                "cat > %5 <<'EOF_GSA_TIMER'\n%6\nEOF_GSA_TIMER\n"
                "chown root:root %1 %8 %10 %12 %3 %5; "
                "chown root:root %7 %14; "
                "systemctl daemon-reload; "
                "systemctl enable --now zfsmgr-gsa.timer")
                            .arg(QString::fromLatin1(kGsaUnixScriptPath),
                                 scriptPayload,
                                 QString::fromLatin1(kGsaLinuxServicePath),
                                 servicePayload,
                                 QString::fromLatin1(kGsaLinuxTimerPath),
                                 timerPayload,
                                 QString::fromLatin1(kGsaUnixConfigDirPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 mainConfigPayload,
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 connectionsPayload,
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 knownHostsPayload,
                                 runtimeConfigDir);
        }
        remoteCmd = withSudo(p, remoteCmd);
    }

    beginUiBusy();
    updateStatus(actionLabel + QStringLiteral("..."));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QString detail;
    const bool ok = executeConnectionCommand(idx, actionLabel, remoteCmd, 240000, &detail, winMode);
    endUiBusy();
    if (!ok) {
        if (interactive) {
            QMessageBox::warning(
                this,
                QStringLiteral("ZFSMgr"),
                trk(QStringLiteral("t_gsa_install_fail_001"),
                    QStringLiteral("No se pudo instalar/actualizar el GSA en \"%1\".\n\n%2"),
                    QStringLiteral("Could not install/update GSA on \"%1\".\n\n%2"),
                    QStringLiteral("无法在 \"%1\" 上安装/更新 GSA。\n\n%2"))
                    .arg(p.name, detail.simplified().left(500)));
        } else {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Actualización automática de GSA fallida en \"%1\": %2")
                       .arg(p.name,
                            detail.simplified().left(500)));
        }
        refreshConnectionByIndex(idx);
        return false;
    }
    refreshConnectionByIndex(idx);
    return true;
}

void MainWindow::refreshInstalledGsaAfterConnectionChange(const QString& changedConnectionName) {
    QVector<int> targets;
    for (int i = 0; i < m_profiles.size() && i < m_states.size(); ++i) {
        if (!m_states[i].gsaInstalled) {
            continue;
        }
        if (isConnectionDisconnected(i)) {
            continue;
        }
        targets.push_back(i);
    }
    if (targets.isEmpty()) {
        return;
    }

    const QString targetList = [&]() {
        QStringList names;
        names.reserve(targets.size());
        for (int idx : targets) {
            const ConnectionProfile& cp = m_profiles[idx];
            names << (cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed());
        }
        return names.join(QStringLiteral(", "));
    }();

    QMessageBox::information(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_gsa_auto_refresh_notice_001"),
            QStringLiteral("La conexión \"%1\" ha cambiado. ZFSMgr actualizará automáticamente los GSA instalados en: %2"),
            QStringLiteral("Connection \"%1\" changed. ZFSMgr will automatically update installed GSA on: %2"),
            QStringLiteral("连接 \"%1\" 已变更。ZFSMgr 将自动更新以下连接上的已安装 GSA：%2"))
            .arg(changedConnectionName.isEmpty() ? QStringLiteral("?") : changedConnectionName, targetList));

    QStringList okNames;
    QStringList failedNames;
    for (int idx : targets) {
        const ConnectionProfile& cp = m_profiles[idx];
        const QString name = cp.name.trimmed().isEmpty() ? cp.id.trimmed() : cp.name.trimmed();
        appLog(QStringLiteral("INFO"),
               QStringLiteral("Actualizando automáticamente GSA en \"%1\" por cambio de conexión").arg(name));
        if (installOrUpdateGsaForConnectionInternal(idx, false)) {
            okNames << name;
        } else {
            failedNames << name;
        }
    }

    QStringList summary;
    if (!okNames.isEmpty()) {
        summary << trk(QStringLiteral("t_gsa_auto_refresh_ok_001"),
                       QStringLiteral("Actualizados: %1"),
                       QStringLiteral("Updated: %1"),
                       QStringLiteral("已更新：%1"))
                          .arg(okNames.join(QStringLiteral(", ")));
    }
    if (!failedNames.isEmpty()) {
        summary << trk(QStringLiteral("t_gsa_auto_refresh_fail_001"),
                       QStringLiteral("Con error: %1"),
                       QStringLiteral("Failed: %1"),
                       QStringLiteral("失败：%1"))
                          .arg(failedNames.join(QStringLiteral(", ")));
    }
    if (!summary.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("ZFSMgr"), summary.join(QStringLiteral("\n")));
    }
}

bool MainWindow::uninstallGsaForConnection(int idx) {
    if (actionsLocked()) {
        return false;
    }
    if (idx < 0 || idx >= m_profiles.size()) {
        return false;
    }
    if (isConnectionDisconnected(idx)) {
        QMessageBox::information(this,
                                 QStringLiteral("ZFSMgr"),
                                 trk(QStringLiteral("t_gsa_conn_disc_001"),
                                     QStringLiteral("La conexión está desconectada."),
                                     QStringLiteral("The connection is disconnected."),
                                     QStringLiteral("该连接已断开。")));
        return false;
    }
    if (idx >= m_states.size() || !m_states[idx].gsaInstalled) {
        return false;
    }

    const ConnectionProfile& p = m_profiles[idx];
    const auto confirm = QMessageBox::question(
        this,
        QStringLiteral("ZFSMgr"),
        trk(QStringLiteral("t_gsa_uninstall_confirm_001"),
            QStringLiteral("ZFSMgr desinstalará el GSA de \"%1\" y eliminará su programación nativa.\n\n¿Continuar?"),
            QStringLiteral("ZFSMgr will uninstall GSA from \"%1\" and remove its native schedule.\n\nContinue?"),
            QStringLiteral("ZFSMgr 将从 \"%1\" 卸载 GSA 并删除其原生计划任务。\n\n是否继续？"))
            .arg(p.name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return false;
    }

    QString remoteCmd;
    WindowsCommandMode winMode = WindowsCommandMode::Auto;
    if (isWindowsConnection(idx)) {
        winMode = WindowsCommandMode::PowerShellNative;
        const QString taskName = QString::fromLatin1(kGsaTaskName);
        remoteCmd = QStringLiteral(
            "$taskName='%1'; "
            "$dir='C:\\ProgramData\\ZFSMgr'; "
            "$script=Join-Path $dir 'gsa.ps1'; "
            "schtasks /Delete /F /TN $taskName >$null 2>&1; "
            "if (Test-Path -LiteralPath $script) { Remove-Item -Force -LiteralPath $script }; "
            "if (Test-Path -LiteralPath $dir) { "
            "  $remaining=Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue; "
            "  if (-not $remaining) { Remove-Item -Force -LiteralPath $dir -ErrorAction SilentlyContinue } "
            "}")
                        .arg(taskName);
    } else {
        const QString osHint = (p.osType + QStringLiteral(" ")
                                + ((idx < m_states.size()) ? m_states[idx].osLine : QString()))
                                   .trimmed()
                                   .toLower();
        const bool isMac = osHint.contains(QStringLiteral("darwin"));
        const bool isFreeBsd = osHint.contains(QStringLiteral("freebsd"));
        if (isMac) {
            remoteCmd = QStringLiteral(
                "launchctl bootout system/org.zfsmgr.gsa >/dev/null 2>&1 || true; "
                "launchctl disable system/org.zfsmgr.gsa >/dev/null 2>&1 || true; "
                "rm -f %1 %2 %3 %4 %5; "
                "rmdir %6 >/dev/null 2>&1 || true")
                            .arg(QString::fromLatin1(kGsaMacPlistPath),
                                 QString::fromLatin1(kGsaUnixScriptPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 QString::fromLatin1(kGsaUnixConfigDirPath));
        } else if (isFreeBsd) {
            remoteCmd = QStringLiteral(
                "if command -v crontab >/dev/null 2>&1; then "
                "  tmp=$(mktemp); "
                "  { crontab -l 2>/dev/null || true; } | "
                "  awk -v begin=%1 -v end=%2 'BEGIN { skip=0 } "
                "  $0==begin { skip=1; next } "
                "  $0==end { skip=0; next } "
                "  skip==0 { print }' > \"$tmp\"; "
                "  crontab \"$tmp\"; "
                "  rm -f \"$tmp\"; "
                "fi; "
                "rm -f %3 %4 %5 %6; "
                "rmdir %7 >/dev/null 2>&1 || true; "
                "rm -f %8; "
                "rmdir %9 >/dev/null 2>&1 || true")
                            .arg(mwhelpers::shSingleQuote(QString::fromLatin1(kGsaFreeBsdCronMarkerBegin)),
                                 mwhelpers::shSingleQuote(QString::fromLatin1(kGsaFreeBsdCronMarkerEnd)),
                                 QString::fromLatin1(kGsaUnixScriptPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 QString::fromLatin1(kGsaUnixConfigDirPath),
                                 QStringLiteral("%1/GSA.log").arg(QString::fromLatin1(kGsaFreeBsdRuntimeDirPath)),
                                 QString::fromLatin1(kGsaFreeBsdRuntimeDirPath));
        } else {
            remoteCmd = QStringLiteral(
                "if command -v systemctl >/dev/null 2>&1; then "
                "  systemctl disable --now zfsmgr-gsa.timer >/dev/null 2>&1 || true; "
                "  systemctl stop zfsmgr-gsa.service >/dev/null 2>&1 || true; "
                "fi; "
                "rm -f %1 %2 %3 %4 %5 %6; "
                "rmdir %7 >/dev/null 2>&1 || true; "
                "if command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload >/dev/null 2>&1 || true; fi")
                            .arg(QString::fromLatin1(kGsaUnixScriptPath),
                                 QString::fromLatin1(kGsaLinuxServicePath),
                                 QString::fromLatin1(kGsaLinuxTimerPath),
                                 QString::fromLatin1(kGsaUnixConfigPath),
                                 QString::fromLatin1(kGsaUnixConnectionsPath),
                                 QString::fromLatin1(kGsaUnixKnownHostsPath),
                                 QString::fromLatin1(kGsaUnixConfigDirPath));
        }
        remoteCmd = withSudo(p, remoteCmd);
    }

    beginUiBusy();
    updateStatus(trk(QStringLiteral("t_gsa_uninstall_progress_001"),
                     QStringLiteral("Desinstalando GSA de %1..."),
                     QStringLiteral("Uninstalling GSA from %1..."),
                     QStringLiteral("正在从 %1 卸载 GSA...")).arg(p.name));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QString detail;
    const bool ok = executeConnectionCommand(idx, QStringLiteral("Desinstalar GSA"), remoteCmd, 120000, &detail, winMode);
    endUiBusy();
    if (!ok) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_gsa_uninstall_fail_001"),
                QStringLiteral("No se pudo desinstalar el GSA de \"%1\".\n\n%2"),
                QStringLiteral("Could not uninstall GSA from \"%1\".\n\n%2"),
                QStringLiteral("无法从 \"%1\" 卸载 GSA。\n\n%2"))
                .arg(p.name, mwhelpers::oneLine(detail)));
        refreshConnectionByIndex(idx);
        return false;
    }
    refreshConnectionByIndex(idx);
    return true;
}

void MainWindow::deleteConnection() {
    if (actionsLocked()) {
        appLog(QStringLiteral("INFO"), QStringLiteral("Acción en curso: borrado bloqueado"));
        return;
    }
    const int idx = currentConnectionIndexFromUi();
    if (idx < 0 || idx >= m_profiles.size()) {
        return;
    }
    if (isLocalConnection(idx)) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_local_builtin_02"),
                QStringLiteral("La conexión local integrada no se puede borrar."),
                QStringLiteral("Built-in local connection cannot be deleted."),
                QStringLiteral("内置本地连接不可删除。")));
        return;
    }
    const bool redirectedLocal = isConnectionRedirectedToLocal(idx);
    if (redirectedLocal) {
        QMessageBox::information(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_conn_redirect_l3"),
                QStringLiteral("La conexión está redirigida a 'Local' y no se puede borrar."),
                QStringLiteral("This connection is redirected to 'Local' and cannot be deleted."),
                QStringLiteral("该连接已重定向到“本地”，不可删除。")));
        return;
    }
    const auto confirm = QMessageBox::question(
        this,
        trk(QStringLiteral("t_del_conn_tit1"), QStringLiteral("Borrar conexión"), QStringLiteral("Delete connection"), QStringLiteral("删除连接")),
        trk(QStringLiteral("t_del_conn_q001"), QStringLiteral("¿Borrar conexión \"%1\"?"),
            QStringLiteral("Delete connection \"%1\"?"),
            QStringLiteral("删除连接“%1”？")).arg(m_profiles[idx].name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    QString err;
    if (!m_store.deleteConnectionById(m_profiles[idx].id, err)) {
        QMessageBox::critical(this, QStringLiteral("ZFSMgr"),
                              trk(QStringLiteral("t_del_conn_err1"),
                                  QStringLiteral("No se pudo borrar conexión:\n%1"),
                                  QStringLiteral("Could not delete connection:\n%1"),
                                  QStringLiteral("无法删除连接：\n%1")).arg(err));
        return;
    }
    // Invalidate in-flight async refresh callbacks that may still be reporting
    // using stale indices while the profile list is being rebuilt.
    ++m_refreshGeneration;
    m_refreshPending = 0;
    m_refreshTotal = 0;
    m_refreshInProgress = false;
    updateConnectivityMatrixButtonState();
    updateBusyCursor();
    loadConnections();
}
