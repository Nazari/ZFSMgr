#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/dist/ZFSMgr"
APP_DIR="${HOME}/Applications/ZFSMgr"
TARGET="$APP_DIR/ZFSMgr"

if [[ ! -x "$BIN" ]]; then
  echo "No se encuentra ejecutable en $BIN. Ejecuta primero: python3 packaging/build.py"
  exit 1
fi

mkdir -p "$APP_DIR"
cp "$BIN" "$TARGET"
chmod +x "$TARGET"
echo "Instalado en $TARGET"
echo "Puedes crear un alias o un launcher si quieres abrirlo desde Finder."
