#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/dist/ZFSMgr"
TARGET_DIR="${HOME}/.local/bin"
TARGET="$TARGET_DIR/ZFSMgr"

if [[ ! -x "$BIN" ]]; then
  echo "No se encuentra ejecutable en $BIN. Ejecuta primero: python3 packaging/build.py"
  exit 1
fi

mkdir -p "$TARGET_DIR"
cp "$BIN" "$TARGET"
chmod +x "$TARGET"
echo "Instalado en $TARGET"
echo "Asegura que ${HOME}/.local/bin esté en tu PATH."
