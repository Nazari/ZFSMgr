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
QT6_PREFIX="${QT6_WINDOWS_PREFIX:-}"
MINGW_TRIPLE="${CROSS_TRIPLE_WINDOWS:-x86_64-w64-mingw32}"

usage() {
  cat <<'EOF'
Uso:
  build-windows-inno-linux.sh [opciones]

Opciones:
  --input-dir <dir>     Directorio con binarios Windows (default: builds/cross-windows)
  --output-dir <dir>    Directorio de salida del instalador (default: builds/windows-installer)
  --version <v>         Versión del instalador (si no, se lee de CMakeLists)
  --exe <name.exe>      Ejecutable principal (default: zfsmgr_qt.exe)
  --qt-prefix <dir>     Prefijo Qt6 para Windows (bin/Qt6*.dll y plugins/). Por defecto
                        se usa QT6_WINDOWS_PREFIX del entorno.
  --mingw-triple <t>    Triple MinGW para localizar DLLs de runtime (default: x86_64-w64-mingw32)
  --wineprefix <dir>    WINEPREFIX para Inno Setup (default: ~/.wine-zfsmgr-inno)
  --inno-iscc <path>    Ruta a ISCC.exe o iscc nativo
  -h, --help            Muestra esta ayuda

Descripción:
  Genera un instalador Inno Setup (.exe) en Linux usando Wine.
  Incluye automáticamente Qt6 DLLs y plugins desde --qt-prefix, y las
  DLLs de runtime MinGW (libstdc++, libgcc, libwinpthread).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input-dir) shift; INPUT_DIR="${1:-}"; shift ;;
    --output-dir) shift; OUTPUT_DIR="${1:-}"; shift ;;
    --version) shift; APP_VERSION="${1:-}"; shift ;;
    --exe) shift; APP_EXE="${1:-}"; shift ;;
    --qt-prefix) shift; QT6_PREFIX="${1:-}"; shift ;;
    --mingw-triple) shift; MINGW_TRIPLE="${1:-}"; shift ;;
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

copy_qt6_dlls() {
  local dest="$1"
  if [[ -z "${QT6_PREFIX}" ]]; then
    echo "[payload] QT6_PREFIX no definido; se omiten Qt6 DLLs (establece QT6_WINDOWS_PREFIX o usa --qt-prefix)" >&2
    return 0
  fi
  local qt_bin="${QT6_PREFIX}/bin"
  if [[ ! -d "${qt_bin}" ]]; then
    echo "[payload] Directorio bin de Qt6 no encontrado: ${qt_bin}" >&2
    return 0
  fi
  local count=0
  while IFS= read -r -d '' dll; do
    cp -f "${dll}" "${dest}/"
    (( count++ )) || true
  done < <(find "${qt_bin}" -maxdepth 1 -type f -name 'Qt6*.dll' -print0)
  echo "[payload] Copiadas ${count} Qt6 DLLs desde ${qt_bin}"

  # Plugin directories
  local qt_plugins="${QT6_PREFIX}/plugins"
  local plugin_dirs=(platforms styles imageformats iconengines tls bearer networkinformation)
  local d
  for d in "${plugin_dirs[@]}"; do
    if [[ -d "${qt_plugins}/${d}" ]]; then
      cp -a "${qt_plugins}/${d}" "${dest}/"
      echo "[payload] Plugin dir copiado: ${d}"
    fi
  done
}

find_mingw_runtime_dll() {
  local name="$1"
  # Try compiler's own search path first
  local found
  found="$(${MINGW_TRIPLE}-gcc --print-file-name="${name}" 2>/dev/null || true)"
  if [[ -n "${found}" && "${found}" != "${name}" && -f "${found}" ]]; then
    printf '%s\n' "${found}"
    return 0
  fi
  # Common MinGW locations on Debian/Ubuntu
  local search_dirs=(
    "/usr/${MINGW_TRIPLE}/bin"
    "/usr/${MINGW_TRIPLE}/lib"
  )
  local d
  for d in "${search_dirs[@]}"; do
    [[ -f "${d}/${name}" ]] && { printf '%s\n' "${d}/${name}"; return 0; }
  done
  # GCC lib dirs
  while IFS= read -r -d '' candidate; do
    [[ -f "${candidate}/${name}" ]] && { printf '%s\n' "${candidate}/${name}"; return 0; }
  done < <(find /usr/lib/gcc/"${MINGW_TRIPLE}" -maxdepth 1 -type d -print0 2>/dev/null)
  return 1
}

copy_mingw_runtime() {
  local dest="$1"
  local runtime_dlls=(libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
  local name found
  for name in "${runtime_dlls[@]}"; do
    found="$(find_mingw_runtime_dll "${name}" || true)"
    if [[ -n "${found}" ]]; then
      cp -f "${found}" "${dest}/"
      echo "[payload] Runtime MinGW copiado: ${name}"
    else
      echo "[payload] Advertencia: no se encontró ${name} para MinGW triple '${MINGW_TRIPLE}'" >&2
    fi
  done
}

prepare_payload() {
  PAYLOAD_DIR="${OUTPUT_DIR}/payload"
  rm -rf "${PAYLOAD_DIR}"
  mkdir -p "${PAYLOAD_DIR}"
  [[ -f "${INPUT_DIR}/${APP_EXE}" ]] || { echo "No existe ${INPUT_DIR}/${APP_EXE}" >&2; exit 1; }

  cp -f "${INPUT_DIR}/${APP_EXE}" "${PAYLOAD_DIR}/"

  # DLLs ya presentes en el directorio de build (p.ej. OpenSSL u otras dependencias precopiadas)
  find "${INPUT_DIR}" -maxdepth 1 -type f -name '*.dll' -exec cp -f {} "${PAYLOAD_DIR}/" \;

  # Plugin dirs ya presentes en el directorio de build
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

  # Copiar Qt6 DLLs y plugins desde el prefijo de Qt para Windows
  copy_qt6_dlls "${PAYLOAD_DIR}"

  # Copiar DLLs de runtime MinGW
  copy_mingw_runtime "${PAYLOAD_DIR}"

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
