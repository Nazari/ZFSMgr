#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${HOME}/Downloads/zfsmgr-builds"

LINUX_REMOTE="${LINUX_REMOTE:-linarese@fc16.local}"
WINDOWS_REMOTE="${WINDOWS_REMOTE:-eladi@surface.local}"

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
  echo "Error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "No se encontró el comando requerido: $1"
}

find_local_artifact() {
  local pattern="$1"
  local type="$2"
  find "${PROJECT_ROOT}" -maxdepth 2 -type "${type}" -name "${pattern}" -print0 \
    | xargs -0 -r ls -td 2>/dev/null \
    | head -n1
}

copy_local_artifact() {
  local source="$1"
  local dest
  dest="${OUTPUT_DIR}/$(basename "${source}")"
  if [[ -e "${dest}" ]]; then
    rm -rf "${dest}"
  fi
  if [[ -d "${source}" ]]; then
    cp -R "${source}" "${dest}"
  else
    cp -f "${source}" "${dest}"
  fi
}

zip_macos_bundle() {
  local app_path="$1"
  local app_name zip_name
  app_name="$(basename "${app_path}")"
  zip_name="${app_name}.zip"
  (
    cd "${OUTPUT_DIR}"
    rm -f "${zip_name}"
    ditto -c -k --sequesterRsrc --keepParent "${app_name}" "${zip_name}"
    rm -rf "${app_name}"
  )
}

ssh_linux() {
  ssh -o BatchMode=yes "${LINUX_REMOTE}" "$@"
}

ssh_windows() {
  ssh -o BatchMode=yes "${WINDOWS_REMOTE}" "$@"
}

copy_linux_remote_artifact() {
  local remote_path="$1"
  local dest="${OUTPUT_DIR}/$(basename "${remote_path}")"
  local quoted_remote
  quoted_remote="$(printf '%q' "${remote_path}")"

  log "Ruta remota Linux resuelta: ${remote_path}"
  if scp -p "${LINUX_REMOTE}:${remote_path}" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp directo falló; reintentando con ruta remota entrecomillada"
  if scp -p "${LINUX_REMOTE}:\"${remote_path}\"" "${OUTPUT_DIR}/"; then
    return 0
  fi
  log "scp volvió a fallar; usando fallback por ssh+cat"
  rm -f "${dest}"
  ssh_linux "cat ${quoted_remote}" > "${dest}"
  chmod +x "${dest}" || true
}

run_linux_b64_script() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_linux "bash -lc \"\$(printf '%s' '${encoded}' | base64 -d)\""
}

run_windows_b64_ps() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "${script}" | base64 | tr -d '\n')"
  ssh_windows "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"\$s=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('${encoded}')); Invoke-Expression \$s\""
}

extract_marked_artifact() {
  local marker="$1"
  awk -v marker="${marker}" 'index($0, marker) == 1 { value = substr($0, length(marker) + 1) } END { if (value != "") print value }' \
    | tr -d '\r'
}

