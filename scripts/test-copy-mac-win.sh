#!/usr/bin/env bash
set -euo pipefail

# Automated copy test: macOS snapshot -> Windows dataset
# - Test A: zfs send|recv
# - Test B: TAR fallback into temporary dataset

SRC_HOST="${SRC_HOST:-mbp.local}"
SRC_USER="${SRC_USER:-linarese}"
SRC_KEY="${SRC_KEY:-$HOME/.ssh/id_ed25519}"
SRC_DATASET="${SRC_DATASET:-stech/user/bin}"
SRC_SNAP="${SRC_SNAP:---3}"
MAC_SUDO_PASS="${MAC_SUDO_PASS:-rpq231}"

DST_HOST="${DST_HOST:-surface.local}"
DST_USER="${DST_USER:-eladi}"
DST_KEY="${DST_KEY:-$HOME/.ssh/id_ed25519}"
DST_DATASET="${DST_DATASET:-t7/bin}"
DST_TMP_DATASET="${DST_TMP_DATASET:-t7/zfsmgr_auto_tar}"
DST_TMP_DRIVE="${DST_TMP_DRIVE:-X}"

SSH_OPTS=(-o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

ssh_src() {
  ssh "${SSH_OPTS[@]}" -i "$SRC_KEY" "${SRC_USER}@${SRC_HOST}" "$@"
}

ssh_dst() {
  ssh "${SSH_OPTS[@]}" -i "$DST_KEY" "${DST_USER}@${DST_HOST}" "$@"
}

src_sudo_bash() {
  local script="$1"
  local b64
  b64="$(printf '%s' "$script" | base64 -w0)"
  ssh_src "printf '%s\n' '${MAC_SUDO_PASS}' | sudo -S -p '' bash -lc \"\$(printf '%s' '${b64}' | base64 -d)\""
}

dst_bash() {
  local script="$1"
  local b64
  b64="$(printf '%s' "$script" | base64 -w0)"
  ssh_dst "powershell -NoProfile -NonInteractive -Command \"\$s=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('${b64}')); & 'C:\\msys64\\usr\\bin\\bash.exe' -lc \$s\""
}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

log "Test settings: ${SRC_USER}@${SRC_HOST} ${SRC_DATASET}@${SRC_SNAP} -> ${DST_USER}@${DST_HOST} ${DST_DATASET}"

# 0) Cleanup partial receive state on destination
log "Cleanup destination partial receive state"
dst_bash "zfs recv -A ${DST_DATASET} 2>/dev/null || true" || true

# 1) Test A: zfs send|recv
log "TEST A: zfs send|recv"
set +e
src_sudo_bash "zfs send -wLecR ${SRC_DATASET}@${SRC_SNAP}" \
  | dst_bash "zfs recv -Fus ${DST_DATASET}"
RC_A=$?
set -e
log "TEST A rc=${RC_A}"

# 2) Test B: TAR fallback to temporary destination dataset
log "TEST B: TAR fallback -> ${DST_TMP_DATASET}"

# Prepare destination temp dataset with temporary driveletter mount
dst_bash "set -e; DS=${DST_TMP_DATASET}; DL=${DST_TMP_DRIVE}; zfs destroy -r \"\$DS\" 2>/dev/null || true; zfs create -u \"\$DS\"; zfs set driveletter=\"\$DL\" \"\$DS\"; zfs mount \"\$DS\" >/dev/null 2>&1 || true"

read -r -d '' TESTB_EXTRACT_SCRIPT <<'EOS' || true
set -e
DS="__DS__"
TMP="/tmp/zfsmgr-dst-mp-$$.txt"
TMP2="/tmp/zfsmgr-dst-mp2-$$.txt"
: > "$TMP"
zfs mount 2>/dev/null | grep -E "^$DS[[:space:]]" | head -n1 | sed 's/^[^[:space:]]*[[:space:]]*//' > "$TMP" || true
MP=""
if [ -s "$TMP" ]; then
  IFS= read -r MP < "$TMP" || true
fi
printf '%s' "$MP" | sed 's/\r//g; s/^[[:space:]]*//; s/[[:space:]]*$//' > "$TMP2"
MP=""
IFS= read -r MP < "$TMP2" || true
rm -f "$TMP" "$TMP2" || true
[ -n "$MP" ] || { echo no-dst-mp; exit 44; }
tar --acls --xattrs -xpf - -C "$MP"
EOS
TESTB_EXTRACT_SCRIPT="${TESTB_EXTRACT_SCRIPT//__DS__/${DST_TMP_DATASET}}"

set +e
src_sudo_bash "set -e; DS=${SRC_DATASET}; MP=\"\$(zfs mount 2>/dev/null | grep -E \"^\$DS[[:space:]]\" | head -n1 | cut -d\" \" -f2-)\"; [ -n \"\$MP\" ] || { echo no-source-mp; exit 41; }; SRC=\"\$MP/.zfs/snapshot/${SRC_SNAP}\"; [ -d \"\$SRC\" ] || { echo no-snapshot-path:\$SRC; exit 42; }; tar --acls --xattrs -cpf - -C \"\$SRC\" ." \
  | dst_bash "$TESTB_EXTRACT_SCRIPT"
RC_B=$?
set -e

# Validate destination temp dataset has data
USED_B=$(dst_bash "zfs get -H -o value used ${DST_TMP_DATASET} 2>/dev/null || echo 0" | tr -d '\r' | tail -n1)

log "TEST B rc=${RC_B}, used=${USED_B}"

echo
if [[ "$RC_A" -eq 0 ]]; then
  echo "RESULT: PASS zfs send|recv"
  exit 0
fi
if [[ "$RC_B" -eq 0 && "$USED_B" != "0" && "$USED_B" != "0B" ]]; then
  echo "RESULT: PASS TAR fallback"
  exit 0
fi

echo "RESULT: FAIL"
echo "  zfs send|recv rc=$RC_A"
echo "  tar fallback  rc=$RC_B used=$USED_B"
exit 1
