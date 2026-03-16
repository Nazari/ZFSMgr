#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-macos"
SOURCE_DIR="${PROJECT_ROOT}/resources"
APP_VERSION=""
BUNDLE_NAME=""
BUNDLE_APP=1
SELF_SIGN_CERT_NAME="${SELF_SIGN_CERT_NAME:-ZFSMgr Local Self-Signed}"
KEYCHAIN_PASSWORD="${KEYCHAIN_PASSWORD:-${MAC_PASS:-}}"
SFTP_TARGET="${ZFSMGR_SFTP_TARGET:-sftp://linarese@fc16:Descargas/z}"
UPLOAD_SFTP=0
SIGN_APP_MODE="auto" # auto|yes|no
EXTRA_CMAKE_ARGS=()

for arg in "$@"; do
  if [[ "${arg}" == "--bundle" ]]; then
    BUNDLE_APP=1
  elif [[ "${arg}" == "--no-bundle" ]]; then
    BUNDLE_APP=0
  elif [[ "${arg}" == "--sftpfc16" ]]; then
    UPLOAD_SFTP=1
  elif [[ "${arg}" == "--sign" ]]; then
    SIGN_APP_MODE="yes"
  elif [[ "${arg}" == "--no-sign" ]]; then
    SIGN_APP_MODE="no"
  else
    EXTRA_CMAKE_ARGS+=("${arg}")
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
      # Soporta formato legacy: sftp://user:host/ruta
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
    scp -r "${artifact}" "${remote}:${path}/"
  else
    ssh -o BatchMode=yes "${remote}" "mkdir -p \"\$HOME/${path}\""
    scp -r "${artifact}" "${remote}:~/${path}/"
  fi
}

has_codesign_identity() {
  local cert_name="$1"
  if security find-identity -v -p codesigning | grep -F "\"${cert_name}\"" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

ensure_codesign_identity() {
  local cert_name="$1"
  if has_codesign_identity "${cert_name}"; then
    return 0
  fi
  cat >&2 <<EOF
Error: no se encontró la identidad de firma '${cert_name}'.
Crea primero un certificado de firma de código autofirmado en "Keychain Access":
1) Keychain Access > Certificate Assistant > Create a Certificate...
2) Name: ${cert_name}
3) Identity Type: Self Signed Root
4) Certificate Type: Code Signing
5) Guardarlo en tu llavero de login
Luego vuelve a ejecutar: ./scripts/build-macos.sh --bundle
EOF
  exit 1
}

prepare_codesign_keychain() {
  local keychain_path="${HOME}/Library/Keychains/login.keychain-db"
  if [[ ! -f "${keychain_path}" ]]; then
    echo "Aviso: no se encontró login keychain en ${keychain_path}" >&2
    return 0
  fi
  security list-keychains -d user -s "${keychain_path}" >/dev/null 2>&1 || true
  security default-keychain -d user -s "${keychain_path}" >/dev/null 2>&1 || true
  if [[ -n "${KEYCHAIN_PASSWORD}" ]]; then
    security unlock-keychain -p "${KEYCHAIN_PASSWORD}" "${keychain_path}" >/dev/null 2>&1 || true
  fi
  security set-keychain-settings -lut 7200 "${keychain_path}" >/dev/null 2>&1 || true
}

# Soporte Homebrew Apple Silicon e Intel.
if [[ -d "/opt/homebrew/opt/qt" ]]; then
  QT_PREFIX="/opt/homebrew/opt/qt"
elif [[ -d "/usr/local/opt/qt" ]]; then
  QT_PREFIX="/usr/local/opt/qt"
elif [[ -d "/opt/homebrew/opt/qt@6" ]]; then
  QT_PREFIX="/opt/homebrew/opt/qt@6"
elif [[ -d "/usr/local/opt/qt@6" ]]; then
  QT_PREFIX="/usr/local/opt/qt@6"
else
  QT_PREFIX=""
fi

if [[ -n "${QT_PREFIX}" ]]; then
  export PATH="${QT_PREFIX}/bin:${PATH}"
  export CMAKE_PREFIX_PATH="${QT_PREFIX}:${CMAKE_PREFIX_PATH:-}"
  export QT_PLUGIN_PATH="${QT_PREFIX}/plugins"
  export QML2_IMPORT_PATH="${QT_PREFIX}/qml"
  export DYLD_FRAMEWORK_PATH="${QT_PREFIX}/lib"
  export DYLD_LIBRARY_PATH="${QT_PREFIX}/lib"
