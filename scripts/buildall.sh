#!/usr/bin/env bash
# buildall.sh — lanza builds en paralelo en todos los hosts y copia los scripts de deploy.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

KEYCHAIN_PASS=""
TS="$(date '+%Y%m%d-%H%M%S')"
LOG_DIR="${PROJECT_ROOT}/buildall-logs/${TS}"

SFTP_REMOTE="linarese@fc16"
SFTP_PATH="Descargas/z"

usage() {
  cat <<'EOF'
Uso:
  buildall.sh [--password <keychain-password>] [-h]

Opciones:
  --password, -p <pass>   Password del llavero macOS para codesign (requerido para firmar)
  -h, --help              Muestra esta ayuda

Descripción:
  Lanza en paralelo builds en:
    mmela   (macOS)   — build-macos.sh --sign --sftpfc16
    mbp     (macOS)   — build-macos.sh --sign --sftpfc16
    fc16    (Linux)   — build-linux.sh --deb --appimage --sftpfc16
    freebsd (FreeBSD) — build-freebsd.sh --pkg --sftpfc16 (linarese@192.168.1.13)
    surface (Windows) — build-windows.ps1 --inno --sftpfc16

  Los logs de cada host se guardan en buildall-logs/<timestamp>/.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --password|-p)
      [[ $# -lt 2 ]] && { echo "Error: falta valor para $1" >&2; exit 1; }
      KEYCHAIN_PASS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "${KEYCHAIN_PASS}" ]]; then
  echo "Aviso: --password no especificado; los builds macOS podrían no firmar correctamente." >&2
fi

mkdir -p "${LOG_DIR}"

ts()  { date '+%H:%M:%S'; }
log() { local host="$1"; shift; echo "[$(ts)] [${host}] $*"; }

# Codificar el password en base64 para pasarlo sin problemas de quoting
ENCODED_PASS="$(printf '%s' "${KEYCHAIN_PASS}" | base64)"

# ---------------------------------------------------------------------------
# build en macOS
# ---------------------------------------------------------------------------
run_macos() {
  local host="$1"
  local logfile="${LOG_DIR}/${host}.log"
  log "${host}" "Iniciando build macOS..."
  # bash -l: login shell para que sourcea ~/.bash_profile y tenga el PATH de Homebrew.
  # Adicionalmente se fuerza el PATH de Homebrew por si el perfil es sólo zsh.
  # Se expande ${ENCODED_PASS} localmente; lo demás se ejecuta en remoto.
  ssh -o BatchMode=yes "${host}" bash -l -s << EOF >> "${logfile}" 2>&1
set -euo pipefail
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:\${PATH}"
cd ~/work/ZFSMgr
git pull
export KEYCHAIN_PASSWORD="\$(printf '%s' '${ENCODED_PASS}' | base64 -d)"
./scripts/build-macos.sh --sign --sftpfc16
EOF
  local rc=$?
  [[ ${rc} -eq 0 ]] && log "${host}" "Completado." || { log "${host}" "FALLÓ (rc=${rc}) — ver ${logfile}"; return 1; }
}

# ---------------------------------------------------------------------------
# build en Linux (fc16)
# ---------------------------------------------------------------------------
run_linux() {
  local logfile="${LOG_DIR}/fc16.log"
  log "fc16" "Iniciando build Linux..."
  ssh -o BatchMode=yes fc16 bash -s << 'EOF' >> "${logfile}" 2>&1
set -euo pipefail
cd ~/work/ZFSMgr
git pull
./scripts/build-linux.sh --deb --appimage --sftpfc16
EOF
  local rc=$?
  [[ ${rc} -eq 0 ]] && log "fc16" "Completado." || { log "fc16" "FALLÓ (rc=${rc}) — ver ${LOG_DIR}/fc16.log"; return 1; }
}

# ---------------------------------------------------------------------------
# build en FreeBSD (192.168.1.13)
# ---------------------------------------------------------------------------
run_freebsd() {
  local logfile="${LOG_DIR}/freebsd.log"
  log "freebsd" "Iniciando build FreeBSD..."
  ssh -o BatchMode=yes "linarese@192.168.1.13" sh -s << 'EOF' >> "${logfile}" 2>&1
set -euo pipefail
cd ~/work/ZFSMgr
git pull
./scripts/build-freebsd.sh --pkg --sftpfc16
EOF
  local rc=$?
  [[ ${rc} -eq 0 ]] && log "freebsd" "Completado." || { log "freebsd" "FALLÓ (rc=${rc}) — ver ${logfile}"; return 1; }
}

# ---------------------------------------------------------------------------
# build en Windows (surface)
# ---------------------------------------------------------------------------
run_windows() {
  local logfile="${LOG_DIR}/surface.log"
  log "surface" "Iniciando build Windows..."
  ssh -o BatchMode=yes surface \
    'pwsh -NoProfile -Command "& { cd ~/work/ZFSMgr; git pull; & ./scripts/build-windows.ps1 --inno --sftpfc16 }"' \
    >> "${logfile}" 2>&1
  local rc=$?
  [[ ${rc} -eq 0 ]] && log "surface" "Completado." || { log "surface" "FALLÓ (rc=${rc}) — ver ${logfile}"; return 1; }
}

# ---------------------------------------------------------------------------
# Lanzar todos en paralelo
# ---------------------------------------------------------------------------
run_macos "mmela" & PID_MMELA=$!
run_macos "mbp"   & PID_MBP=$!
run_linux         & PID_FC16=$!
run_freebsd       & PID_FREEBSD=$!
run_windows       & PID_SURFACE=$!

# ---------------------------------------------------------------------------
# Esperar y recoger resultados
# ---------------------------------------------------------------------------
FAILED=0
STATUS_MMELA="OK";   wait "${PID_MMELA}"   || { STATUS_MMELA="FALLÓ";   FAILED=1; }
STATUS_MBP="OK";     wait "${PID_MBP}"     || { STATUS_MBP="FALLÓ";     FAILED=1; }
STATUS_FC16="OK";    wait "${PID_FC16}"    || { STATUS_FC16="FALLÓ";    FAILED=1; }
STATUS_FREEBSD="OK"; wait "${PID_FREEBSD}" || { STATUS_FREEBSD="FALLÓ"; FAILED=1; }
STATUS_SURFACE="OK"; wait "${PID_SURFACE}" || { STATUS_SURFACE="FALLÓ"; FAILED=1; }

echo ""
echo "=== Resumen de builds ==="
printf "  %-10s %s\n" "mmela"   "${STATUS_MMELA}"
printf "  %-10s %s\n" "mbp"     "${STATUS_MBP}"
printf "  %-10s %s\n" "fc16"    "${STATUS_FC16}"
printf "  %-10s %s\n" "freebsd" "${STATUS_FREEBSD}"
printf "  %-10s %s\n" "surface" "${STATUS_SURFACE}"
echo ""
echo "Logs guardados en: ${LOG_DIR}/"
echo ""

# ---------------------------------------------------------------------------
# Resultado final
# ---------------------------------------------------------------------------
if [[ "${FAILED}" -ne 0 ]]; then
  echo "Algunos builds fallaron. Revisa los logs en ${LOG_DIR}/"
  exit 1
fi
echo "Todo completado correctamente."
