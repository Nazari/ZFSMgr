#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_DIR="${PROJECT_ROOT}/resources"
INPUT_DIR="${PROJECT_ROOT}/builds/cross-windows"
OUTPUT_DIR="${PROJECT_ROOT}/builds/windows-installer"
PAYLOAD_DIR="${OUTPUT_DIR}/payload"
ISS_FILE="${OUTPUT_DIR}/zfsmgr-installer.iss"
WINEPREFIX="${WINEPREFIX:-${HOME}/.wine-zfsmgr-inno}"
WINEARCH="${WINEARCH:-win64}"
INNO_URL="${INNO_URL:-https://jrsoftware.org/download.php/is.exe}"
INNO_ISCC="${INNO_ISCC:-}"
APP_NAME="ZFSMgr"
APP_EXE="zfsmgr_qt.exe"
APP_VERSION=""

usage() {
  cat <<'EOF'
Uso:
  build-windows-inno-linux.sh [opciones]

Opciones:
  --input-dir <dir>     Directorio con binarios Windows (default: builds/cross-windows)
  --output-dir <dir>    Directorio de salida del instalador (default: builds/windows-installer)
  --version <v>         Versión del instalador (si no, se lee de CMakeLists)
  --exe <name.exe>      Ejecutable principal (default: zfsmgr_qt.exe)
  --wineprefix <dir>    WINEPREFIX para Inno Setup (default: ~/.wine-zfsmgr-inno)
  --inno-iscc <path>    Ruta a ISCC.exe o iscc nativo
  -h, --help            Muestra esta ayuda

Descripción:
  Genera un instalador Inno Setup (.exe) en Linux usando Wine.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input-dir) shift; INPUT_DIR="${1:-}"; shift ;;
    --output-dir) shift; OUTPUT_DIR="${1:-}"; shift ;;
    --version) shift; APP_VERSION="${1:-}"; shift ;;
    --exe) shift; APP_EXE="${1:-}"; shift ;;
    --wineprefix) shift; WINEPREFIX="${1:-}"; shift ;;
    --inno-iscc) shift; INNO_ISCC="${1:-}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

[[ -d "${INPUT_DIR}" ]] || { echo "No existe input dir: ${INPUT_DIR}" >&2; exit 1; }

if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
fi
[[ -n "${APP_VERSION}" ]] || APP_VERSION="0.10.0rc1"

run_wine() {
  if command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a env WINEPREFIX="${WINEPREFIX}" WINEARCH="${WINEARCH}" wine "$@"
  else
    env WINEPREFIX="${WINEPREFIX}" WINEARCH="${WINEARCH}" wine "$@"
  fi
}

ensure_wine_prefix() {
  command -v wine >/dev/null 2>&1 || { echo "wine no está instalado" >&2; exit 1; }
  command -v winepath >/dev/null 2>&1 || { echo "winepath no está instalado" >&2; exit 1; }
  mkdir -p "${WINEPREFIX}"
  if command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a env WINEPREFIX="${WINEPREFIX}" WINEARCH="${WINEARCH}" wineboot -u >/dev/null 2>&1 || true
  else
    env WINEPREFIX="${WINEPREFIX}" WINEARCH="${WINEARCH}" wineboot -u >/dev/null 2>&1 || true
  fi
}

find_iscc() {
  if [[ -n "${INNO_ISCC}" && -f "${INNO_ISCC}" ]]; then
    printf '%s\n' "${INNO_ISCC}"
    return 0
  fi
  if command -v iscc >/dev/null 2>&1; then
    command -v iscc
    return 0
  fi
  local candidates=(
    "${WINEPREFIX}/drive_c/Program Files (x86)/Inno Setup 6/ISCC.exe"
    "${WINEPREFIX}/drive_c/Program Files/Inno Setup 6/ISCC.exe"
  )
  local c
  for c in "${candidates[@]}"; do
    [[ -f "${c}" ]] && { printf '%s\n' "${c}"; return 0; }
  done
  return 1
}

