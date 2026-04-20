#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ZFSMGR_BUILD_DIR:-${PROJECT_ROOT}/builds/linux}"
SOURCE_DIR="${PROJECT_ROOT}/resources"
APPDIR="${PROJECT_ROOT}/AppDir"
TOOLS_DIR="${PROJECT_ROOT}/.tools/appimage"
ARCH="$(uname -m)"
SFTP_TARGET="${ZFSMGR_SFTP_TARGET:-sftp://linarese@fc16:Descargas/z}"
DOWNLOADS_DIR="${DOWNLOADS_DIR:-${HOME}/Downloads/z}"
APP_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
BUILD_APPIMAGE=0
BUILD_DEB=0
UPLOAD_SFTP=0
EXTRA_ARGS=()

usage() {
  cat <<'EOF'
Uso:
  build-linux.sh [opciones] [-- <args extra de CMake>]

Opciones:
  --appimage   Genera también el artefacto AppImage
  --deb        Genera también el paquete .deb mediante CPack
  --sftpfc16   Sube los artefactos finales (.AppImage, .deb) al destino SFTP configurado
  -h, --help   Muestra esta ayuda

Variables opcionales:
  ZFSMGR_SFTP_TARGET  Destino SFTP para --sftpfc16

Ejemplos:
  ./scripts/build-linux.sh
  ./scripts/build-linux.sh --deb
  ./scripts/build-linux.sh --appimage --deb
EOF
}

for arg in "$@"; do
  case "${arg}" in
    -h|--help)
      usage
      exit 0
      ;;
  esac
done

