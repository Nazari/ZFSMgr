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
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#0f1c2e"/>
      <stop offset="100%" stop-color="#1f3554"/>
    </linearGradient>
  </defs>
  <rect x="6" y="6" width="116" height="116" rx="16" fill="url(#bg)"/>
  <rect x="14" y="14" width="100" height="100" rx="12" fill="none" stroke="#e8c76a" stroke-width="2"/>
  <path d="M28 30h72v8H28zm0 26h72v8H28z" fill="#57c4c0"/>
  <path d="M96 38L48 56h48z" fill="#57c4c0"/>
  <circle cx="64" cy="74" r="6" fill="#f3f6fb"/>
  <rect x="62" y="80" width="4" height="18" rx="2" fill="#f3f6fb"/>
  <rect x="50" y="92" width="28" height="4" rx="2" fill="#f3f6fb"/>
  <path d="M70 80l18-18" stroke="#e8c76a" stroke-width="3" stroke-linecap="round"/>
  <circle cx="30" cy="92" r="6" fill="#f6b73c"/>
  <circle cx="98" cy="92" r="6" fill="#a8d4ff"/>
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