windows_to_scp_path() {
  local win_path="$1"
  local drive rest
  if [[ "${win_path}" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
    drive="${BASH_REMATCH[1]}"
    rest="${BASH_REMATCH[2]}"
    rest="${rest//\\//}"
    printf '/%s:/%s\n' "${drive}" "${rest}"
    return 0
  fi
  fail "Ruta Windows no válida para scp: ${win_path}"
}

mkdir -p "${OUTPUT_DIR}"

require_cmd ssh
require_cmd scp
require_cmd git
require_cmd base64
require_cmd ditto

log "Directorio destino local: ${OUTPUT_DIR}"

log "Compilando macOS en local"
"${SCRIPT_DIR}/build-macos.sh" --bundle
MAC_ARTIFACT="$(find_local_artifact 'ZFSMgr-*.app' d)"
[[ -n "${MAC_ARTIFACT}" ]] || fail "No se encontró el .app generado en build-macos"
copy_local_artifact "${MAC_ARTIFACT}"
zip_macos_bundle "${OUTPUT_DIR}/$(basename "${MAC_ARTIFACT}")"
log "Artefacto macOS copiado: $(basename "${MAC_ARTIFACT}").zip"

log "Compilando Linux remoto en ${LINUX_REMOTE}"
read -r -d '' LINUX_BUILD_SCRIPT <<'EOF' || true
set -euo pipefail
repo=""
for candidate in \
  "$HOME/work/ZFSMgr" \
  "$HOME/Work/ZFSMgr" \
  "/home/linarese/work/ZFSMgr" \
  "/home/linarese/Work/ZFSMgr"
do
  if [[ -d "${candidate}/.git" ]]; then
    repo="${candidate}"
    break
  fi
done
[[ -n "${repo}" ]] || { echo "No se encontró el repo ZFSMgr en Linux." >&2; exit 1; }
cd "${repo}"
git pull --ff-only
app_version="$(sed -nE 's/^[[:space:]]*set\(ZFSMGR_APP_VERSION_STRING "([^"]+)"\).*/\1/p' "${repo}/resources/CMakeLists.txt" | head -n1)"
if [[ -z "${app_version}" ]]; then
  app_version="$(sed -nE 's/^[[:space:]]*project\(ZFSMgrQt VERSION ([^ )]+).*/\1/p' "${repo}/resources/CMakeLists.txt" | head -n1)"
fi
[[ -n "${app_version}" ]] || { echo "No se pudo resolver la versión de la AppImage." >&2; exit 1; }
arch="$(uname -m)"
find "${repo}/build-linux" -maxdepth 1 -type f -name 'ZFSMgr-*.AppImage' -delete 2>/dev/null || true
./scripts/build-linux.sh --appimage
artifact="${repo}/build-linux/ZFSMgr-${app_version}-${arch}.AppImage"
[[ -n "${artifact}" ]] || { echo "No se encontró el .AppImage generado." >&2; exit 1; }
[[ -f "${artifact}" ]] || { echo "No se encontró el .AppImage generado: ${artifact}" >&2; exit 1; }
printf 'ARTIFACT_LINUX=%s\n' "${artifact}"
EOF
LINUX_ARTIFACT="$(run_linux_b64_script "${LINUX_BUILD_SCRIPT}" | extract_marked_artifact 'ARTIFACT_LINUX=')"
[[ -n "${LINUX_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Linux"
copy_linux_remote_artifact "${LINUX_ARTIFACT}"
log "Artefacto Linux copiado: $(basename "${LINUX_ARTIFACT}")"

log "Compilando Windows remoto en ${WINDOWS_REMOTE}"
read -r -d '' WINDOWS_BUILD_SCRIPT <<'EOF' || true
$ErrorActionPreference = "Stop"
$candidates = @(
  "$HOME\Work\ZFSMgr",
  "$HOME\work\ZFSMgr",
  "C:\Users\eladi\Work\ZFSMgr",
  "C:\Users\eladi\work\ZFSMgr"
)
$repo = $null
foreach ($candidate in $candidates) {
  if (Test-Path (Join-Path $candidate ".git")) {
    $repo = $candidate
    break
  }
}
if (-not $repo) {
  throw "No se encontró el repo ZFSMgr en Windows."
}
Set-Location $repo
git pull --ff-only
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "scripts\build-windows.ps1") --inno
$artifact = Get-ChildItem -Path (Join-Path $repo "build-windows\installer") -Filter "*.exe" -File -ErrorAction Stop |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $artifact) {
  throw "No se encontró el .exe generado."
}
Write-Output "ARTIFACT_WINDOWS=$artifact"
EOF
WINDOWS_ARTIFACT="$(run_windows_b64_ps "${WINDOWS_BUILD_SCRIPT}" | extract_marked_artifact 'ARTIFACT_WINDOWS=')"
[[ -n "${WINDOWS_ARTIFACT}" ]] || fail "No se pudo resolver el artefacto Windows"
WINDOWS_SCP_PATH="$(windows_to_scp_path "${WINDOWS_ARTIFACT}")"
scp -p "${WINDOWS_REMOTE}:${WINDOWS_SCP_PATH}" "${OUTPUT_DIR}/"
log "Artefacto Windows copiado: $(basename "${WINDOWS_ARTIFACT}")"

log "Artefactos disponibles en ${OUTPUT_DIR}"
ls -1 "${OUTPUT_DIR}"
