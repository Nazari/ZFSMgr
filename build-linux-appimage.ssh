#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-linux"
APPDIR="${SCRIPT_DIR}/AppDir"
TOOLS_DIR="${SCRIPT_DIR}/.tools/appimage"
ARCH="$(uname -m)"

if [[ "${ARCH}" != "x86_64" ]]; then
  echo "Error: AppImage build script currently supports x86_64 only (detected: ${ARCH})." >&2
  exit 1
fi

mkdir -p "${TOOLS_DIR}"

LINUXDEPLOY_APPIMAGE="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_PLUGIN="${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"

download_if_missing() {
  local url="$1"
  local out="$2"
  if [[ -f "${out}" ]]; then
    return 0
  fi
  echo "Downloading $(basename "${out}")..."
  curl -fL "${url}" -o "${out}"
  chmod +x "${out}"
}

download_if_missing \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
  "${LINUXDEPLOY_APPIMAGE}"
download_if_missing \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
  "${LINUXDEPLOY_QT_PLUGIN}"

echo "Configuring and building Release binary..."
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"

echo "Preparing AppDir..."
rm -rf "${APPDIR}"
cmake --install "${BUILD_DIR}" --prefix "${APPDIR}/usr"
# Safety: never ship local connection secrets inside AppImage.
find "${APPDIR}" -type f -name "connections.ini" -delete || true

mkdir -p "${APPDIR}/usr/share/applications" "${APPDIR}/usr/share/icons/hicolor/512x512/apps"

cat > "${APPDIR}/usr/share/applications/zfsmgr.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=ZFSMgr
Comment=Cross-platform OpenZFS GUI manager
Exec=zfsmgr_qt
Icon=ZFSMgr
Categories=System;Utility;
Terminal=false
EOF

cp -f "${SCRIPT_DIR}/icons/ZFSMgr-512.png" "${APPDIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png"

echo "Building AppImage..."
export OUTPUT="${SCRIPT_DIR}/ZFSMgr-0.1.0-${ARCH}.AppImage"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
if [[ -z "${QMAKE}" ]]; then
  echo "Warning: qmake/qmake6 not found in PATH. linuxdeploy-plugin-qt may fail." >&2
fi

"${LINUXDEPLOY_APPIMAGE}" \
  --appdir "${APPDIR}" \
  --desktop-file "${APPDIR}/usr/share/applications/zfsmgr.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png" \
  --plugin qt \
  --output appimage

echo "AppImage generated:"
ls -lh "${SCRIPT_DIR}"/*.AppImage
