#include "mainwindow.h"
#include "mainwindow_helpers.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QEvent>
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

#include <QtConcurrent/QtConcurrent>

namespace {
constexpr int kConnPropKeyRole = Qt::UserRole + 14;
constexpr int kPoolNameRole = Qt::UserRole + 11;
constexpr int kIsPoolRootRole = Qt::UserRole + 12;
constexpr const char* kGsaTaskName = "ZFSMgr-GSA";
constexpr const char* kGsaUnixScriptPath = "/usr/local/libexec/zfsmgr-gsa.sh";
constexpr const char* kGsaUnixConfigDirPath = "/etc/zfsmgr";
constexpr const char* kGsaUnixConfigPath = "/etc/zfsmgr/gsa.conf";
constexpr const char* kGsaUnixConnectionsPath = "/etc/zfsmgr/gsa-connections.conf";
constexpr const char* kGsaUnixKnownHostsPath = "/etc/zfsmgr/gsa_known_hosts";
constexpr const char* kGsaMacPlistPath = "/Library/LaunchDaemons/org.zfsmgr.gsa.plist";
constexpr const char* kGsaLinuxServicePath = "/etc/systemd/system/zfsmgr-gsa.service";
constexpr const char* kGsaLinuxTimerPath = "/etc/systemd/system/zfsmgr-gsa.timer";
constexpr const char* kGsaVersionSuffix = ".3";
struct ConnTreeNavSnapshot {
    QSet<QString> expandedKeys;
    QString selectedKey;
};

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
    return QStringLiteral(ZFSMGR_APP_VERSION) + QString::fromLatin1(kGsaVersionSuffix);
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
  ds="$1"
  parent="${ds%/*}"
  while [ "$parent" != "$ds" ] && [ -n "$parent" ]; do
    if bool_on "$(prop_value "$parent" "$PROP_ENABLED")" \
       && bool_on "$(prop_value "$parent" "$PROP_RECURSIVE")"; then
      return 0
    fi
    ds="$parent"
    parent="${ds%/*}"
  done
  return 1
}

