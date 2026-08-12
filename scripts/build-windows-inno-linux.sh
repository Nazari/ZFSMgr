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
# win32 y no win64: el compilador de Inno Setup (ISCC.exe) es un ejecutable de 32
# bits, así que un prefijo de 32 bits le vale y evita depender de la capa WoW64. La
# arquitectura del prefijo no influye en el instalador que se GENERA, que la decide
# el .iss.
WINEARCH="${WINEARCH:-win32}"
# URL directa de la release, no https://jrsoftware.org/download.php/is.exe: esa
# dejó de servir el ejecutable y ahora redirige a una página HTML de descarga, así
# que se bajaban 10 KB de HTML y wine fallaba con "No se encontró ISCC.exe" sin
# explicar por qué. No se notaba en una máquina donde Inno ya estuviera instalado en
# el prefijo de wine, porque entonces la descarga ni se intenta.
INNO_URL="${INNO_URL:-https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe}"
INNO_ISCC="${INNO_ISCC:-}"
APP_NAME="ZFSMgr"
APP_EXE="zfsmgr_qt.exe"
APP_VERSION=""
QT6_PREFIX="${QT6_WINDOWS_PREFIX:-}"
MINGW_TRIPLE="${CROSS_TRIPLE_WINDOWS:-x86_64-w64-mingw32}"
AGENT_BUNDLE_DIR="${AGENT_BUNDLE_DIR:-}"

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
  --agent-bundle-dir <dir> Directorio con agentes multi-OS para incluir en installer
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
    --agent-bundle-dir) shift; AGENT_BUNDLE_DIR="${1:-}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

[[ -d "${INPUT_DIR}" ]] || { echo "No existe input dir: ${INPUT_DIR}" >&2; exit 1; }

if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
fi
[[ -n "${APP_VERSION}" ]] || APP_VERSION="0.10.0rc1"

# WINEARCH NO se pasa al ejecutar: una vez creado el prefijo, su arquitectura manda,
# y forzar otra hace que wine aborte con "WINEARCH set to win64 but ... is a 32-bit
# installation". Solo se fija al crear el prefijo (ver ensure_wine_prefix).
run_wine() {
  if command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a env WINEPREFIX="${WINEPREFIX}" wine "$@"
  else
    env WINEPREFIX="${WINEPREFIX}" wine "$@"
  fi
}

