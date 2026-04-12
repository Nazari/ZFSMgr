#!/usr/bin/env bash
set -euo pipefail

DO_WINDOWS=0
DO_FREEBSD=0
DO_MACOS=0
DRY_RUN=0

usage() {
  cat <<'USAGE'
Uso:
  setup-cross-linux.sh [--windows] [--freebsd] [--macos] [--all] [--dry-run]

Opciones:
  --windows   Instala base de cross para Windows (mingw-w64 + herramientas)
  --freebsd   Instala base de cross para FreeBSD (clang/lld/cmake/ninja)
  --macos     Prepara dependencias host para osxcross (no instala SDK de Apple)
  --all       Equivale a --windows --freebsd --macos
  --dry-run   Muestra comandos sin ejecutarlos
  -h, --help  Muestra esta ayuda

Notas:
- macOS cross requiere SDK de Apple proporcionado por el usuario (licencia Apple).
- Qt6 para target (Windows/FreeBSD/macOS) debe instalarse aparte y exportarse en:
  QT6_WINDOWS_PREFIX, QT6_FREEBSD_PREFIX, QT6_MACOS_PREFIX
- Si ejecutas en entorno no interactivo, puedes pasar la contraseña de sudo por:
  export SUDO_PASSWORD='...'
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --windows) DO_WINDOWS=1; shift ;;
    --freebsd) DO_FREEBSD=1; shift ;;
    --macos) DO_MACOS=1; shift ;;
    --all) DO_WINDOWS=1; DO_FREEBSD=1; DO_MACOS=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ ${DO_WINDOWS} -eq 0 && ${DO_FREEBSD} -eq 0 && ${DO_MACOS} -eq 0 ]]; then
  usage
  exit 1
fi

run_cmd() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$*"
  fi
}

run_root_cmd() {
  local cmd="$*"
  if [[ ${DRY_RUN} -eq 1 ]]; then
    echo "[dry-run] sudo ${cmd}"
    return 0
  fi
  if [[ -n "${SUDO_PASSWORD:-}" ]]; then
    printf '%s\n' "${SUDO_PASSWORD}" | sudo -S bash -lc "${cmd}"
  else
    sudo bash -lc "${cmd}"
  fi
}

if ! command -v apt-get >/dev/null 2>&1; then
  echo "Este script está preparado para distros Debian/Ubuntu (apt-get)." >&2
  echo "Instala manualmente las dependencias equivalentes en tu distro." >&2
  exit 2
fi

run_root_cmd "apt-get update"

if [[ ${DO_WINDOWS} -eq 1 ]]; then
  run_root_cmd "apt-get install -y mingw-w64 binutils-mingw-w64 g++-mingw-w64 cmake ninja-build pkg-config"
fi

if [[ ${DO_FREEBSD} -eq 1 ]]; then
  run_root_cmd "apt-get install -y clang lld llvm cmake ninja-build pkg-config"
fi

if [[ ${DO_MACOS} -eq 1 ]]; then
  run_root_cmd "apt-get install -y clang lld cmake ninja-build pkg-config git python3 xz-utils libxml2-dev zlib1g-dev"
  cat <<'MAC_NOTE'

[macOS cross]
1) Clona e instala osxcross en una ruta local, por ejemplo: /opt/osxcross
2) Proporciona un SDK de macOS válido (Xcode/Mac)
3) Exporta variables:
   export OSXCROSS_TARGET=x86_64-apple-darwin23
   export OSX_SYSROOT=/opt/osxcross/target/SDK/MacOSX*.sdk
   export PATH=/opt/osxcross/target/bin:$PATH

MAC_NOTE
fi

echo "Setup base completado."