build_target_recv_command() {
  ds="$1"
  base="zfs recv -F $(shq "$ds")"
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
  base="zfs list -H -t snapshot -o name -s creation -r $(shq "$ds") 2>/dev/null"
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

list_target_gsa_snapshots() {
  ds="$1"
  if [ "$TARGET_MODE" = "local" ]; then
    zfs list -H -t snapshot -o name -s creation -r "$ds" 2>/dev/null
  else
    run_via_target_ssh "$(build_target_list_command "$ds")"
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

latest_common_gsa() {
  src_ds="$1"
  dst_ds="$2"
  src_snaps="$(zfs list -H -t snapshot -o name -s creation -r "$src_ds" 2>/dev/null | grep "^${src_ds}@GSA-" | sed "s#^${src_ds}@##" || true)"
  dst_snaps="$(list_target_gsa_snapshots "$dst_ds" | grep "^${dst_ds}@GSA-" | sed "s#^${dst_ds}@##" || true)"
  [ -n "$src_snaps" ] && [ -n "$dst_snaps" ] || return 0
  found=''
  printf '%s\n' "$src_snaps" | while IFS= read -r snap; do
    [ -z "$snap" ] && continue
    if printf '%s\n' "$dst_snaps" | grep -Fx "$snap" >/dev/null 2>&1; then
      found="$snap"
    fi
  done
  printf '%s\n' "${found:-}"
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
  common="$(latest_common_gsa "$src_ds" "$dst_dataset")"
  recv_cmd="$(build_target_recv_command "$dst_dataset")"
  if [ "$TARGET_MODE" = "local" ]; then
    if ! zfs list -H -o name "$dst_dataset" >/dev/null 2>&1; then
      log "GSA level skip for $src_ds: destination dataset not found ($dst_dataset)"
      return 0
    fi
    if bool_on "$recursive"; then
      if [ -n "$common" ]; then
        zfs send -R -I "@${common}" "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
      else
        zfs send -R "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
      fi
    else
      if [ -n "$common" ]; then
        zfs send -I "@${common}" "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
      else
        zfs send "${src_ds}@${snap_name}" | sh -lc "$recv_cmd"
      fi
    fi
  else
    if ! run_via_target_ssh "true" >/dev/null 2>&1; then
      log "GSA level skip for $src_ds: SSH no disponible hacia $dst_conn"
      return 0
    fi
    if bool_on "$recursive"; then
      if [ -n "$common" ]; then
        zfs send -R -I "@${common}" "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
      else
        zfs send -R "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
      fi
    else
      if [ -n "$common" ]; then
        zfs send -I "@${common}" "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
      else
        zfs send "${src_ds}@${snap_name}" | run_via_target_ssh "$recv_cmd"
      fi
    fi
  fi
}

main() {
  init_logging
  log "GSA start version __VERSION__"
  datasets="$(zfs list -H -o name -t filesystem 2>/dev/null || true)"
  [ -n "$datasets" ] || exit 0
  printf '%s\n' "$datasets" | while IFS= read -r ds; do
    [ -n "$ds" ] || continue
    enabled="$(prop_value "$ds" "$PROP_ENABLED")"
    bool_on "$enabled" || continue
    recursive="$(prop_value "$ds" "$PROP_RECURSIVE")"
    if has_recursive_gsa_ancestor "$ds"; then
      log "GSA skip for $ds: cubierto por ancestro con programación recursiva"
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
  done
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
    if ((Test-On (Get-PropValue $current $PropEnabled)) -and (Test-On (Get-PropValue $current $PropRecursive))) {
      return $true
    }
  }
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

function Get-LatestCommonGsa([string]$SrcDataset, [string]$DstDataset) {
  $src = @((& zfs list -H -t snapshot -o name -s creation -r $SrcDataset 2>$null) |
    Where-Object { $_ -like "$SrcDataset@GSA-*" } |
    ForEach-Object { $_.Substring($SrcDataset.Length + 1) })
  $dstSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
  ((& zfs list -H -t snapshot -o name -s creation -r $DstDataset 2>$null) |
    Where-Object { $_ -like "$DstDataset@GSA-*" } |
    ForEach-Object { $null = $dstSet.Add($_.Substring($DstDataset.Length + 1)) })
  $common = $null
  foreach ($snap in $src) {
    if ($dstSet.Contains($snap)) { $common = $snap }
  }
  return $common
}

function Invoke-GsaLevel([string]$SrcDataset, [bool]$Recursive, [string]$SnapName, [string]$DestSpec, [bool]$LevelOn) {
  if (-not $LevelOn -or [string]::IsNullOrWhiteSpace($DestSpec)) { return }
  if ($DestSpec -notmatch '^(?<conn>[^:]+)::(?<dataset>.+)$') { return }
  $dstConn = $Matches['conn']
  $dstDataset = $Matches['dataset']
  if ($dstConn -ne $SelfConnection -and $dstConn -ne 'Local') { return }
  try {
    & zfs list -H -o name $dstDataset | Out-Null
  } catch {
    return
  }
  $common = Get-LatestCommonGsa $SrcDataset $DstDataset
  if ($Recursive) {
    if ($common) {
      & powershell -NoProfile -Command "& zfs send -R -I '@$common' '$SrcDataset@$SnapName' | zfs recv -F '$DstDataset'" | Out-Null
    } else {
      & powershell -NoProfile -Command "& zfs send -R '$SrcDataset@$SnapName' | zfs recv -F '$DstDataset'" | Out-Null
    }
  } else {
    if ($common) {
      & powershell -NoProfile -Command "& zfs send -I '@$common' '$SrcDataset@$SnapName' | zfs recv -F '$DstDataset'" | Out-Null
    } else {
      & powershell -NoProfile -Command "& zfs send '$SrcDataset@$SnapName' | zfs recv -F '$DstDataset'" | Out-Null
    }
  }
try {
  Rotate-GsaLog
  Start-Transcript -LiteralPath $LogFile -Append | Out-Null
  Write-GsaLog ("GSA start version " + '__VERSION__')
  $datasets = @((& zfs list -H -o name -t filesystem 2>$null))
  foreach ($ds in $datasets) {
    if ([string]::IsNullOrWhiteSpace($ds)) { continue }
    if (-not (Test-On (Get-PropValue $ds $PropEnabled))) { continue }
    if (Test-HasRecursiveGsaAncestor $ds) {
      Write-GsaLog ("GSA skip for " + $ds + ": cubierto por ancestro con programación recursiva")
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
    foreach ($klass in (Get-DueClasses $hourly $daily $weekly $monthly $yearly)) {
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
    bool ok{false};
};

QString connectivityMatrixRemoteProbe(const ConnectionProfile& target) {
    if (target.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(QStringLiteral("echo ZFSMGR_CONNECT_OK"));
    if (!target.password.trimmed().isEmpty()) {
        return QStringLiteral(
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
    }
    return mwhelpers::sshBaseCommand(target) + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd;
}

QString connectivityMatrixRsyncProbe(const ConnectionProfile& target) {
    if (target.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    const QString targetHost = mwhelpers::shSingleQuote(mwhelpers::sshUserHost(target));
    const QString targetCmd = mwhelpers::shSingleQuote(
        QStringLiteral("if command -v rsync >/dev/null 2>&1; then echo ZFSMGR_RSYNC_OK; else echo ZFSMGR_RSYNC_MISSING >&2; exit 127; fi"));
    const QString baseProbe = target.password.trimmed().isEmpty()
        ? mwhelpers::sshBaseCommand(target) + QStringLiteral(" ") + targetHost + QStringLiteral(" ") + targetCmd
        : QStringLiteral(
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
    const bool isTop = (connIdx == m_topDetailConnIdx);
    const bool isBottom = (connIdx == m_bottomDetailConnIdx);
    if (isTop && isBottom) {
        return QStringLiteral("both");
    }
    if (isTop) {
        return QStringLiteral("source");
    }
    if (isBottom) {
        return QStringLiteral("target");
    }
    return QString();
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
    QString sshOut;
    QString sshErr;
    int sshRc = -1;
    const bool sshOk = runSsh(src, sshCmd, 12000, sshOut, sshErr, sshRc);
    const QString sshMerged = (sshOut + QStringLiteral("\n") + sshErr).trimmed();
    const bool sshProbeOk = sshOk && sshRc == 0 && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));
    if (!sshProbeOk) {
        return fail(sshMerged.isEmpty() ? QStringLiteral("ssh exit %1").arg(sshRc) : sshMerged.left(300));
    }
    if (errorOut) {
        errorOut->clear();
    }
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_connectionsTable || (m_connectionsTable && watched == m_connectionsTable->viewport()))
        && event
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest)) {
        QTimer::singleShot(0, this, [this]() { repositionConnectivityButton(); });
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::repositionConnectivityButton() {
    if (!m_connectionsTable || !m_connectivityMatrixBtn || !m_connectionsTable->viewport()) {
        return;
    }
    QWidget* host = m_connectionsTable->viewport();
    const QSize hint = m_connectivityMatrixBtn->sizeHint();
    const int margin = 10;
    const int x = qMax(margin, host->width() - hint.width() - margin);
    const int y = qMax(margin, host->height() - hint.height() - margin);
    m_connectivityMatrixBtn->setGeometry(x, y, hint.width(), hint.height());
    m_connectivityMatrixBtn->raise();
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
            return result;
        }
        if (rowIdx == colIdx || sameMachine(rowIdx, colIdx)) {
            result.text = composeText(QStringLiteral("✓"), QStringLiteral("✓"));
            result.tooltip = trk(QStringLiteral("t_connectivity_same_machine_001"),
                                 QStringLiteral("Misma máquina."),
                                 QStringLiteral("Same machine."),
                                 QStringLiteral("同一台机器。"));
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
                    result.ok = true;
                    return result;
                }
            } else {
                result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
                result.tooltip = trk(QStringLiteral("t_connectivity_local_no_ssh_001"),
                                     QStringLiteral("Local no tiene una conexión SSH equivalente para comprobarla remotamente."),
                                     QStringLiteral("Local has no equivalent SSH connection for remote probing."),
                                     QStringLiteral("本地连接没有可用于远程探测的等效 SSH 连接。"));
                return result;
            }
        }
        if (effectiveDst.connType.trimmed().compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_unsupported_target_001"),
                                 QStringLiteral("Solo se comprueba conectividad SSH hacia conexiones SSH/Local."),
                                 QStringLiteral("Only SSH connectivity to SSH/Local connections is checked."),
                                 QStringLiteral("只检查到 SSH/本地连接的 SSH 连通性。"));
            return result;
        }
        if (src.connType.trimmed().compare(QStringLiteral("PSRP"), Qt::CaseInsensitive) == 0) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            result.tooltip = trk(QStringLiteral("t_connectivity_unsupported_source_001"),
                                 QStringLiteral("No se comprueba conectividad saliente desde conexiones PSRP."),
                                 QStringLiteral("Outgoing connectivity is not checked from PSRP connections."),
                                 QStringLiteral("不检查来自 PSRP 连接的出站连通性。"));
            return result;
        }
        const QString sshCmd = connectivityMatrixRemoteProbe(effectiveDst);
        if (sshCmd.trimmed().isEmpty()) {
            result.text = composeText(QStringLiteral("-"), QStringLiteral("-"));
            return result;
        }
        QString sshOut;
        QString sshErr;
        int sshRc = -1;
        const bool sshOk = runSsh(src, sshCmd, 12000, sshOut, sshErr, sshRc);
        const QString sshMerged = (sshOut + QStringLiteral("\n") + sshErr).trimmed();
        const bool sshProbeOk = sshOk && sshRc == 0 && sshMerged.contains(QStringLiteral("ZFSMGR_CONNECT_OK"));

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
                QString rsyncOut;
                QString rsyncErr;
                int rsyncRc = -1;
                const bool rsyncOk = runSsh(src, rsyncCmd, 12000, rsyncOut, rsyncErr, rsyncRc);
                const QString rsyncMerged = (rsyncOut + QStringLiteral("\n") + rsyncErr).trimmed();
                if (rsyncOk && rsyncRc == 0 && rsyncMerged.contains(QStringLiteral("ZFSMGR_RSYNC_OK"))) {
                    rsyncState = QStringLiteral("✓");
                    tooltipLines << trk(QStringLiteral("t_connectivity_rsync_ok_001"),
                                        QStringLiteral("rsync disponible en origen y destino."),
                                        QStringLiteral("rsync available on source and target."),
                                        QStringLiteral("源端和目标端均可用 rsync。"));
                } else {
                    rsyncState = QStringLiteral("✗");
                    tooltipLines << (rsyncMerged.isEmpty()
                                         ? QStringLiteral("rsync exit %1").arg(rsyncRc)
                                         : rsyncMerged.left(300));
                }
            }
            result.text = composeText(QStringLiteral("✓"), rsyncState);
            result.tooltip = tooltipLines.join(QStringLiteral("\n"));
            result.ok = true;
            return result;
        }
        result.text = composeText(QStringLiteral("✗"), QStringLiteral("-"));
        result.tooltip = sshMerged.isEmpty() ? QStringLiteral("ssh exit %1").arg(sshRc) : sshMerged.left(300);
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
    matrix->setSelectionMode(QAbstractItemView::NoSelection);
    matrix->setAlternatingRowColors(false);
    beginUiBusy();
    m_connectivityMatrixInProgress = true;
    updateConnectivityMatrixButtonState();
    for (int r = 0; r < m_profiles.size(); ++r) {
        for (int c = 0; c < m_profiles.size(); ++c) {
            const ConnectivityProbeResult probe = probeConnectivity(r, c);
            auto* item = new QTableWidgetItem(probe.text);
            item->setTextAlignment(Qt::AlignCenter);
            item->setToolTip(probe.tooltip);
            if (probe.ok) {
                item->setForeground(QBrush(QColor(QStringLiteral("#1d6f42"))));
            } else if (probe.text.contains(QStringLiteral("✗"))) {
                item->setForeground(QBrush(QColor(QStringLiteral("#8b1e1e"))));
            }
            matrix->setItem(r, c, item);
            qApp->processEvents();
        }
    }
    m_connectivityMatrixInProgress = false;
    updateConnectivityMatrixButtonState();
    endUiBusy();
    layout->addWidget(matrix, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::syncConnectionDisplaySelectors() {
    if (!m_connectionsTable) {
        return;
    }
    const QSignalBlocker blocker(m_connectionsTable);
    m_syncConnSelectorChecks = true;
    for (int row = 0; row < m_connectionsTable->rowCount(); ++row) {
        QTableWidgetItem* connItem = m_connectionsTable->item(row, 0);
        QTableWidgetItem* srcItem = m_connectionsTable->item(row, 1);
        QTableWidgetItem* dstItem = m_connectionsTable->item(row, 2);
        if (!connItem || !srcItem || !dstItem) {
            continue;
        }
        const int connIdx = connItem->data(Qt::UserRole).toInt();
        const QString mode = connectionDisplayModeForIndex(connIdx);
        srcItem->setCheckState((mode == QStringLiteral("source") || mode == QStringLiteral("both"))
                                   ? Qt::Checked
                                   : Qt::Unchecked);
        dstItem->setCheckState((mode == QStringLiteral("target") || mode == QStringLiteral("both"))
                                   ? Qt::Checked
                                   : Qt::Unchecked);
    }
    m_syncConnSelectorChecks = false;
}

void MainWindow::applyConnectionDisplayMode(int connIdx, const QString& modeRaw) {
    if (!m_connectionsTable || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    const QString mode = modeRaw.trimmed().toLower();
    if (isConnectionDisconnected(connIdx)) {
        syncConnectionDisplaySelectors();
        return;
    }

    const int prevTop = m_topDetailConnIdx;
    const int prevBottom = m_bottomDetailConnIdx;
    const bool wantTop = (mode == QStringLiteral("source") || mode == QStringLiteral("both"));
    const bool wantBottom = (mode == QStringLiteral("target") || mode == QStringLiteral("both"));

    if (wantTop && prevTop >= 0 && prevTop != connIdx) {
        saveTopTreeStateForConnection(prevTop);
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
        }
    } else if (!wantTop && prevTop == connIdx) {
        saveTopTreeStateForConnection(connIdx);
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
        }
    }

    if (wantBottom && prevBottom >= 0 && prevBottom != connIdx) {
        saveBottomTreeStateForConnection(prevBottom);
    } else if (!wantBottom && prevBottom == connIdx) {
        saveBottomTreeStateForConnection(connIdx);
    }

    m_topDetailConnIdx = wantTop ? connIdx : ((prevTop == connIdx) ? -1 : prevTop);
    m_bottomDetailConnIdx = wantBottom ? connIdx : ((prevBottom == connIdx) ? -1 : prevBottom);
    m_forceRestoreTopStateConnIdx = wantTop ? connIdx : ((prevTop == connIdx) ? -1 : m_forceRestoreTopStateConnIdx);
    m_forceRestoreBottomStateConnIdx =
        wantBottom ? connIdx : ((prevBottom == connIdx) ? -1 : m_forceRestoreBottomStateConnIdx);

    if (prevTop == connIdx && !wantTop) {
        setConnectionOriginSelection(DatasetSelectionContext{});
    }
    if (prevBottom == connIdx && !wantBottom) {
        setConnectionDestinationSelection(DatasetSelectionContext{});
    }

    syncConnectionDisplaySelectors();

    const bool topChanged = (prevTop != m_topDetailConnIdx);
    const bool bottomChanged = (prevBottom != m_bottomDetailConnIdx);
    if (wantTop || wantBottom) {
        m_userSelectedConnectionKey = m_profiles[connIdx].id.trimmed().toLower();
        if (m_userSelectedConnectionKey.isEmpty()) {
            m_userSelectedConnectionKey = m_profiles[connIdx].name.trimmed().toLower();
        }
        const int row = rowForConnectionIndex(m_connectionsTable, connIdx);
        if (row >= 0) {
            m_connectionsTable->setCurrentCell(row, 0);
        }
    }
    if (topChanged) {
        rebuildConnectionEntityTabs();
        refreshConnectionNodeDetails();
    }
    if (bottomChanged || topChanged) {
        updateSecondaryConnectionDetail();
    }
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
        m_refreshInProgress = false;
        updateBusyCursor();
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
        rebuildConnectionsTable();
        populateAllPoolsTables();
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
    const int idx = selectedConnectionRow(m_connectionsTable);
    if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
        return;
    }
    if (m_connectionEntityTabs) {
        const int t = m_connectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_connectionEntityTabs->count()) {
            const QString key = m_connectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshTopTabDataByConn[idx] = key;
            }
        }
    }
    if (m_bottomConnectionEntityTabs && m_bottomConnectionEntityTabs->isVisible()) {
        const int t = m_bottomConnectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshBottomTabDataByConn[idx] = key;
            }
        }
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
    const int selectedIdx = selectedConnectionRow(m_connectionsTable);
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
    cachePoolStatusTextsForConnection(targetIdx, state);
    rebuildConnectionsTable();
    if (m_connectionEntityTabs) {
        const QString wantedTop = m_pendingRefreshTopTabDataByConn.value(targetIdx);
        if (!wantedTop.isEmpty()) {
            for (int t = 0; t < m_connectionEntityTabs->count(); ++t) {
                if (m_connectionEntityTabs->tabData(t).toString() == wantedTop) {
                    m_connectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    if (m_bottomConnectionEntityTabs) {
        const QString wantedBottom = m_pendingRefreshBottomTabDataByConn.value(targetIdx);
        if (!wantedBottom.isEmpty()) {
            for (int t = 0; t < m_bottomConnectionEntityTabs->count(); ++t) {
                if (m_bottomConnectionEntityTabs->tabData(t).toString() == wantedBottom) {
                    m_bottomConnectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    m_pendingRefreshTopTabDataByConn.remove(targetIdx);
    m_pendingRefreshBottomTabDataByConn.remove(targetIdx);
    if (selectedIdx >= 0 && m_connectionsTable) {
        const int row = rowForConnectionIndex(m_connectionsTable, selectedIdx);
        if (row >= 0) {
            m_connectionsTable->setCurrentCell(row, 0);
        }
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
    if (m_connectionsTable && m_connectionsTable->rowCount() > 0 && m_connectionsTable->currentRow() < 0) {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                m_connectionsTable->setCurrentCell(r, 0);
                break;
            }
        }
    }
    refreshConnectionNodeDetails();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Refresco paralelo finalizado"));
    m_refreshInProgress = false;
    updateBusyCursor();
    updateConnectivityMatrixButtonState();
    if (m_busyOnImportRefresh) {
        m_busyOnImportRefresh = false;
        endUiBusy();
    }
}

void MainWindow::onConnectionSelectionChanged() {
    QWidget* paintRoot = m_poolDetailTabs ? m_poolDetailTabs : static_cast<QWidget*>(m_rightTabs);
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(false);
    }

    QString selectionKey;
    if (m_connectionsTable) {
        int idx = m_topDetailConnIdx;
        if (idx < 0) {
            idx = selectedConnectionRow(m_connectionsTable);
        }
        if (idx >= 0 && isConnectionDisconnected(idx)) {
            idx = -1;
        }
        const QString tabKey =
            (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0
             && m_connectionEntityTabs->currentIndex() < m_connectionEntityTabs->count())
                ? m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString()
                : QString();
        selectionKey = QStringLiteral("%1|%2").arg(idx).arg(tabKey);
    }
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
        const bool bottomLoaded = detailLoadedFor(m_bottomDetailConnIdx, m_bottomConnContentTree);
        if (topLoaded && bottomLoaded) {
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
        updateSecondaryConnectionDetail();
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
    updateSecondaryConnectionDetail();
    if (paintRoot) {
        paintRoot->setUpdatesEnabled(true);
        paintRoot->update();
    }
}

void MainWindow::updateSecondaryConnectionDetail() {
    if (!m_bottomConnContentTree) {
        return;
    }
    QScopedValueRollback<bool> rebuildingGuard(m_rebuildingBottomConnContentTree, true);
    const QSignalBlocker blockBottomTree(m_bottomConnContentTree);
    ConnTreeNavSnapshot nav;
    const int pendingConnIdx = m_bottomDetailConnIdx;
    if (pendingConnIdx >= 0 && m_pendingBottomExpandedKeysByConn.contains(pendingConnIdx)) {
        nav.expandedKeys = m_pendingBottomExpandedKeysByConn.take(pendingConnIdx);
        nav.selectedKey = m_pendingBottomSelectedKeyByConn.take(pendingConnIdx);
    } else {
        nav = captureConnTreeNavSnapshot(m_bottomConnContentTree);
    }
    if (pendingConnIdx >= 0 && m_forceRestoreBottomStateConnIdx == pendingConnIdx) {
        nav.expandedKeys = m_savedBottomExpandedKeysByConn.value(pendingConnIdx);
        nav.selectedKey = m_savedBottomSelectedKeyByConn.value(pendingConnIdx);
        m_forceRestoreBottomStateConnIdx = -1;
    } else if (nav.expandedKeys.isEmpty() && nav.selectedKey.isEmpty() && pendingConnIdx >= 0) {
        nav.expandedKeys = m_savedBottomExpandedKeysByConn.value(pendingConnIdx);
        nav.selectedKey = m_savedBottomSelectedKeyByConn.value(pendingConnIdx);
    }
    m_bottomConnContentTree->clear();
    if (m_bottomDetailConnIdx < 0 || m_bottomDetailConnIdx >= m_profiles.size()
        || m_bottomDetailConnIdx >= m_states.size() || isConnectionDisconnected(m_bottomDetailConnIdx)) {
        QTreeWidget* prevTree = m_connContentTree;
        m_connContentTree = m_bottomConnContentTree;
        syncConnContentPropertyColumns();
        m_connContentTree = prevTree;
        return;
    }
    const ConnectionRuntimeState st = m_states[m_bottomDetailConnIdx];
    populateConnectionPoolsIntoTree(m_bottomConnContentTree, m_bottomDetailConnIdx, st);
    if (m_bottomConnContentTree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        m_bottomConnContentTree->addTopLevelItem(noPools);
    }
    restoreSnapshotSelectionInTree(m_bottomConnContentTree, nav);
    restoreConnTreeNavSnapshot(m_bottomConnContentTree, nav);
    expandPoolRootsIfNoNav(m_bottomConnContentTree, nav);
    saveBottomTreeStateForConnection(m_bottomDetailConnIdx);
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
    if (connIdx < 0 || !m_bottomConnContentTree) {
        return;
    }
    const ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_bottomConnContentTree);
    m_savedBottomExpandedKeysByConn[connIdx] = nav.expandedKeys;
    m_savedBottomSelectedKeyByConn[connIdx] = nav.selectedKey;
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
    if (connIdx < 0 || !m_bottomConnContentTree) {
        return;
    }
    ConnTreeNavSnapshot nav;
    nav.expandedKeys = m_savedBottomExpandedKeysByConn.value(connIdx);
    nav.selectedKey = m_savedBottomSelectedKeyByConn.value(connIdx);
    restoreConnTreeNavSnapshot(m_bottomConnContentTree, nav);
}

void MainWindow::rebuildConnectionEntityTabs() {
    if (!m_connContentTree || !m_connectionsTable) {
        return;
    }
    QScopedValueRollback<bool> rebuildingGuard(m_rebuildingTopConnContentTree, true);
    ConnTreeNavSnapshot nav = captureConnTreeNavSnapshot(m_connContentTree);
    if (m_connectionEntityTabs) {
        while (m_connectionEntityTabs->count() > 0) {
            m_connectionEntityTabs->removeTab(m_connectionEntityTabs->count() - 1);
        }
    }
    int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size() || connIdx >= m_states.size()
        || isConnectionDisconnected(connIdx)) {
        m_connContentTree->clear();
        syncConnContentPropertyColumns();
        return;
    }
    if (m_forceRestoreTopStateConnIdx == connIdx) {
        nav.expandedKeys = m_savedTopExpandedKeysByConn.value(connIdx);
        nav.selectedKey = m_savedTopSelectedKeyByConn.value(connIdx);
        m_forceRestoreTopStateConnIdx = -1;
    } else if (nav.expandedKeys.isEmpty() && nav.selectedKey.isEmpty()) {
        nav.expandedKeys = m_savedTopExpandedKeysByConn.value(connIdx);
        nav.selectedKey = m_savedTopSelectedKeyByConn.value(connIdx);
    }
    m_connContentTree->clear();
    const ConnectionRuntimeState st = m_states[connIdx];
    populateConnectionPoolsIntoTree(m_connContentTree, connIdx, st);
    if (m_connContentTree->topLevelItemCount() == 0) {
        auto* noPools = new QTreeWidgetItem();
        noPools->setText(0, trk(QStringLiteral("t_no_pools_001"),
                                QStringLiteral("Sin Pools"),
                                QStringLiteral("No Pools"),
                                QStringLiteral("无存储池")));
        QFont f = noPools->font(0);
        f.setItalic(true);
        noPools->setFont(0, f);
        noPools->setFlags((noPools->flags() & ~Qt::ItemIsSelectable) & ~Qt::ItemIsEnabled);
        m_connContentTree->addTopLevelItem(noPools);
    }
    restoreSnapshotSelectionInTree(m_connContentTree, nav);
    restoreConnTreeNavSnapshot(m_connContentTree, nav);
    expandPoolRootsIfNoNav(m_connContentTree, nav);
    saveTopTreeStateForConnection(connIdx);
    QTimer::singleShot(0, this, [this]() {
        syncConnContentPoolColumns();
    });
}

void MainWindow::populateConnectionPoolsIntoTree(QTreeWidget* tree,
                                                 int connIdx,
                                                 const ConnectionRuntimeState& st) {
    if (!tree || connIdx < 0 || connIdx >= m_profiles.size()) {
        return;
    }
    auto addPoolTree = [this, tree](int cidx, const QString& poolName, bool allowRemoteLoadIfMissing) {
        QTreeWidget tmp;
        populateDatasetTree(&tmp, cidx, poolName, QStringLiteral("conncontent_multi"), allowRemoteLoadIfMissing);
        while (tmp.topLevelItemCount() > 0) {
            tree->addTopLevelItem(tmp.takeTopLevelItem(0));
        }
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

void MainWindow::onConnectionEntityTabChanged(int idx) {
    if (m_updatingConnectionEntityTabs || !m_connectionEntityTabs || !m_connectionsTable) {
        return;
    }
    if (idx < 0 || idx >= m_connectionEntityTabs->count()) {
        return;
    }
    const QString key = m_connectionEntityTabs->tabData(idx).toString();
    if (key.isEmpty()) {
        return;
    }
    const QStringList parts = key.split(':');
    if (parts.size() < 2) {
        return;
    }
    const int connIdx = parts.value(1).toInt();
    if (connIdx < 0 || connIdx >= m_profiles.size() || isConnectionDisconnected(connIdx)) {
        return;
    }
    const int row = rowForConnectionIndex(m_connectionsTable, connIdx);
    if (row < 0) {
        return;
    }
    if (m_connectionsTable->currentRow() != row) {
        m_connectionsTable->setCurrentCell(row, 0);
    } else {
        refreshConnectionNodeDetails();
    }
}

void MainWindow::refreshConnectionNodeDetails() {
    auto setConnectionActionButtonsVisible = [this](bool visible) {
        Q_UNUSED(visible);
        if (m_connPropsRefreshBtn) {
            m_connPropsRefreshBtn->setVisible(false);
        }
        if (m_connPropsEditBtn) {
            m_connPropsEditBtn->setVisible(false);
        }
        if (m_connPropsDeleteBtn) {
            m_connPropsDeleteBtn->setVisible(false);
        }
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
        if (m_connPropsRefreshBtn) {
            m_connPropsRefreshBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsRefreshBtn->setEnabled(false);
        }
        if (m_connPropsEditBtn) {
            m_connPropsEditBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsEditBtn->setEnabled(false);
        }
        if (m_connPropsDeleteBtn) {
            m_connPropsDeleteBtn->setProperty("zfsmgr_can_conn_action", false);
            m_connPropsDeleteBtn->setEnabled(false);
        }
    };

    int connIdx = m_topDetailConnIdx;
    if (connIdx < 0 || connIdx >= m_profiles.size()) {
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
            m_connContentToken.clear();
        }
        if (m_poolViewTabBar) {
            m_poolViewTabBar->setVisible(false);
            m_poolViewTabBar->setCurrentIndex(0);
        }
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_connBottomStack && m_connStatusPage) {
            m_connBottomStack->setCurrentWidget(m_connStatusPage);
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
            syncConnContentPropertyColumns();
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
    if (m_connectionEntityTabs && !m_connectionEntityTabs->isVisible()) {
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_connBottomGroup) {
            m_connBottomGroup->setVisible(false);
        }
        if (m_connDetailSplit) {
            m_connDetailSplit->setSizes({1, 0});
        }
        setConnectionActionButtonsVisible(false);
        setPoolActionButtonsVisible(false);
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    QString activePoolName;
    if (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0) {
        const QString key = m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString();
        const QStringList parts = key.split(':');
        if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                activePoolName = parts.value(2).trimmed();
            }
        }
    }

    const bool hasPoolTabSelected = !activePoolName.isEmpty();
    setConnectionActionButtonsVisible(false);
    setPoolActionButtonsVisible(hasPoolTabSelected);
    if (m_poolViewTabBar) {
        m_poolViewTabBar->setVisible(false);
        m_poolViewTabBar->setCurrentIndex(0);
        m_poolViewTabBar->setTabData(0, QVariant());
        m_poolViewTabBar->setTabData(1, QVariant());
    }
    if (!hasPoolTabSelected) {
        if (!m_connContentToken.isEmpty()) {
            saveConnContentTreeState(m_connContentToken);
            m_connContentToken.clear();
        }
        if (m_connPropsStack && m_connContentPage) {
            m_connPropsStack->setCurrentWidget(m_connContentPage);
        }
        if (m_connBottomStack && m_connStatusPage) {
            m_connBottomStack->setCurrentWidget(m_connStatusPage);
        }
        if (m_connBottomGroup) {
            m_connBottomGroup->setVisible(true);
        }
        if (m_poolPropsTable) {
            setTablePopulationMode(m_poolPropsTable, true);
            m_poolPropsTable->setRowCount(0);
            setTablePopulationMode(m_poolPropsTable, false);
        }
        if (m_poolStatusText) {
            m_poolStatusText->clear();
        }
        if (m_connContentTree) {
            m_connContentTree->clear();
            syncConnContentPropertyColumns();
        }
        if (m_connContentPropsTable) {
            setTablePopulationMode(m_connContentPropsTable, true);
            m_connContentPropsTable->setRowCount(0);
            setTablePopulationMode(m_connContentPropsTable, false);
        }
            resetPoolActionButtons();
        updateConnectionActionsState();
        updateConnectionDetailTitlesForCurrentSelection();
        return;
    }

    const QString connName = (connIdx >= 0 && connIdx < m_profiles.size()) ? m_profiles[connIdx].name : QString();
    const QString poolName = activePoolName;
    const QString newConnContentToken = QStringLiteral("%1::%2").arg(connIdx).arg(poolName);
    if (!m_connContentToken.isEmpty() && m_connContentToken != newConnContentToken) {
        saveConnContentTreeState(m_connContentToken);
    }
    if (m_connPropsStack && m_connContentPage) {
        m_connPropsStack->setCurrentWidget(m_connContentPage);
    }
    if (m_connBottomGroup) {
        m_connBottomGroup->setVisible(false);
    }
    resetPoolActionButtons();
    refreshSelectedPoolDetails(false, true);
    if (connIdx >= 0 && connIdx < m_profiles.size() && m_connContentTree) {
        m_connContentToken = newConnContentToken;
        populateDatasetTree(m_connContentTree, connIdx, poolName, QStringLiteral("conncontent"), true);
        syncConnContentPoolColumns();
        refreshDatasetProperties(QStringLiteral("conncontent"));
    }
    updateConnectionActionsState();
    updateConnectionDetailTitlesForCurrentSelection();
}

void MainWindow::updateConnectionDetailTitlesForCurrentSelection() {
    int connIdx = m_topDetailConnIdx;
    QString activePoolName;
    bool poolMode = false;
    if (m_connectionEntityTabs && m_connectionEntityTabs->currentIndex() >= 0) {
        const QString key = m_connectionEntityTabs->tabData(m_connectionEntityTabs->currentIndex()).toString();
        const QStringList parts = key.split(':');
        if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
            bool ok = false;
            const int tabConnIdx = parts.value(1).toInt(&ok);
            if (ok && tabConnIdx == connIdx) {
                poolMode = true;
                activePoolName = parts.value(2).trimmed();
            }
        }
    }
    if (m_poolViewTabBar) {
        QString tab0 = trk(QStringLiteral("t_pool_props001"),
                           QStringLiteral("Propiedades"),
                           QStringLiteral("Properties"),
                           QStringLiteral("属性"));
        QString tab1 = trk(QStringLiteral("t_content_node_001"),
                           QStringLiteral("Contenido"),
                           QStringLiteral("Content"),
                           QStringLiteral("内容"));
        if (poolMode) {
            const QString poolName = activePoolName;
            const QString shown = poolName.isEmpty() ? QStringLiteral("-") : poolName;
            tab0 = QStringLiteral("Propiedades %1").arg(shown);
            tab1 = QStringLiteral("Contenido %1").arg(shown);
        }
        m_poolViewTabBar->setTabText(0, tab0);
        m_poolViewTabBar->setTabText(1, tab1);
    }
}

int MainWindow::selectedConnectionIndexForPoolManagement() const {
    if (m_connectionsTable) {
        const int idx = selectedConnectionRow(m_connectionsTable);
        if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
            return idx;
        }
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
    if (m_bottomConnContentTree && idx == m_bottomDetailConnIdx) {
        QSet<QString> expandedKeys;
        QString selectedKey;
        std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* n) {
            if (!n) {
                return;
            }
            const QString k = connTreeNodeKey(n);
            if (!k.isEmpty() && n->isExpanded()) {
                expandedKeys.insert(k);
            }
            for (int i = 0; i < n->childCount(); ++i) {
                collect(n->child(i));
            }
        };
        for (int i = 0; i < m_bottomConnContentTree->topLevelItemCount(); ++i) {
            collect(m_bottomConnContentTree->topLevelItem(i));
        }
        if (QTreeWidgetItem* cur = m_bottomConnContentTree->currentItem()) {
            QTreeWidgetItem* owner = cur;
            while (owner && connTreeNodeKey(owner).isEmpty()) {
                owner = owner->parent();
            }
            if (owner) {
                selectedKey = connTreeNodeKey(owner);
            }
        }
        m_pendingBottomExpandedKeysByConn[idx] = expandedKeys;
        m_pendingBottomSelectedKeyByConn[idx] = selectedKey;
    }
    if (m_connectionEntityTabs) {
        const int t = m_connectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_connectionEntityTabs->count()) {
            const QString key = m_connectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshTopTabDataByConn[idx] = key;
            }
        }
    }
    if (m_bottomConnectionEntityTabs) {
        const int t = m_bottomConnectionEntityTabs->currentIndex();
        if (t >= 0 && t < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(t).toString();
            if (key.startsWith(QStringLiteral("pool:%1:").arg(idx))) {
                m_pendingRefreshBottomTabDataByConn[idx] = key;
            }
        }
    }
    if (!m_connContentToken.isEmpty() && m_connContentTree) {
        saveConnContentTreeState(m_connContentToken);
    }
    if (m_bottomConnContentTree && m_bottomConnectionEntityTabs
        && m_bottomConnectionEntityTabs->isVisible()) {
        const int bIdx = m_bottomConnectionEntityTabs->currentIndex();
        if (bIdx >= 0 && bIdx < m_bottomConnectionEntityTabs->count()) {
            const QString key = m_bottomConnectionEntityTabs->tabData(bIdx).toString();
            const QStringList parts = key.split(':');
            if (parts.size() >= 3 && parts.first() == QStringLiteral("pool")) {
                const QString bottomToken = QStringLiteral("%1::%2").arg(parts.value(1), parts.value(2).trimmed());
                const QString prevToken = m_connContentToken;
                QTreeWidget* prevTree = m_connContentTree;
                m_connContentTree = m_bottomConnContentTree;
                m_connContentToken = bottomToken;
                saveConnContentTreeState(bottomToken);
                m_connContentTree = prevTree;
                m_connContentToken = prevToken;
            }
        }
    }
    // Al refrescar una conexión, invalidar toda la caché asociada a todos sus pools.
    {
        const QString connPrefix = QStringLiteral("%1::").arg(m_profiles[idx].name.trimmed().toLower());
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
    rebuildConnectionsTable();
    if (m_connectionEntityTabs) {
        const QString wantedTop = m_pendingRefreshTopTabDataByConn.value(idx);
        if (!wantedTop.isEmpty()) {
            for (int t = 0; t < m_connectionEntityTabs->count(); ++t) {
                if (m_connectionEntityTabs->tabData(t).toString() == wantedTop) {
                    m_connectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    if (m_bottomConnectionEntityTabs && m_bottomConnectionEntityTabs->isVisible()) {
        const QString wantedBottom = m_pendingRefreshBottomTabDataByConn.value(idx);
        if (!wantedBottom.isEmpty()) {
            for (int t = 0; t < m_bottomConnectionEntityTabs->count(); ++t) {
                if (m_bottomConnectionEntityTabs->tabData(t).toString() == wantedBottom) {
                    m_bottomConnectionEntityTabs->setCurrentIndex(t);
                    break;
                }
            }
        }
    }
    m_pendingRefreshTopTabDataByConn.remove(idx);
    m_pendingRefreshBottomTabDataByConn.remove(idx);
    populateAllPoolsTables();
}
void MainWindow::loadConnections() {
    QString prevSelectedConnId;
    if (m_connectionsTable) {
        const int prevIdx = selectedConnectionRow(m_connectionsTable);
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

    rebuildConnectionsTable();
    appLog(QStringLiteral("NORMAL"), QStringLiteral("Loaded %1 connections from %2")
                                   .arg(m_profiles.size())
                                   .arg(m_store.iniPath()));
    for (const QString& warning : loaded.warnings) {
        appLog(QStringLiteral("WARN"), warning);
    }

    if (m_connectionsTable && m_connectionsTable->rowCount() > 0) {
        int targetRow = -1;
        if (!prevSelectedConnId.trimmed().isEmpty()) {
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                const int connIdx = connectionIndexForRow(m_connectionsTable, r);
                if (connIdx >= 0
                    && connIdx < m_profiles.size()
                    && m_profiles[connIdx].id.trimmed().compare(prevSelectedConnId, Qt::CaseInsensitive) == 0) {
                    targetRow = r;
                    break;
                }
            }
        }
        if (targetRow < 0 && m_initialRefreshCompleted) {
            for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
                const int connIdx = connectionIndexForRow(m_connectionsTable, r);
                if (connIdx >= 0 && connIdx < m_profiles.size() && !isConnectionDisconnected(connIdx)) {
                    targetRow = r;
                    break;
                }
            }
        }
        if (targetRow >= 0) {
            m_connectionsTable->setCurrentCell(targetRow, 0);
        }
    }

    syncConnectionLogTabs();
    updatePoolManagementBoxTitle();
}

void MainWindow::rebuildConnectionsTable() {
    beginUiBusy();
    m_syncConnSelectorChecks = true;
    QString prevSelectedKey;
    {
        const int prevIdx = selectedConnectionRow(m_connectionsTable);
        if (prevIdx >= 0 && prevIdx < m_profiles.size()) {
            prevSelectedKey = m_profiles[prevIdx].id.trimmed().toLower();
            if (prevSelectedKey.isEmpty()) {
                prevSelectedKey = m_profiles[prevIdx].name.trimmed().toLower();
            }
        }
    }
    m_connectionsTable->clear();
    m_connectionsTable->setColumnCount(3);
    m_connectionsTable->setHorizontalHeaderLabels({
        trk(QStringLiteral("t_connections_001"),
            QStringLiteral("Conexión"),
            QStringLiteral("Connection"),
            QStringLiteral("连接")),
        QStringLiteral("O"),
        QStringLiteral("D")
    });
    m_connectionsTable->setRowCount(0);
    m_connectionsTable->setFont(QApplication::font());
    if (m_connectionsTable->horizontalHeader()) {
        m_connectionsTable->horizontalHeader()->setFont(QApplication::font());
    }
    auto zfsVersionTooOld = [](const QString& rawVersion) -> bool {
        const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));
        const QRegularExpressionMatch m = rx.match(rawVersion.trimmed());
        if (!m.hasMatch()) {
            return false;
        }
        const int major = m.captured(1).toInt();
        const int minor = m.captured(2).toInt();
        const int patch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
        if (major != 2) return false;
        if (minor < 3) return true;
        if (minor > 3) return false;
        return patch < 3;
    };
    auto connectionRowColorReason = [&](const ConnectionProfile& p, const ConnectionRuntimeState& st, bool disconnected) {
        Q_UNUSED(p);
        if (disconnected) {
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
        if (zfsVersionTooOld(st.zfsVersion)) {
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
    };
    auto buildConnectionStateTooltip = [this, &connectionRowColorReason](const ConnectionProfile& p, const ConnectionRuntimeState& st, bool disconnected) {
        QStringList lines;
        lines << QStringLiteral("Host: %1").arg(p.host);
        lines << QStringLiteral("Port: %1").arg(p.port);
        lines << QStringLiteral("Estado: %1").arg(st.status.trimmed().isEmpty() ? QStringLiteral("-") : st.status.trimmed());
        const QString colorReason = connectionRowColorReason(p, st, disconnected);
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
        QStringList detected = st.detectedUnixCommands;
        QStringList missing = st.missingUnixCommands;
        lines << QStringLiteral("Comandos detectados: %1")
                     .arg(detected.isEmpty() ? QStringLiteral("(ninguno)") : detected.join(QStringLiteral(", ")));
        lines << QStringLiteral("Comandos no detectados: %1")
                     .arg(missing.isEmpty() ? QStringLiteral("(ninguno)") : missing.join(QStringLiteral(", ")));
        if (st.commandsLayer.trimmed().compare(QStringLiteral("Powershell"), Qt::CaseInsensitive) == 0
            && !st.powershellFallbackCommands.isEmpty()) {
            lines << QStringLiteral("Comandos PowerShell usados: %1")
                         .arg(st.powershellFallbackCommands.join(QStringLiteral(", ")));
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
        return QStringLiteral("<pre style=\"font-family:monospace; white-space:pre;\">%1</pre>").arg(plain);
    };
    QStringList redirectedToLocalNames;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (i >= m_states.size()) {
            continue;
        }
        if (isLocalConnection(i)) {
            continue;
        }
        if (isConnectionRedirectedToLocal(i)) {
            redirectedToLocalNames << m_profiles[i].name;
        }
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        const auto& p = m_profiles[i];
        const auto& s = m_states[i];

        const QString line1 = QStringLiteral("%1").arg(p.name);
        QString zfsTxt = s.zfsVersion.trimmed();
        if (zfsTxt.isEmpty()) {
            zfsTxt = QStringLiteral("?");
        }
        QString statusTag = QStringLiteral("[Ko]");
        QColor rowBg = m_connectionsTable->palette().base().color();
        const bool disconnected = isConnectionDisconnected(i);
        const QString st = s.status.trimmed().toUpper();
        const bool localConn = isLocalConnection(p);
        const bool redirectedLocal = isConnectionRedirectedToLocal(i);
        if (redirectedLocal) {
            continue;
        }
        if (localConn && !s.machineUuid.trimmed().isEmpty()) {
            m_localMachineUuid = s.machineUuid.trimmed();
        }
        if (disconnected) {
            statusTag = QStringLiteral("[Off]");
            rowBg = QColor(QStringLiteral("#eef1f4"));
        } else if (st == QStringLiteral("OK")) {
            statusTag = QStringLiteral("[Ok]");
            rowBg = s.missingUnixCommands.isEmpty() ? QColor(QStringLiteral("#e4f4e4"))
                                                    : QColor(QStringLiteral("#fff1d9"));
            if (zfsVersionTooOld(s.zfsVersion)) {
                rowBg = QColor(QStringLiteral("#f9dfdf"));
            }
        } else if (!st.isEmpty()) {
            statusTag = QStringLiteral("[Ko]");
            rowBg = QColor(QStringLiteral("#f9dfdf"));
        }
        QString connLabel = line1;
        if (localConn) {
            connLabel = redirectedToLocalNames.isEmpty() ? QStringLiteral("Local")
                                                         : redirectedToLocalNames.first();
        }
        QString line = QStringLiteral("%1 %2").arg(statusTag, connLabel);
        if (localConn) {
            line += QStringLiteral(" [Local]");
        }

        const int row = m_connectionsTable->rowCount();
        m_connectionsTable->insertRow(row);
        const QString tooltip = buildConnectionStateTooltip(p, s, disconnected);
        auto* it = new QTableWidgetItem(line);
        it->setData(Qt::UserRole, i);
        it->setForeground(m_connectionsTable->palette().text());
        it->setBackground(QBrush(rowBg));
        it->setFont(m_connectionsTable->font());
        if (disconnected) {
            QFont f = it->font();
            f.setItalic(true);
            it->setFont(f);
        }
        it->setToolTip(tooltip);
        m_connectionsTable->setItem(row, 0, it);

        auto makeCheckItem = [&](bool checked) {
            auto* checkItem = new QTableWidgetItem();
            checkItem->setData(Qt::UserRole, i);
            checkItem->setFlags((checkItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled)
                                & ~Qt::ItemIsEditable);
            if (disconnected) {
                checkItem->setFlags(checkItem->flags() & ~Qt::ItemIsEnabled);
            }
            checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            checkItem->setBackground(QBrush(rowBg));
            checkItem->setForeground(m_connectionsTable->palette().text());
            checkItem->setToolTip(tooltip);
            return checkItem;
        };
        const QString mode = connectionDisplayModeForIndex(i);
        m_connectionsTable->setItem(row, 1, makeCheckItem(mode == QStringLiteral("source") || mode == QStringLiteral("both")));
        m_connectionsTable->setItem(row, 2, makeCheckItem(mode == QStringLiteral("target") || mode == QStringLiteral("both")));

    }
    auto ensureValidConnIdx = [this](int& idx) {
        if (idx < 0) {
            return false;
        }
        if (isConnectionDisconnected(idx)) {
            return false;
        }
        return rowForConnectionIndex(m_connectionsTable, idx) >= 0;
    };
    const int rowCount = m_connectionsTable->rowCount();
    auto firstConnectedIndex = [this]() -> int {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                return idx;
            }
        }
        return -1;
    };
    auto secondConnectedIndex = [this](int firstIdx) -> int {
        bool seenFirst = false;
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
                continue;
            }
            if (!seenFirst && idx == firstIdx) {
                seenFirst = true;
                continue;
            }
            return idx;
        }
        return -1;
    };
    auto connIdxFromPersistedKey = [this](const QString& wantedKey) -> int {
        const QString wanted = wantedKey.trimmed().toLower();
        if (wanted.isEmpty()) {
            return -1;
        }
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx < 0 || idx >= m_profiles.size() || isConnectionDisconnected(idx)) {
                continue;
            }
            const QString key = connPersistKeyFromProfiles(m_profiles, idx);
            if (!key.isEmpty() && key == wanted) {
                return idx;
            }
        }
        return -1;
    };
    if (!m_connSelectorDefaultsInitialized) {
        if (m_topDetailConnIdx < 0) {
            m_topDetailConnIdx = connIdxFromPersistedKey(m_persistedTopDetailConnectionKey);
        }
        if (m_bottomDetailConnIdx < 0) {
            m_bottomDetailConnIdx = connIdxFromPersistedKey(m_persistedBottomDetailConnectionKey);
        }
        if (!ensureValidConnIdx(m_topDetailConnIdx)) {
            m_topDetailConnIdx = (rowCount > 0) ? firstConnectedIndex() : -1;
        }
        if (!ensureValidConnIdx(m_bottomDetailConnIdx)) {
            if (rowCount > 1) {
                // Inicialmente: primera conexión conectada como Origen y segunda conectada como Destino.
                const int second = secondConnectedIndex(m_topDetailConnIdx);
                m_bottomDetailConnIdx = (second >= 0) ? second : m_topDetailConnIdx;
            } else {
                m_bottomDetailConnIdx = m_topDetailConnIdx;
            }
        }
        m_connSelectorDefaultsInitialized = true;
    } else {
        if (!ensureValidConnIdx(m_topDetailConnIdx)) {
            m_topDetailConnIdx = -1;
        }
        if (!ensureValidConnIdx(m_bottomDetailConnIdx)) {
            m_bottomDetailConnIdx = -1;
        }
    }
    syncConnectionDisplaySelectors();
    int targetRow = rowForConnectionIndex(m_connectionsTable, m_topDetailConnIdx);
    const QString preferredKey = !m_userSelectedConnectionKey.trimmed().isEmpty()
                                     ? m_userSelectedConnectionKey.trimmed().toLower()
                                     : prevSelectedKey;
    if (!preferredKey.isEmpty()) {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            QTableWidgetItem* it = m_connectionsTable->item(r, 0);
            if (!it) {
                continue;
            }
            bool ok = false;
            const int connIdx = it->data(Qt::UserRole).toInt(&ok);
            if (!ok || connIdx < 0 || connIdx >= m_profiles.size()) {
                continue;
            }
            QString key = m_profiles[connIdx].id.trimmed().toLower();
            if (key.isEmpty()) {
                key = m_profiles[connIdx].name.trimmed().toLower();
            }
            if (key == preferredKey) {
                targetRow = r;
                break;
            }
        }
    }
    if (targetRow < 0 && m_connectionsTable->rowCount() > 0) {
        for (int r = 0; r < m_connectionsTable->rowCount(); ++r) {
            const int idx = connectionIndexForRow(m_connectionsTable, r);
            if (idx >= 0 && idx < m_profiles.size() && !isConnectionDisconnected(idx)) {
                targetRow = r;
                break;
            }
        }
    }
    m_connectionsTable->resizeColumnToContents(0);
    m_connectionsTable->resizeColumnToContents(1);
    m_connectionsTable->resizeColumnToContents(2);
    if (targetRow >= 0) {
        m_connectionsTable->setCurrentCell(targetRow, 0);
    }
    m_syncConnSelectorChecks = false;

    rebuildConnectionEntityTabs();
    syncConnectionLogTabs();
    endUiBusy();
    refreshConnectionNodeDetails();
    updateSecondaryConnectionDetail();
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
            if (m_connectionsTable) {
                const int row = rowForConnectionIndex(m_connectionsTable, i);
                if (row >= 0) {
                    m_connectionsTable->setCurrentCell(row, 0);
                }
            }
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
    const int idx = selectedConnectionRow(m_connectionsTable);
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
    const int idx = selectedConnectionRow(m_connectionsTable);
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
        QString err;
        int rc = -1;
        if (!runSsh(p, detectCmd, 30000, out, err, rc, {}, {}, {}, psMode) || rc != 0) {
            if (detailOut) {
                *detailOut = (err.isEmpty() ? QStringLiteral("ssh exit %1").arg(rc) : err).simplified().left(220);
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

    QString installOut;
    QString installErr;
    int installRc = -1;
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
    const bool ok = runSsh(p, installCmd, 900000, installOut, installErr, installRc, {}, {}, {}, psMode) && installRc == 0;
    endUiBusy();

    if (!ok) {
        const QString detail = (installErr.isEmpty() ? QStringLiteral("ssh exit %1").arg(installRc) : installErr).simplified().left(400);
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
    if (compareAppVersions(gsaScriptVersion(), st.gsaVersion) > 0) {
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
    auto unixConfigDirForProfile = [this](const ConnectionProfile& cp, bool isMac) {
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
    for (auto it = m_datasetPropsCache.cbegin(); it != m_datasetPropsCache.cend(); ++it) {
        if (!it.value().loaded || it.value().datasetType.trimmed().compare(QStringLiteral("filesystem"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QStringList parts = it.key().split(QStringLiteral("::"));
        if (parts.size() < 3) {
            continue;
        }
        bool okConn = false;
        const int dsConnIdx = parts.at(0).toInt(&okConn);
        const QString datasetName = parts.mid(2).join(QStringLiteral("::")).trimmed();
        if (!okConn || dsConnIdx != idx || datasetName.isEmpty()) {
            continue;
        }
        QMap<QString, QString> props;
        for (const DatasetPropCacheRow& row : it.value().rows) {
            props.insert(row.prop, row.value);
        }
        const QString token = QStringLiteral("%1::%2").arg(idx).arg(parts.at(1).trimmed());
        const QString liveKey = QStringLiteral("%1|%2").arg(token, datasetName);
        const auto liveIt = m_connContentPropValuesByObject.constFind(liveKey);
        if (liveIt != m_connContentPropValuesByObject.cend()) {
            for (auto vit = liveIt->cbegin(); vit != liveIt->cend(); ++vit) {
                props[vit.key()] = vit.value();
            }
        }
        const QString enabled = props.value(QStringLiteral("org.fc16.gsa:activado")).trimmed().toLower();
        if (!(enabled == QStringLiteral("on") || enabled == QStringLiteral("yes") || enabled == QStringLiteral("true") || enabled == QStringLiteral("1"))) {
            continue;
        }
        const QString levelOn = props.value(QStringLiteral("org.fc16.gsa:nivelar")).trimmed().toLower();
        if (!(levelOn == QStringLiteral("on") || levelOn == QStringLiteral("yes")
              || levelOn == QStringLiteral("true") || levelOn == QStringLiteral("1"))) {
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
        const bool isMac = p.osType.trimmed().toLower().contains(QStringLiteral("darwin"))
                           || (idx < m_states.size() && m_states[idx].osLine.toLower().contains(QStringLiteral("darwin")));
        const QString runtimeConfigDir = unixConfigDirForProfile(p, isMac);
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
        } else {
            const QString servicePayload = QString::fromUtf8(R"([Unit]
Description=ZFSMgr automatic snapshot manager

[Service]
Type=oneshot
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
                "mkdir -p /usr/local/libexec %7; "
                "cat > %1 <<'EOF_GSA'\n%2\nEOF_GSA\n"
                "cat > %8 <<'EOF_GSA_CONF'\n%9\nEOF_GSA_CONF\n"
                "cat > %10 <<'EOF_GSA_CONN'\n%11\nEOF_GSA_CONN\n"
                "cat > %12 <<'EOF_GSA_KNOWN'\n%13\nEOF_GSA_KNOWN\n"
                "chmod 700 %1; "
                "chmod 600 %8 %10 %12; "
                "chmod 700 %7; "
                "cat > %3 <<'EOF_GSA_SERVICE'\n%4\nEOF_GSA_SERVICE\n"
                "cat > %5 <<'EOF_GSA_TIMER'\n%6\nEOF_GSA_TIMER\n"
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
                                 knownHostsPayload);
        }
        remoteCmd = withSudo(p, remoteCmd);
    }

    beginUiBusy();
    updateStatus(actionLabel + QStringLiteral("..."));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, 240000, out, err, rc, {}, {}, {}, winMode) && rc == 0;
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
                    .arg(p.name, (err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err).simplified().left(500)));
        } else {
            appLog(QStringLiteral("WARN"),
                   QStringLiteral("Actualización automática de GSA fallida en \"%1\": %2")
                       .arg(p.name,
                            (err.isEmpty() ? QStringLiteral("exit %1").arg(rc) : err).simplified().left(500)));
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
        const bool isMac = p.osType.trimmed().toLower().contains(QStringLiteral("darwin"))
                           || (idx < m_states.size() && m_states[idx].osLine.toLower().contains(QStringLiteral("darwin")));
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
    QString out;
    QString err;
    int rc = -1;
    const bool ok = runSsh(p, remoteCmd, 120000, out, err, rc, {}, {}, {}, winMode) && rc == 0;
    endUiBusy();
    if (!ok) {
        QMessageBox::warning(
            this,
            QStringLiteral("ZFSMgr"),
            trk(QStringLiteral("t_gsa_uninstall_fail_001"),
                QStringLiteral("No se pudo desinstalar el GSA de \"%1\".\n\n%2"),
                QStringLiteral("Could not uninstall GSA from \"%1\".\n\n%2"),
                QStringLiteral("无法从 \"%1\" 卸载 GSA。\n\n%2"))
                .arg(p.name, mwhelpers::oneLine(err.isEmpty() ? out : err)));
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
    const int idx = selectedConnectionRow(m_connectionsTable);
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
    m_pendingRefreshTopTabDataByConn.clear();
    m_pendingRefreshBottomTabDataByConn.clear();
    updateBusyCursor();
    loadConnections();
}