ensure_wine_prefix() {
  command -v wine >/dev/null 2>&1 || { echo "wine no está instalado" >&2; exit 1; }
  command -v winepath >/dev/null 2>&1 || { echo "winepath no está instalado" >&2; exit 1; }
  # WINEARCH solo cuenta al CREAR el prefijo. Si ya existe se respeta el suyo, para
  # no romper prefijos previos (en el host había uno de 64 bits ya funcionando).
  local arch_env=()
  if [[ ! -f "${WINEPREFIX}/system.reg" ]]; then
    arch_env=(WINEARCH="${WINEARCH}")
  fi
  mkdir -p "${WINEPREFIX}"
  if command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a env WINEPREFIX="${WINEPREFIX}" "${arch_env[@]}" wineboot -u >/dev/null 2>&1 || true
  else
    env WINEPREFIX="${WINEPREFIX}" "${arch_env[@]}" wineboot -u >/dev/null 2>&1 || true
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
  curl -fL "${INNO_URL}" -o "${installer}" >&2
  # Un instalador de Inno ronda los 10 MB. Si lo descargado es minúsculo o no es un
  # ejecutable, se aborta aquí en vez de dejar que wine falle sin decir la causa.
  if [[ ! -s "${installer}" ]] || [[ "$(stat -c%s "${installer}")" -lt 1000000 ]]; then
    echo "Error: la descarga de Inno Setup no parece un instalador ($(stat -c%s "${installer}" 2>/dev/null || echo 0) bytes)." >&2
    echo "       URL: ${INNO_URL}" >&2
    exit 1
  fi
  # La salida de wine va a stderr: ensure_inno DEVUELVE la ruta de ISCC por stdout y
  # se captura con $(...). Sin esto, los "err:menubuilder" de wine se mezclan con esa
  # ruta y el llamante intenta ejecutarlos como si fueran una orden (exit 127).
  # Mismo error que ya hubo en ensure_aqt() del aprovisionamiento; conviene revisarlo
  # en cualquier función de este repositorio que devuelva un valor por stdout.
  run_wine "${installer}" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- >&2 2>&1
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
      # Las tres son obligatorias: sin ellas el ejecutable muere en Windows con
      # 0xC0000135 (DLL no encontrada) y sin decir cuál. Era un aviso, así que el
      # instalador se generaba igual y el fallo aparecía en la máquina del usuario.
      echo "[payload] Error: no se encontró ${name} para MinGW triple '${MINGW_TRIPLE}'." >&2
      echo "[payload] Sin esa DLL el ejecutable no arranca en Windows (0xC0000135)." >&2
      exit 1
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

  # El script que activa OpenSSH viaja con el instalador: así puede tener plazo,
  # registro y lógica, que en una línea de [Run] no caben.
  cp "${SCRIPT_DIR}/windows-enable-openssh.ps1" "${PAYLOAD_DIR}/" 2>/dev/null || \
    cp "$(dirname "$0")/windows-enable-openssh.ps1" "${PAYLOAD_DIR}/"

  # Incluir bundle multi-OS de agentes, si está disponible.
  if [[ -n "${AGENT_BUNDLE_DIR}" && -d "${AGENT_BUNDLE_DIR}" ]]; then
    mkdir -p "${PAYLOAD_DIR}/agents"
    cp -a "${AGENT_BUNDLE_DIR}/." "${PAYLOAD_DIR}/agents/"
    echo "[payload] Bundle de agentes incluido desde: ${AGENT_BUNDLE_DIR}"
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
; Activar OpenSSH Server en ESTA máquina, para poder gestionarla desde otro ZFSMgr.
;
; Es el problema del huevo y la gallina de Windows: ZFSMgr habla con una máquina remota
; SOLO por SSH —el RPC del agente viaja por un túnel sobre esa conexión—, así que sin
; sshd no hay forma de llegar ni para instalar el agente. Quien monta un Windows nuevo
; tenía que dar con los tres comandos de PowerShell y ejecutarlos a mano.
;
; Marcada por omisión porque es justo a lo que viene quien instala esto en un equipo
; que va a gestionar en remoto, pero VISIBLE y desmarcable: levantar un servidor SSH
; cambia la exposición del equipo en la red y eso no se hace en silencio.
Name: "opensshserver"; Description: "Enable OpenSSH Server (required to manage this machine remotely from another ZFSMgr)"; GroupDescription: "Remote access:"

[Run]
; shellexec: el ejecutable pide requireAdministrator en su manifiesto, y el lanzamiento
; por omisión de Inno es CreateProcess, que NO eleva — falla con ERROR_ELEVATION_REQUIRED.
; Solo ShellExecuteEx lee el manifiesto y pide confirmación a UAC.
;
; runasoriginaluser: sin esto la aplicación heredaría el token elevado del instalador, o
; sea que correría con la cuenta que consintió el UAC de la instalación. Si es otra
; distinta de la del escritorio, la configuración y el registro se escribirían en el
; perfil equivocado y al abrir después la aplicación desde su acceso directo no estarían.
; Los tres pasos que hacían falta a mano. Se ejecutan por separado para que un fallo en
; uno no impida los siguientes: en un Windows que ya trae la capacidad instalada, el
; Add-WindowsCapability devuelve error y aun así hay que arrancar y automatizar sshd.
;
; -WindowStyle Hidden y runhidden: el instalador no debe dejar ventanas negras sueltas.
; El "|Out-Null" y el ErrorAction evitan que un aviso pare el paso.
; Regla de firewall: sin ella el servicio arranca pero no se llega desde fuera, que es
; exactamente el síntoma más desconcertante —sshd corriendo y la conexión rechazada—.
;
; Sin `if (...) { }` a propósito: en Inno Setup la llave abre una constante, así que un
; bloque de PowerShell aborta la compilación del instalador. Si la regla ya existe, el
; comando falla y -ErrorAction se lo traga, que es el mismo efecto.
; Una sola llamada, a un script que viaja en el paquete. Antes eran tres líneas sueltas
; y la primera —Add-WindowsCapability— colgó una instalación: es una operación de
; servicing que puede irse a Windows Update y no volver. El script lleva plazo, registro
; en %TEMP%\\zfsmgr-openssh.log y sale siempre con 0, de modo que no puede impedir que
; ZFSMgr quede instalado.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\\windows-enable-openssh.ps1"""; StatusMsg: "Enabling OpenSSH Server (this can take a few minutes)..."; Flags: runhidden waituntilterminated; Tasks: opensshserver
Filename: "{app}\\${APP_EXE}"; Description: "Run ${APP_NAME}"; Flags: nowait postinstall skipifsilent shellexec runasoriginaluser

[Code]
// Avisar si no está OpenZFS, que es lo que da sentido a todo lo demás.
//
// Sin él no hay ni zfs.exe ni zpool.exe, así que ZFSMgr se instala y no puede hacer
// nada: ni gestionar esta máquina ni ser gestionada desde otra. Y en Windows no hay
// gestor de paquetes que lo resuelva, viene de un único sitio, así que callarlo deja al
// usuario buscándolo fuera.
//
// Se comprueba al terminar, no al empezar: instalar ZFSMgr es útil igualmente, y abortar
// por esto sería desproporcionado.
function OpenZfsInstalado(): Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf}\\OpenZFS On Windows\\zfs.exe'))
         or FileExists(ExpandConstant('{sys}\\zfs.exe'))
         or FileExists('C:\\Program Files\\OpenZFS On Windows\\zfs.exe');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ErrCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    if not OpenZfsInstalado() then
    begin
      if MsgBox('OpenZFS on Windows was not found on this machine.' + #13#10 + #13#10 +
                'ZFSMgr needs it to manage pools and datasets: it provides zfs.exe and' + #13#10 +
                'zpool.exe. Windows has no package manager for it, so it must be' + #13#10 +
                'downloaded from the project releases page.' + #13#10 + #13#10 +
                'Open the download page now?',
                mbConfirmation, MB_YESNO) = IDYES then
        ShellExec('open', 'https://github.com/openzfsonwindows/openzfs/releases',
                  '', '', SW_SHOW, ewNoWait, ErrCode);
    end;
  end;
end;
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
