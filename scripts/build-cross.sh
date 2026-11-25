#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=entorno.sh
. "${SCRIPT_DIR}/entorno.sh"
SOURCE_DIR="${PROJECT_ROOT}/resources"
TARGET=""
BUILD_TYPE="Release"
BUILD_DIR=""
JOBS="$(nproc 2>/dev/null || echo 4)"
RUN_DOCTOR=0
DO_CONFIGURE=1
DO_BUILD=1
# Only the agent. See --agent-only.
AGENT_ONLY=0
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
Uso:
  build-cross.sh --target <windows|freebsd|macos> [opciones] [-- <args extra de CMake>]

Opciones:
  --target <t>        Target de cross: windows, freebsd, macos
  --build-dir <dir>   Directorio de build (por defecto: builds/cross-<target>)
  --build-type <t>    Tipo de build CMake (por defecto: Release)
  --jobs <n>          Paralelismo para cmake --build (por defecto: nproc)
  --doctor            Solo valida prerequisitos del target
  --agent-only        Compila SOLO el agente (sin el CLI)
  --no-configure      No ejecuta cmake -S/-B
  --no-build          No ejecuta cmake --build
  -h, --help          Muestra esta ayuda

Toolchains:
  toolchains/windows-mingw64.cmake
  toolchains/freebsd-clang.cmake
  toolchains/macos-osxcross.cmake

Variables de entorno esperadas:
  Windows:
    OPENSSL_ROOT_DIR    (OpenSSL target MinGW; autodetecta <entorno>/opt/openssl-mingw64)
    CROSS_TRIPLE_WINDOWS (opcional, default x86_64-w64-mingw32)

  FreeBSD:
    FREEBSD_SYSROOT      (sysroot FreeBSD)
    FREEBSD_TRIPLE       (opcional, default x86_64-unknown-freebsd13)
    FREEBSD_CC/FREEBSD_CXX (opcionales)

  macOS (osxcross):
    OSXCROSS_TARGET      (opcional; si no se define se autodetecta desde /opt/osxcross/target/bin)
    OSX_SYSROOT          (SDK de macOS)
    MACOSX_DEPLOYMENT_TARGET (opcional; default 10.15, por std::filesystem)
    OPENSSL_ROOT_DIR     (opcional; autodetecta <entorno>/opt/openssl-macos-x86_64 o <entorno>/opt/openssl-macos-arm64)
    OSXCROSS_CC/OSXCROSS_CXX (opcionales)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      shift; [[ $# -gt 0 ]] || { echo "Falta valor para --target" >&2; exit 1; }
      TARGET="$1"
      shift
      ;;
    --build-dir)
      shift; [[ $# -gt 0 ]] || { echo "Falta valor para --build-dir" >&2; exit 1; }
      BUILD_DIR="$1"
      shift
      ;;
    --build-type)
      shift; [[ $# -gt 0 ]] || { echo "Falta valor para --build-type" >&2; exit 1; }
      BUILD_TYPE="$1"
      shift
      ;;
    --jobs)
      shift; [[ $# -gt 0 ]] || { echo "Falta valor para --jobs" >&2; exit 1; }
      JOBS="$1"
      shift
      ;;
    --doctor)
      RUN_DOCTOR=1
      shift
      ;;
    --agent-only)
      AGENT_ONLY=1
      shift
      ;;
    --no-configure)
      DO_CONFIGURE=0
      shift
      ;;
    --no-build)
      DO_BUILD=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        EXTRA_CMAKE_ARGS+=("$1")
        shift
      done
      ;;
    *)
      EXTRA_CMAKE_ARGS+=("$1")
      shift
      ;;
  esac
done

[[ -n "${TARGET}" ]] || { usage >&2; exit 1; }
case "${TARGET}" in
  windows|freebsd|macos) ;;
  *) echo "Target no soportado: ${TARGET}" >&2; exit 1 ;;
esac

if [[ -z "${BUILD_DIR}" ]]; then
  BUILD_DIR="${PROJECT_ROOT}/builds/cross-${TARGET}"
fi

