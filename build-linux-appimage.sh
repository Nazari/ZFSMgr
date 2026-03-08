#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-linux"
APPDIR="${SCRIPT_DIR}/AppDir"
TOOLS_DIR="${SCRIPT_DIR}/.tools/appimage"
SOURCE_DIR="${SCRIPT_DIR}/resources"
ARCH="$(uname -m)"
SFTP_TARGET="${ZFSMGR_SFTP_TARGET:-sftp://linarese@fc16:Descargas/z}"
APP_VERSION="$(sed -n 's/^project(ZFSMgrQt VERSION \([0-9.]*\).*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
UPLOAD_SFTP=0
if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="0.9.1"
fi

EXTRA_ARGS=()
for arg in "$@"; do
  if [[ "${arg}" == "--sftpfc16" ]]; then
    UPLOAD_SFTP=1
  else
    EXTRA_ARGS+=("${arg}")
  fi
done

parse_sftp_target() {
  local target="$1"
  local authority path user host host_and_base base_path
  if [[ "${target}" =~ ^sftp:// || "${target}" =~ ^sft:// ]]; then
    target="${target#sftp://}"
    target="${target#sft://}"
    authority="${target%%/*}"
    path="/${target#*/}"
    if [[ "${authority}" == *"@"* ]]; then
      user="${authority%@*}"
      host_and_base="${authority#*@}"
      if [[ "${host_and_base}" == *":"* ]]; then
        host="${host_and_base%%:*}"
        base_path="${host_and_base#*:}"
        if [[ -n "${base_path}" ]]; then
          if [[ "${path}" == "/" ]]; then
            path=""
          fi
          if [[ "${base_path}" == /* ]]; then
            path="${base_path}${path}"
          else
            # host:path/... => path relativa al HOME remoto
            path="${base_path}${path}"
          fi
        fi
      else
        host="${host_and_base}"
      fi
    elif [[ "${authority}" == *":"* ]]; then
      # Formato legacy soportado: sftp://user:host/ruta
      user="${authority%%:*}"
      host="${authority#*:}"
    else
      user="${USER:-linarese}"
      host="${authority}"
    fi
  elif [[ "${target}" == *":"* ]]; then
    user="${target%%@*}"
    if [[ "${target}" != *"@"* ]]; then
      user="${USER:-linarese}"
    fi
    host="${target#*@}"
    host="${host%%:*}"
    path="/${target#*:}"
  else
    echo "Error: destino SFTP inválido: ${target}" >&2
    return 1
  fi
  echo "${user}@${host}|${path}"
}

upload_to_sftp() {
  local artifact="$1"
  local parsed remote path
  parsed="$(parse_sftp_target "${SFTP_TARGET}")"
  remote="${parsed%%|*}"
  path="${parsed#*|}"
  echo "Subiendo artefacto a ${remote}:${path}"
  if [[ "${path}" == /* ]]; then
    ssh -o BatchMode=yes "${remote}" "mkdir -p '${path}'"
    scp "${artifact}" "${remote}:${path}/"
  else
    ssh -o BatchMode=yes "${remote}" "mkdir -p \"\$HOME/${path}\""
    scp "${artifact}" "${remote}:~/${path}/"
  fi
}

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
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${EXTRA_ARGS[@]}"
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
export OUTPUT="${SCRIPT_DIR}/ZFSMgr-${APP_VERSION}-${ARCH}.AppImage"
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
if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
  upload_to_sftp "${OUTPUT}"
fi
