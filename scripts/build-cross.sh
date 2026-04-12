#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_DIR="${PROJECT_ROOT}/resources"
TARGET=""
BUILD_TYPE="Release"
BUILD_DIR=""
JOBS="$(nproc 2>/dev/null || echo 4)"
RUN_DOCTOR=0
DO_CONFIGURE=1
DO_BUILD=1
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
Uso:
  build-cross.sh --target <windows|freebsd|macos> [opciones] [-- <args extra de CMake>]

Opciones:
  --target <t>        Target de cross: windows, freebsd, macos
  --build-dir <dir>   Directorio de build (por defecto: build-cross-<target>)
  --build-type <t>    Tipo de build CMake (por defecto: Release)
  --jobs <n>          Paralelismo para cmake --build (por defecto: nproc)
  --doctor            Solo valida prerequisitos del target
  --no-configure      No ejecuta cmake -S/-B
  --no-build          No ejecuta cmake --build
  -h, --help          Muestra esta ayuda

Toolchains:
  toolchains/windows-mingw64.cmake
  toolchains/freebsd-clang.cmake
  toolchains/macos-osxcross.cmake

Variables de entorno esperadas:
  Windows:
    QT6_WINDOWS_PREFIX  (ruta de Qt6 para MinGW target)
                       (si no se define, se intenta autodetectar en ~/Qt/*/mingw_64)
    QT6_HOST_PREFIX     (Qt6 host, por defecto /usr/lib/x86_64-linux-gnu/cmake)
    OPENSSL_ROOT_DIR    (OpenSSL target MinGW; autodetecta ~/opt/openssl-mingw64)
    CROSS_TRIPLE_WINDOWS (opcional, default x86_64-w64-mingw32)

  FreeBSD:
    FREEBSD_SYSROOT      (sysroot FreeBSD)
    QT6_FREEBSD_PREFIX   (Qt6 para target FreeBSD)
    FREEBSD_TRIPLE       (opcional, default x86_64-unknown-freebsd13)
    FREEBSD_CC/FREEBSD_CXX (opcionales)

  macOS (osxcross):
    OSXCROSS_TARGET      (default x86_64-apple-darwin23)
    OSX_SYSROOT          (SDK de macOS)
    QT6_MACOS_PREFIX     (Qt6 para target macOS)
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
  BUILD_DIR="${PROJECT_ROOT}/build-cross-${TARGET}"
fi

case "${TARGET}" in
  windows) toolchain_file="${PROJECT_ROOT}/toolchains/windows-mingw64.cmake" ;;
  freebsd) toolchain_file="${PROJECT_ROOT}/toolchains/freebsd-clang.cmake" ;;
  macos) toolchain_file="${PROJECT_ROOT}/toolchains/macos-osxcross.cmake" ;;
esac

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Falta comando: $1" >&2; return 1; }
}

autodetect_qt_prefix() {
  local pattern="$1"
  local found=""
  shopt -s nullglob
  local candidates=("${HOME}"/Qt/${pattern})
  shopt -u nullglob
  if [[ ${#candidates[@]} -gt 0 ]]; then
    found="$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n1)"
  fi
  echo "${found}"
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

doctor_windows() {
  local ok=0
  echo "[doctor] target=windows"
  need_cmd cmake || ok=1
  local triple="${CROSS_TRIPLE_WINDOWS:-x86_64-w64-mingw32}"
  need_cmd "${triple}-gcc" || ok=1
  need_cmd "${triple}-g++" || ok=1
  need_cmd "${triple}-windres" || ok=1
  if [[ -z "${QT6_WINDOWS_PREFIX:-}" ]]; then
    QT6_WINDOWS_PREFIX="$(autodetect_qt_prefix '*/mingw_64')"
    export QT6_WINDOWS_PREFIX
    if [[ -n "${QT6_WINDOWS_PREFIX}" ]]; then
      echo "QT6_WINDOWS_PREFIX autodetectado: ${QT6_WINDOWS_PREFIX}"
    fi
  fi
  if [[ -z "${QT6_WINDOWS_PREFIX:-}" ]]; then
    echo "Falta QT6_WINDOWS_PREFIX" >&2
    ok=1
  fi
  if [[ -z "${QT6_HOST_PREFIX:-}" ]] && [[ -d "/usr/lib/x86_64-linux-gnu/cmake/Qt6" ]]; then
    QT6_HOST_PREFIX="/usr/lib/x86_64-linux-gnu/cmake"
    export QT6_HOST_PREFIX
    echo "QT6_HOST_PREFIX autodetectado: ${QT6_HOST_PREFIX}"
  fi
  if [[ -z "${OPENSSL_ROOT_DIR:-}" ]] && [[ -f "${HOME}/opt/openssl-mingw64/lib/libcrypto.a" ]]; then
    OPENSSL_ROOT_DIR="${HOME}/opt/openssl-mingw64"
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
    FREEBSD_SYSROOT="$(autodetect_path_glob "${HOME}/sysroots/freebsd*-amd64")"
    export FREEBSD_SYSROOT
    if [[ -n "${FREEBSD_SYSROOT}" ]]; then
      echo "FREEBSD_SYSROOT autodetectado: ${FREEBSD_SYSROOT}"
    fi
  fi
  if [[ -z "${QT6_FREEBSD_PREFIX:-}" ]] && [[ -n "${FREEBSD_SYSROOT:-}" ]] && [[ -f "${FREEBSD_SYSROOT}/usr/local/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    QT6_FREEBSD_PREFIX="${FREEBSD_SYSROOT}/usr/local"
    export QT6_FREEBSD_PREFIX
    echo "QT6_FREEBSD_PREFIX autodetectado: ${QT6_FREEBSD_PREFIX}"
  fi
  [[ -n "${FREEBSD_SYSROOT:-}" ]] || { echo "Falta FREEBSD_SYSROOT" >&2; ok=1; }
  [[ -n "${QT6_FREEBSD_PREFIX:-}" ]] || { echo "Falta QT6_FREEBSD_PREFIX" >&2; ok=1; }
  if [[ -n "${QT6_FREEBSD_PREFIX:-}" ]] && [[ ! -f "${QT6_FREEBSD_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
    echo "QT6_FREEBSD_PREFIX no parece válido (falta Qt6Config.cmake): ${QT6_FREEBSD_PREFIX}" >&2
    ok=1
  fi
  return ${ok}
}

doctor_macos() {
  local ok=0
  echo "[doctor] target=macos"
  need_cmd cmake || ok=1
  if [[ -z "${OSXCROSS_TARGET:-}" ]] && [[ -x "/opt/osxcross/target/bin/x86_64-apple-darwin23-clang" ]]; then
    OSXCROSS_TARGET="x86_64-apple-darwin23"
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
  local cc="${OSXCROSS_CC:-${OSXCROSS_TARGET:-x86_64-apple-darwin23}-clang}"
  local cxx="${OSXCROSS_CXX:-${OSXCROSS_TARGET:-x86_64-apple-darwin23}-clang++}"
  need_cmd "${cc}" || ok=1
  need_cmd "${cxx}" || ok=1
  [[ -n "${OSX_SYSROOT:-}" ]] || { echo "Falta OSX_SYSROOT" >&2; ok=1; }
  [[ -n "${QT6_MACOS_PREFIX:-}" ]] || { echo "Falta QT6_MACOS_PREFIX" >&2; ok=1; }
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
  if [[ "${TARGET}" == "windows" ]]; then
    if [[ -z "${QT_HOST_PATH:-}" ]] && [[ -n "${QT6_WINDOWS_PREFIX:-}" ]]; then
      qt_ver_dir="$(dirname "${QT6_WINDOWS_PREFIX}")"
      if [[ -d "${qt_ver_dir}/gcc_64" ]]; then
        QT_HOST_PATH="${qt_ver_dir}/gcc_64"
        export QT_HOST_PATH
      fi
    fi
    if [[ -z "${QT_HOST_PATH:-}" ]] && [[ -d "/usr/lib/qt6" ]]; then
      QT_HOST_PATH="/usr"
      export QT_HOST_PATH
    fi
    if [[ -z "${QT_HOST_PATH_CMAKE_DIR:-}" ]] && [[ -n "${QT_HOST_PATH:-}" ]]; then
      if [[ -d "${QT_HOST_PATH}/lib/cmake/Qt6" ]]; then
        QT_HOST_PATH_CMAKE_DIR="${QT_HOST_PATH}/lib/cmake/Qt6"
        export QT_HOST_PATH_CMAKE_DIR
      elif [[ -d "/usr/lib/x86_64-linux-gnu/cmake/Qt6" ]]; then
        QT_HOST_PATH_CMAKE_DIR="/usr/lib/x86_64-linux-gnu/cmake/Qt6"
        export QT_HOST_PATH_CMAKE_DIR
      fi
    fi
    [[ -n "${QT_HOST_PATH:-}" ]] && target_extra_args+=("-DQT_HOST_PATH=${QT_HOST_PATH}")
    [[ -n "${QT_HOST_PATH_CMAKE_DIR:-}" ]] && target_extra_args+=("-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}")
    target_extra_args+=("-DCMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders=TRUE")
  elif [[ "${TARGET}" == "freebsd" ]]; then
    if [[ -z "${QT_HOST_PATH:-}" ]]; then
      qt_host_guess="$(autodetect_qt_prefix '*/gcc_64')"
      if [[ -n "${qt_host_guess}" ]]; then
        QT_HOST_PATH="${qt_host_guess}"
        export QT_HOST_PATH
      fi
    fi
    if [[ -z "${QT_HOST_PATH_CMAKE_DIR:-}" ]] && [[ -n "${QT_HOST_PATH:-}" ]] && [[ -d "${QT_HOST_PATH}/lib/cmake/Qt6" ]]; then
      QT_HOST_PATH_CMAKE_DIR="${QT_HOST_PATH}/lib/cmake/Qt6"
      export QT_HOST_PATH_CMAKE_DIR
    fi
    [[ -n "${QT_HOST_PATH:-}" ]] && target_extra_args+=("-DQT_HOST_PATH=${QT_HOST_PATH}")
    [[ -n "${QT_HOST_PATH_CMAKE_DIR:-}" ]] && target_extra_args+=("-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}")
    target_extra_args+=("-DCMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders=TRUE")
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
