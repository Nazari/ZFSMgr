#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-macos"
APP_VERSION="$(sed -n 's/^project(ZFSMgrQt VERSION \\([0-9.]*\\).*/\\1/p' "${SCRIPT_DIR}/CMakeLists.txt" | head -n1)"
if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="0.1.0"
fi
BUNDLE_NAME="ZFSMgr-${APP_VERSION}"
BUNDLE_APP=0
SELF_SIGN_CERT_NAME="${SELF_SIGN_CERT_NAME:-ZFSMgr Local Self-Signed}"
EXTRA_CMAKE_ARGS=()

for arg in "$@"; do
  if [[ "${arg}" == "--bundle" ]]; then
    BUNDLE_APP=1
  else
    EXTRA_CMAKE_ARGS+=("${arg}")
  fi
done

ensure_codesign_identity() {
  local cert_name="$1"
  if security find-identity -v -p codesigning | grep -F "\"${cert_name}\"" >/dev/null 2>&1; then
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
Luego vuelve a ejecutar: ./build-macos.sh --bundle
EOF
  exit 1
}

# Soporte Homebrew Apple Silicon e Intel.
if [[ -d "/opt/homebrew/opt/qt@6" ]]; then
  QT_PREFIX="/opt/homebrew/opt/qt@6"
elif [[ -d "/usr/local/opt/qt@6" ]]; then
  QT_PREFIX="/usr/local/opt/qt@6"
else
  QT_PREFIX=""
fi

if [[ -n "${QT_PREFIX}" ]]; then
  export PATH="${QT_PREFIX}/bin:${PATH}"
  export CMAKE_PREFIX_PATH="${QT_PREFIX}:${CMAKE_PREFIX_PATH:-}"
fi

if [[ -d "/opt/homebrew/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/usr/local/opt/openssl@3" ]]; then
  export CMAKE_PREFIX_PATH="/usr/local/opt/openssl@3:${CMAKE_PREFIX_PATH:-}"
fi

cmake_cmd=(cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
  cmake_cmd+=("${EXTRA_CMAKE_ARGS[@]}")
fi
"${cmake_cmd[@]}"
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
    macdeployqt "${APP_BUNDLE}" -always-overwrite
  else
    echo "Aviso: macdeployqt no encontrado; el .app puede no ser portable fuera de este equipo."
  fi

  ensure_codesign_identity "${SELF_SIGN_CERT_NAME}"
  MAIN_BIN="${APP_BUNDLE}/Contents/MacOS/${BUNDLE_NAME}"
  /usr/bin/codesign --remove-signature "${MAIN_BIN}" >/dev/null 2>&1 || true
  /usr/bin/codesign --force --sign "${SELF_SIGN_CERT_NAME}" --timestamp=none "${MAIN_BIN}"
  /usr/bin/codesign --force --deep --sign "${SELF_SIGN_CERT_NAME}" --timestamp=none "${APP_BUNDLE}"
  /usr/bin/codesign --verify --strict --verbose=4 "${MAIN_BIN}"
  /usr/bin/codesign --verify --deep --strict --verbose=4 "${APP_BUNDLE}"
  echo "App macOS creada y firmada con certificado autofirmado: ${APP_BUNDLE}"
else
  echo "Empaquetado .app omitido (usa --bundle para generarlo)."
fi