ensure_inno() {
  local iscc_path
  iscc_path="$(find_iscc || true)"
  if [[ -n "${iscc_path}" ]]; then
    printf '%s\n' "${iscc_path}"
    return 0
  fi
  ensure_wine_prefix
  mkdir -p /tmp
  local installer="/tmp/innosetup-installer.exe"
  curl -fL "${INNO_URL}" -o "${installer}"
  run_wine "${installer}" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
  iscc_path="$(find_iscc || true)"
  [[ -n "${iscc_path}" ]] || { echo "No se encontró ISCC.exe tras instalar Inno Setup" >&2; exit 1; }
  printf '%s\n' "${iscc_path}"
}

prepare_payload() {
  PAYLOAD_DIR="${OUTPUT_DIR}/payload"
  rm -rf "${PAYLOAD_DIR}"
  mkdir -p "${PAYLOAD_DIR}"
  [[ -f "${INPUT_DIR}/${APP_EXE}" ]] || { echo "No existe ${INPUT_DIR}/${APP_EXE}" >&2; exit 1; }

  cp -f "${INPUT_DIR}/${APP_EXE}" "${PAYLOAD_DIR}/"
  find "${INPUT_DIR}" -maxdepth 1 -type f -name '*.dll' -exec cp -f {} "${PAYLOAD_DIR}/" \;

  local dirs=(platforms styles imageformats iconengines tls bearer networkinformation plugins)
  local d
  for d in "${dirs[@]}"; do
    if [[ -d "${INPUT_DIR}/${d}" ]]; then
      cp -a "${INPUT_DIR}/${d}" "${PAYLOAD_DIR}/"
    fi
  done

  if [[ -f "${INPUT_DIR}/qt.conf" ]]; then
    cp -f "${INPUT_DIR}/qt.conf" "${PAYLOAD_DIR}/"
  fi
  return 0
}

generate_iss() {
  ISS_FILE="${OUTPUT_DIR}/zfsmgr-installer.iss"
  cat > "${ISS_FILE}" <<EOF
[Setup]
AppId={{A86A99F9-2E0A-4E35-9C20-6B7B83D59C52}
AppName=${APP_NAME}
AppVersion=${APP_VERSION}
AppPublisher=ZFSMgr
DefaultDirName={autopf}\\${APP_NAME}
DefaultGroupName=${APP_NAME}
OutputDir=.
OutputBaseFilename=ZFSMgr-Setup-${APP_VERSION}
Compression=lzma
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequired=admin

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "payload\\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\\${APP_NAME}"; Filename: "{app}\\${APP_EXE}"
Name: "{autodesktop}\\${APP_NAME}"; Filename: "{app}\\${APP_EXE}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\\${APP_EXE}"; Description: "Run ${APP_NAME}"; Flags: nowait postinstall skipifsilent
EOF
}

build_installer() {
  local iscc_path="$1"
  mkdir -p "${OUTPUT_DIR}"
  prepare_payload
  generate_iss
  if [[ "${iscc_path}" == *.exe ]]; then
    local iss_win
    iss_win="$(WINEPREFIX="${WINEPREFIX}" winepath -w "${ISS_FILE}")"
    ( cd "${OUTPUT_DIR}" && run_wine "${iscc_path}" "${iss_win}" )
  else
    ( cd "${OUTPUT_DIR}" && "${iscc_path}" "${ISS_FILE}" )
  fi
}

iscc="$(ensure_inno)"
build_installer "${iscc}"

installer="$(find "${OUTPUT_DIR}" -maxdepth 1 -type f -name "ZFSMgr-Setup-${APP_VERSION}*.exe" | sort -V | tail -n1 || true)"
[[ -n "${installer}" ]] || { echo "No se generó el instalador Inno" >&2; exit 1; }
echo "Instalador generado: ${installer}"