fi

QT_EXTRA_LIB_DIRS=()
add_qt_lib_dir() {
  local libdir="$1"
  if [[ -d "${libdir}" ]]; then
    local existing
    for existing in "${QT_EXTRA_LIB_DIRS[@]:-}"; do
      if [[ "${existing}" == "${libdir}" ]]; then
        return
      fi
    done
    QT_EXTRA_LIB_DIRS+=("${libdir}")
  fi
}

for qt_mod in qtpdf qtsvg qtvirtualkeyboard qtdeclarative qttools qtwebengine; do
  for brew_prefix in /opt/homebrew/opt /usr/local/opt; do
    add_qt_lib_dir "${brew_prefix}/${qt_mod}/lib"
  done
  for cellar_prefix in /opt/homebrew/Cellar /usr/local/Cellar; do
    if [[ -d "${cellar_prefix}/${qt_mod}" ]]; then
      latest_lib="$(ls -1dt "${cellar_prefix}/${qt_mod}"/*/lib 2>/dev/null | head -n1 || true)"
      if [[ -n "${latest_lib}" ]]; then
        add_qt_lib_dir "${latest_lib}"
      fi
    fi
  done
done

prepare_macdeployqt_staging() {
  local staging_dir="$1"
  mkdir -p "${staging_dir}"
  local framework_name framework_path libdir
  for framework_name in QtPdf.framework QtSvg.framework QtVirtualKeyboard.framework QtVirtualKeyboardQml.framework; do
    framework_path=""
    if [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/lib/${framework_name}" ]]; then
      framework_path="${QT_PREFIX}/lib/${framework_name}"
    else
      for libdir in "${QT_EXTRA_LIB_DIRS[@]:-}"; do
        if [[ -d "${libdir}/${framework_name}" ]]; then
          framework_path="${libdir}/${framework_name}"
          break
        fi
      done
    fi
    if [[ -n "${framework_path}" ]]; then
      ln -sfn "${framework_path}" "${staging_dir}/${framework_name}"
    fi
  done
}

if [[ ${#QT_EXTRA_LIB_DIRS[@]} -gt 0 ]]; then
  qt_extra_joined=""
  for libdir in "${QT_EXTRA_LIB_DIRS[@]}"; do
    if [[ -z "${qt_extra_joined}" ]]; then
      qt_extra_joined="${libdir}"
    else
      qt_extra_joined="${qt_extra_joined}:${libdir}"
    fi
  done
  if [[ -n "${qt_extra_joined}" ]]; then
    export DYLD_FRAMEWORK_PATH="${qt_extra_joined}:${DYLD_FRAMEWORK_PATH:-}"
    export DYLD_LIBRARY_PATH="${qt_extra_joined}:${DYLD_LIBRARY_PATH:-}"
  fi
fi

if [[ -d "/opt/homebrew/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/usr/local/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/usr/local/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
fi

cmake_cmd=(cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
  cmake_cmd+=("${EXTRA_CMAKE_ARGS[@]}")
fi
"${cmake_cmd[@]}"

# Leer versión real de CMake ya configurada (fuente de verdad).
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
fi
if [[ -z "${APP_VERSION}" && -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
  APP_VERSION="$(sed -n 's/.*VERSION[[:space:]]\\([0-9][0-9.]*\\).*/\\1/p' "${SOURCE_DIR}/CMakeLists.txt" | head -n1)"
fi
if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="0.9.7"
fi
BUNDLE_NAME="ZFSMgr-${APP_VERSION}"

cmake --build "${BUILD_DIR}" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Build completado: ${BUILD_DIR}/${BUNDLE_NAME}.app"
if [[ "${BUNDLE_APP}" -eq 1 ]]; then
  APP_BUNDLE="${BUILD_DIR}/${BUNDLE_NAME}.app"
  if [[ ! -d "${APP_BUNDLE}" ]]; then
    echo "Error: no se ha generado ${APP_BUNDLE}" >&2
    exit 1
  fi

  # Empaqueta frameworks/plugins de Qt dentro del .app (sin firma).
  if command -v macdeployqt >/dev/null 2>&1; then
    prepare_macdeployqt_staging "${BUILD_DIR}/lib"
    macdeployqt_args=("${APP_BUNDLE}" -always-overwrite)
    if [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/lib" ]]; then
      macdeployqt_args+=("-libpath=${QT_PREFIX}/lib")
    fi
    if [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/plugins" ]]; then
      macdeployqt_args+=("-plugindir=${QT_PREFIX}/plugins")
    fi
    if [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/qml" ]]; then
      macdeployqt_args+=("-qmldir=${PROJECT_ROOT}/src")
    fi
    if [[ ${#QT_EXTRA_LIB_DIRS[@]} -gt 0 ]]; then
      for libdir in "${QT_EXTRA_LIB_DIRS[@]}"; do
        macdeployqt_args+=("-libpath=${libdir}")
      done
    fi
    echo "macOS Qt deploy debug:"
    echo "  QT_PREFIX=${QT_PREFIX}"
    echo "  QT_PLUGIN_PATH=${QT_PLUGIN_PATH:-}"
    echo "  QML2_IMPORT_PATH=${QML2_IMPORT_PATH:-}"
    echo "  DYLD_FRAMEWORK_PATH=${DYLD_FRAMEWORK_PATH:-}"
    echo "  DYLD_LIBRARY_PATH=${DYLD_LIBRARY_PATH:-}"
    echo "  macdeployqt staging dir: ${BUILD_DIR}/lib"
    ls -1 "${BUILD_DIR}/lib" 2>/dev/null | sed 's/^/    * /' || true
    if [[ ${#QT_EXTRA_LIB_DIRS[@]} -gt 0 ]]; then
      echo "  QT_EXTRA_LIB_DIRS:"
      for libdir in "${QT_EXTRA_LIB_DIRS[@]}"; do
        echo "    - ${libdir}"
      done
    else
      echo "  QT_EXTRA_LIB_DIRS: (none)"
    fi
    printf '  macdeployqt command:'
    for arg in "${QT_PREFIX}/bin/macdeployqt" "${macdeployqt_args[@]}"; do
      printf ' %q' "${arg}"
    done
    printf '\n'
    "${QT_PREFIX}/bin/macdeployqt" "${macdeployqt_args[@]}"
  else
    echo "Aviso: macdeployqt no encontrado; el .app puede no ser portable fuera de este equipo."
  fi

  # Safety: never ship local connection secrets inside the macOS app bundle.
  find "${APP_BUNDLE}" -type f -name "connections.ini" -delete || true

  SHOULD_SIGN=0
  if [[ "${SIGN_APP_MODE}" == "yes" ]]; then
    SHOULD_SIGN=1
  elif [[ "${SIGN_APP_MODE}" == "no" ]]; then
    SHOULD_SIGN=0
  else
    # auto: en CI no firmar por defecto; en local, firmar solo si existe identidad.
    if [[ -n "${CI:-}" ]]; then
      SHOULD_SIGN=0
    elif has_codesign_identity "${SELF_SIGN_CERT_NAME}"; then
      SHOULD_SIGN=1
    else
      SHOULD_SIGN=0
    fi
  fi

  if [[ "${SHOULD_SIGN}" -eq 1 ]]; then
    ensure_codesign_identity "${SELF_SIGN_CERT_NAME}"
    prepare_codesign_keychain
    MAIN_BIN="${APP_BUNDLE}/Contents/MacOS/${BUNDLE_NAME}"
    echo "codesign debug:"
    security find-identity -v -p codesigning || true
    security show-keychain-info "${HOME}/Library/Keychains/login.keychain-db" || true
    /usr/bin/codesign --remove-signature "${MAIN_BIN}" >/dev/null 2>&1 || true
    /usr/bin/codesign --force --sign "${SELF_SIGN_CERT_NAME}" --timestamp=none -vvv "${MAIN_BIN}"
    /usr/bin/codesign --force --deep --sign "${SELF_SIGN_CERT_NAME}" --timestamp=none -vvv "${APP_BUNDLE}"
    /usr/bin/codesign --verify --strict --verbose=4 "${MAIN_BIN}"
    /usr/bin/codesign --verify --deep --strict --verbose=4 "${APP_BUNDLE}"
    echo "App macOS creada y firmada con certificado autofirmado: ${APP_BUNDLE}"
  else
    echo "App macOS creada sin firma: ${APP_BUNDLE}"
  fi

  if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
    upload_to_sftp "${APP_BUNDLE}"
  fi
else
  echo "Empaquetado .app omitido (usa --bundle para generarlo)."
fi