resolve_app_version() {
  local version=""
  if [[ -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    version="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
  fi
  if [[ -z "${version}" && -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    version="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*ZFSMgrQt[[:space:]]+VERSION[[:space:]]+([^[:space:])]+).*/\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
  fi
  if [[ -z "${version}" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    version="$(sed -n 's/^ZFSMGR_APP_VERSION_STRING:UNINITIALIZED=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
  fi
  if [[ -z "${version}" ]]; then
    version="0.10.0rc1"
  fi
  printf '%s\n' "${version}"
}

APP_VERSION="$(resolve_app_version)"

for arg in "$@"; do
  case "${arg}" in
    --appimage)
      BUILD_APPIMAGE=1
      ;;
    --deb)
      BUILD_DEB=1
      ;;
    --sftpfc16)
      UPLOAD_SFTP=1
      ;;
    *)
      EXTRA_ARGS+=("${arg}")
      ;;
  esac
done

if [[ "${UPLOAD_SFTP}" -eq 1 && "${BUILD_APPIMAGE}" -eq 0 && "${BUILD_DEB}" -eq 0 ]]; then
  echo "Error: --sftpfc16 requiere --appimage o --deb." >&2
  exit 1
fi

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
            path="${base_path}${path}"
          fi
        fi
      else
        host="${host_and_base}"
      fi
    elif [[ "${authority}" == *":"* ]]; then
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
    ssh -o BatchMode=yes "${remote}" "mkdir -p '${path}'" </dev/null
    scp "${artifact}" "${remote}:${path}/" </dev/null
  else
    ssh -o BatchMode=yes "${remote}" "mkdir -p \"\$HOME/${path}\"" </dev/null
    scp "${artifact}" "${remote}:~/${path}/" </dev/null
  fi
}

upload_deb_artifacts() {
  local deb_file
  local found=0
  while IFS= read -r -d '' deb_file; do
    upload_to_sftp "${deb_file}"
    found=1
  done < <(find "${BUILD_DIR}" -maxdepth 1 -type f -name '*.deb' -print0)
  if [[ ${found} -eq 0 ]]; then
    echo "Error: no se encontró ningún paquete .deb para subir." >&2
    exit 1
  fi
}

ensure_build_dir_source_match() {
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    return 0
  fi
  local cached_source current_source
  cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
  current_source="$(cd "${SOURCE_DIR}" && pwd -P)"
  if [[ -z "${cached_source}" ]]; then
    return 0
  fi
  if [[ "${cached_source}" != "${current_source}" ]]; then
    echo "Detectado build cache con fuente distinta:"
    echo "  cache:   ${cached_source}"
    echo "  actual:  ${current_source}"
    echo "Regenerando ${BUILD_DIR}..."
    rm -rf "${BUILD_DIR}"
  fi
}

if [[ "${BUILD_APPIMAGE}" -eq 0 && "${BUILD_DEB}" -eq 0 ]]; then
  ensure_build_dir_source_match
  cmake_args=(-S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
  if [[ -n "${ZFSMGR_AGENT_BUNDLE_DIR:-}" ]]; then
    cmake_args+=("-DZFSMGR_AGENT_BUNDLE_DIR=${ZFSMGR_AGENT_BUNDLE_DIR}")
  fi
  cmake "${cmake_args[@]}" "${EXTRA_ARGS[@]}"
  cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"
  echo "Build completado: ${BUILD_DIR}/zfsmgr_qt"
  exit 0
fi

if [[ "${BUILD_APPIMAGE}" -eq 1 && "${ARCH}" != "x86_64" ]]; then
  echo "Error: AppImage build script currently supports x86_64 only (detected: ${ARCH})." >&2
  exit 1
fi

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

extract_appimage_tool() {
  local appimage_path="$1"
  local extract_dir="$2"
  local marker="${extract_dir}/AppRun"
  if [[ -x "${marker}" ]]; then
    printf '%s\n' "${marker}"
    return 0
  fi
  rm -rf "${extract_dir}"
  mkdir -p "${extract_dir}"
  (
    cd "${extract_dir}"
    APPIMAGE_EXTRACT_AND_RUN=1 "${appimage_path}" --appimage-extract >/dev/null
  )
  if [[ ! -x "${extract_dir}/squashfs-root/AppRun" ]]; then
    echo "Error: no se pudo extraer ${appimage_path}" >&2
    return 1
  fi
  mv "${extract_dir}/squashfs-root/"* "${extract_dir}/"
  rm -rf "${extract_dir}/squashfs-root"
  printf '%s\n' "${extract_dir}/AppRun"
}

echo "Configuring and building Release binary..."
ensure_build_dir_source_match
cmake_args=(-S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ -n "${ZFSMGR_AGENT_BUNDLE_DIR:-}" ]]; then
  cmake_args+=("-DZFSMGR_AGENT_BUNDLE_DIR=${ZFSMGR_AGENT_BUNDLE_DIR}")
fi
cmake "${cmake_args[@]}" "${EXTRA_ARGS[@]}"
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"

if [[ "${BUILD_DEB}" -eq 1 ]]; then
  echo "Building Debian package..."
  cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G DEB -B "${BUILD_DIR}"
  echo "DEB generated:"
  ls -lh "${BUILD_DIR}"/*.deb
  if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
    upload_deb_artifacts
  fi
fi

if [[ "${BUILD_APPIMAGE}" -eq 0 ]]; then
  exit 0
fi

mkdir -p "${TOOLS_DIR}"

LINUXDEPLOY_APPIMAGE="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_PLUGIN="${TOOLS_DIR}/linuxdeploy-plugin-qt-x86_64.AppImage"
APPIMAGE_RUNTIME_FILE="${TOOLS_DIR}/runtime-x86_64"

download_if_missing \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
  "${LINUXDEPLOY_APPIMAGE}"
download_if_missing \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
  "${LINUXDEPLOY_QT_PLUGIN}"
download_if_missing \
  "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64" \
  "${APPIMAGE_RUNTIME_FILE}"

# Si quedó un wrapper temporal de una ejecución previa, restaurar el plugin real.
if [[ -f "${LINUXDEPLOY_QT_PLUGIN}.real" ]]; then
  mv -f "${LINUXDEPLOY_QT_PLUGIN}.real" "${LINUXDEPLOY_QT_PLUGIN}"
fi

echo "Preparing AppDir..."
rm -rf "${APPDIR}"
cmake --install "${BUILD_DIR}" --prefix "${APPDIR}/usr"
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

cp -f "${PROJECT_ROOT}/icons/ZFSMgr-512.png" "${APPDIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png"

echo "Building AppImage..."
export OUTPUT="${BUILD_DIR}/ZFSMgr-${APP_VERSION}-${ARCH}.AppImage"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
LINUXDEPLOY_BIN="${LINUXDEPLOY_APPIMAGE}"
if [[ -z "${QMAKE}" ]]; then
  echo "Warning: qmake/qmake6 not found in PATH. linuxdeploy-plugin-qt may fail." >&2
fi
if [[ ! -c /dev/fuse ]] || { ! command -v fusermount >/dev/null 2>&1 && ! command -v fusermount3 >/dev/null 2>&1; }; then
  # En entornos sin FUSE, forzar modo extract-and-run para los AppImage tools.
  export APPIMAGE_EXTRACT_AND_RUN=1
fi
export LDAI_RUNTIME_FILE="${APPIMAGE_RUNTIME_FILE}"

"${LINUXDEPLOY_BIN}" \
  --appdir "${APPDIR}" \
  --desktop-file "${APPDIR}/usr/share/applications/zfsmgr.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png" \
  --plugin qt \
  --output appimage

echo "AppImage generated:"
ls -lh "${BUILD_DIR}"/*.AppImage
  if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
    if [[ -f "${OUTPUT}" ]]; then
      upload_to_sftp "${OUTPUT}"
    else
      echo "Error: no se encontró AppImage ${OUTPUT}" >&2
      exit 1
    fi
  fi
