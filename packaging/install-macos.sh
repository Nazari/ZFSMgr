#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE_SRC="$ROOT/dist/ZFSMgr.app"
BIN="$ROOT/dist/ZFSMgr"
APP_DIR="${HOME}/Applications"
APP_BUNDLE_DST="$APP_DIR/ZFSMgr.app"
BIN_DIR="${HOME}/Applications/ZFSMgr"
BIN_DST="$BIN_DIR/ZFSMgr"

if [[ -d "$APP_BUNDLE_SRC" ]]; then
  mkdir -p "$APP_DIR"
  rm -rf "$APP_BUNDLE_DST"
  ditto "$APP_BUNDLE_SRC" "$APP_BUNDLE_DST"
  # Evita bloqueos por cuarentena al copiar builds locales.
  xattr -dr com.apple.quarantine "$APP_BUNDLE_DST" 2>/dev/null || true
  echo "Instalado bundle nativo en $APP_BUNDLE_DST"
  echo "Abre ZFSMgr con doble click en Finder."
  exit 0
fi

if [[ ! -x "$BIN" ]]; then
  echo "No se encuentra $APP_BUNDLE_SRC ni ejecutable en $BIN. Ejecuta primero: python3 packaging/build.py"
  exit 1
fi

mkdir -p "$BIN_DIR"
cp "$BIN" "$BIN_DST"
chmod +x "$BIN_DST"
echo "Instalado binario en $BIN_DST (fallback sin .app)."
echo "Recomendado: regenerar con PyInstaller para obtener dist/ZFSMgr.app."
