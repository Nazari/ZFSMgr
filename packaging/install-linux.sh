#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/dist/ZFSMgr"
TARGET_DIR="${HOME}/.local/bin"
TARGET="$TARGET_DIR/ZFSMgr"
APP_DIR="${HOME}/.local/share/applications"
DESKTOP_FILE="${APP_DIR}/zfsmgr.desktop"
ICON_DIR="${HOME}/.local/share/icons/hicolor/scalable/apps"
ICON_FILE="${ICON_DIR}/zfsmgr.svg"

if [[ ! -x "$BIN" ]]; then
  echo "No se encuentra ejecutable en $BIN. Ejecuta primero: python3 packaging/build.py"
  exit 1
fi

mkdir -p "$TARGET_DIR"
cp "$BIN" "$TARGET"
chmod +x "$TARGET"

mkdir -p "$ICON_DIR"
cat >"$ICON_FILE" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <rect x="8" y="8" width="112" height="112" rx="16" fill="#1f5f7a"/>
  <rect x="22" y="24" width="84" height="80" rx="8" fill="#f3f6f8"/>
  <text x="64" y="56" text-anchor="middle" font-family="sans-serif" font-size="18" font-weight="700" fill="#1f5f7a">ZFS</text>
  <text x="64" y="78" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#1f5f7a">Manager</text>
</svg>
EOF

mkdir -p "$APP_DIR"
cat >"$DESKTOP_FILE" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=ZFSMgr
Comment=Gestion de pools y datasets ZFS
Exec=${TARGET}
Icon=${ICON_FILE}
Terminal=false
Categories=System;Utility;
StartupNotify=true
EOF

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "${APP_DIR}" >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -q "${HOME}/.local/share/icons/hicolor" >/dev/null 2>&1 || true
fi

echo "Instalado binario en: $TARGET"
echo "Instalado lanzador en: $DESKTOP_FILE"
echo "Instalado icono en: $ICON_FILE"
echo "Asegura que ${HOME}/.local/bin este en tu PATH."