case "${TARGET}" in
  windows) toolchain_file="${PROJECT_ROOT}/toolchains/windows-mingw64.cmake" ;;
  freebsd) toolchain_file="${PROJECT_ROOT}/toolchains/freebsd-clang.cmake" ;;
  macos) toolchain_file="${PROJECT_ROOT}/toolchains/macos-osxcross.cmake" ;;
esac

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Falta comando: $1" >&2; return 1; }
}

autodetect_path_glob() {
  local glob_pattern="$1"
  local found=""
  shopt -s nullglob
  local candidates=(${glob_pattern})
  shopt -u nullglob
  if [[ ${#candidates[@]} -gt 0 ]]; then
    found="$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n1)"
  fi
  echo "${found}"
}

autodetect_osxcross_target() {
  local found=""
  shopt -s nullglob
  local candidates=(/opt/osxcross/target/bin/*-apple-darwin*-clang)
  shopt -u nullglob
  if [[ ${#candidates[@]} -eq 0 ]]; then
    echo ""
    return 0
  fi

  local preferred=""
  preferred="$(printf '%s\n' "${candidates[@]}" | sed -E 's|.*/([^/]+)-clang$|\1|' | rg '^x86_64-apple-darwin' | sort -V | tail -n1 || true)"
  if [[ -z "${preferred}" ]]; then
    preferred="$(printf '%s\n' "${candidates[@]}" | sed -E 's|.*/([^/]+)-clang$|\1|' | sort -V | tail -n1)"
  fi
  found="${preferred}"
  echo "${found}"
}

macos_arch_from_target() {
  local target="${1:-}"
  case "${target}" in
    arm64-*|aarch64-*) echo "arm64" ;;
    x86_64-*) echo "x86_64" ;;
    *) echo "" ;;
  esac
}

autodetect_macos_openssl_prefix() {
  local arch=""
  arch="$(macos_arch_from_target "${OSXCROSS_TARGET:-}")"
  local arm_prefix="${ZFSMGR_ENTORNO}/opt/openssl-macos-arm64"
  local x86_prefix="${ZFSMGR_ENTORNO}/opt/openssl-macos-x86_64"

  if [[ "${arch}" == "arm64" ]]; then
    [[ -f "${arm_prefix}/include/openssl/evp.h" ]] && { echo "${arm_prefix}"; return 0; }
    [[ -f "${x86_prefix}/include/openssl/evp.h" ]] && { echo "${x86_prefix}"; return 0; }
  elif [[ "${arch}" == "x86_64" ]]; then
    [[ -f "${x86_prefix}/include/openssl/evp.h" ]] && { echo "${x86_prefix}"; return 0; }
    [[ -f "${arm_prefix}/include/openssl/evp.h" ]] && { echo "${arm_prefix}"; return 0; }
  else
    [[ -f "${x86_prefix}/include/openssl/evp.h" ]] && { echo "${x86_prefix}"; return 0; }
    [[ -f "${arm_prefix}/include/openssl/evp.h" ]] && { echo "${arm_prefix}"; return 0; }
  fi

  echo ""
}

doctor_windows() {
  local ok=0
  echo "[doctor] target=windows"
  need_cmd cmake || ok=1
  local triple="${CROSS_TRIPLE_WINDOWS:-x86_64-w64-mingw32}"
  need_cmd "${triple}-gcc" || ok=1
  need_cmd "${triple}-g++" || ok=1
  need_cmd "${triple}-windres" || ok=1
  if [[ -z "${OPENSSL_ROOT_DIR:-}" ]] && [[ -f "${ZFSMGR_ENTORNO}/opt/openssl-mingw64/lib/libcrypto.a" ]]; then
    OPENSSL_ROOT_DIR="${ZFSMGR_ENTORNO}/opt/openssl-mingw64"
    export OPENSSL_ROOT_DIR
    echo "OPENSSL_ROOT_DIR autodetectado: ${OPENSSL_ROOT_DIR}"
  fi
  return ${ok}
}

doctor_freebsd() {
  local ok=0
  echo "[doctor] target=freebsd"
  need_cmd cmake || ok=1
  need_cmd clang || ok=1
  need_cmd clang++ || ok=1
  if [[ -z "${FREEBSD_SYSROOT:-}" ]]; then
    FREEBSD_SYSROOT="$(autodetect_path_glob "${ZFSMGR_ENTORNO}/sysroots/freebsd*-amd64")"
    export FREEBSD_SYSROOT
    if [[ -n "${FREEBSD_SYSROOT}" ]]; then
      echo "FREEBSD_SYSROOT autodetectado: ${FREEBSD_SYSROOT}"
    fi
  fi
  [[ -n "${FREEBSD_SYSROOT:-}" ]] || { echo "Falta FREEBSD_SYSROOT" >&2; ok=1; }
  return ${ok}
}

doctor_macos() {
  local ok=0
  echo "[doctor] target=macos"
  need_cmd cmake || ok=1
  if [[ -d "/opt/osxcross/target/bin" ]] && [[ ":${PATH}:" != *":/opt/osxcross/target/bin:"* ]]; then
    export PATH="/opt/osxcross/target/bin:${PATH}"
    echo "PATH actualizado con /opt/osxcross/target/bin"
  fi
  if [[ -z "${OSXCROSS_TARGET:-}" ]]; then
    OSXCROSS_TARGET="$(autodetect_osxcross_target)"
    if [[ -n "${OSXCROSS_TARGET}" ]]; then
      echo "OSXCROSS_TARGET autodetectado: ${OSXCROSS_TARGET}"
    fi
    export OSXCROSS_TARGET
  fi
  if [[ -z "${OSX_SYSROOT:-}" ]]; then
    local sdk_guess
    sdk_guess="$(ls -d /opt/osxcross/target/SDK/MacOSX*.sdk 2>/dev/null | sort -V | tail -n1 || true)"
    if [[ -n "${sdk_guess}" ]]; then
      OSX_SYSROOT="${sdk_guess}"
      export OSX_SYSROOT
      echo "OSX_SYSROOT autodetectado: ${OSX_SYSROOT}"
    fi
  fi
  if [[ -z "${OPENSSL_ROOT_DIR:-}" ]]; then
    OPENSSL_ROOT_DIR="$(autodetect_macos_openssl_prefix)"
    if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
      export OPENSSL_ROOT_DIR
      echo "OPENSSL_ROOT_DIR autodetectado: ${OPENSSL_ROOT_DIR}"
    fi
  fi
  local cc="${OSXCROSS_CC:-${OSXCROSS_TARGET:-}-clang}"
  local cxx="${OSXCROSS_CXX:-${OSXCROSS_TARGET:-}-clang++}"
  [[ -n "${OSXCROSS_TARGET:-}" ]] || { echo "Falta OSXCROSS_TARGET (no se pudo autodetectar)" >&2; ok=1; }
  need_cmd "${cc}" || ok=1
  need_cmd "${cxx}" || ok=1
  [[ -n "${OSX_SYSROOT:-}" ]] || { echo "Falta OSX_SYSROOT" >&2; ok=1; }
  [[ -n "${OPENSSL_ROOT_DIR:-}" ]] || { echo "Falta OPENSSL_ROOT_DIR (OpenSSL target macOS)" >&2; ok=1; }
  if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
    [[ -f "${OPENSSL_ROOT_DIR}/include/openssl/evp.h" ]] || { echo "OPENSSL_ROOT_DIR inválido (falta include/openssl/evp.h): ${OPENSSL_ROOT_DIR}" >&2; ok=1; }
    [[ -f "${OPENSSL_ROOT_DIR}/lib/libcrypto.a" || -f "${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib" ]] || { echo "OPENSSL_ROOT_DIR inválido (falta libcrypto en lib/): ${OPENSSL_ROOT_DIR}" >&2; ok=1; }
  fi
  return ${ok}
}

if [[ "${TARGET}" == "windows" ]]; then
  doctor_windows || { echo "Doctor FALLÓ para windows" >&2; exit 2; }
elif [[ "${TARGET}" == "freebsd" ]]; then
  doctor_freebsd || { echo "Doctor FALLÓ para freebsd" >&2; exit 2; }
else
  doctor_macos || { echo "Doctor FALLÓ para macos" >&2; exit 2; }
fi

if [[ "${RUN_DOCTOR}" -eq 1 ]]; then
  echo "Doctor OK para ${TARGET}"
  exit 0
fi

if [[ "${DO_CONFIGURE}" -eq 1 ]]; then
  cmake_generator_args=()
  target_extra_args=()
  common_extra_args=(-DBUILD_TESTING=OFF)
  if command -v ninja >/dev/null 2>&1; then
    cmake_generator_args=(-G Ninja)
  fi
  if [[ "${TARGET}" == "freebsd" ]]; then
    # Link the daemon agent against static libssl/libcrypto so the binary
    # works on any FreeBSD version regardless of the base system SSL soname
    # (FreeBSD 13 ships libssl.so.111 / LibreSSL; FreeBSD 14+ changed soname).
    if [[ -n "${FREEBSD_SYSROOT:-}" ]]; then
      fbsd_ssl_a="${FREEBSD_SYSROOT}/usr/lib/libssl.a"
      fbsd_crypto_a="${FREEBSD_SYSROOT}/usr/lib/libcrypto.a"
      if [[ -f "${fbsd_ssl_a}" ]] && [[ -f "${fbsd_crypto_a}" ]]; then
        target_extra_args+=("-DOPENSSL_SSL_LIBRARY=${fbsd_ssl_a}")
        target_extra_args+=("-DOPENSSL_CRYPTO_LIBRARY=${fbsd_crypto_a}")
      fi
    fi
  elif [[ "${TARGET}" == "macos" ]]; then
    if [[ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
      MACOSX_DEPLOYMENT_TARGET="10.15"
      export MACOSX_DEPLOYMENT_TARGET
    fi
    if [[ -z "${OPENSSL_ROOT_DIR:-}" ]]; then
      OPENSSL_ROOT_DIR="$(autodetect_macos_openssl_prefix)"
      [[ -n "${OPENSSL_ROOT_DIR:-}" ]] && export OPENSSL_ROOT_DIR
    fi
    if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
      target_extra_args+=("-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
      [[ -d "${OPENSSL_ROOT_DIR}/include" ]] && target_extra_args+=("-DOPENSSL_INCLUDE_DIR=${OPENSSL_ROOT_DIR}/include")
      [[ -f "${OPENSSL_ROOT_DIR}/lib/libcrypto.a" ]] && target_extra_args+=("-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
      [[ -f "${OPENSSL_ROOT_DIR}/lib/libssl.a" ]] && target_extra_args+=("-DOPENSSL_SSL_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libssl.a")
      [[ -f "${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib" ]] && target_extra_args+=("-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib")
      [[ -f "${OPENSSL_ROOT_DIR}/lib/libssl.dylib" ]] && target_extra_args+=("-DOPENSSL_SSL_LIBRARY=${OPENSSL_ROOT_DIR}/lib/libssl.dylib")
      [[ -d "${OPENSSL_ROOT_DIR}/lib/pkgconfig" ]] && target_extra_args+=("-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=TRUE")
    fi
    target_extra_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET}")
  fi
  # Always explicit: a cached ON from an earlier pass would otherwise survive.
  if [[ "${AGENT_ONLY}" -eq 1 ]]; then
    target_extra_args+=("-DZFSMGR_AGENT_ONLY=ON")
  else
    target_extra_args+=("-DZFSMGR_AGENT_ONLY=OFF")
  fi
  cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    "${cmake_generator_args[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}" \
    "${common_extra_args[@]}" \
    "${target_extra_args[@]}" \
    "${EXTRA_CMAKE_ARGS[@]}"
fi

if [[ "${DO_BUILD}" -eq 1 ]]; then
  cmake --build "${BUILD_DIR}" -j"${JOBS}"
fi

echo "Cross build finalizado: ${TARGET} -> ${BUILD_DIR}"
