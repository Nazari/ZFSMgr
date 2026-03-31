#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-macos"
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}}"
DOWNLOADS_DIR="${DOWNLOADS_DIR:-${HOME}/Downloads/z}"

create_macos_dmg() {
  local app_path="$1"
  local app_name dmg_name staging_dir
  app_name="$(basename "${app_path}")"
  dmg_name="${app_name%.app}.dmg"
  staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfsmgr-dmg.XXXXXX")"
  (
    cp -R "${app_path}" "${staging_dir}/${app_name}"
    ln -s /Applications "${staging_dir}/Applications"
    rm -f "${BUILD_DIR}/${dmg_name}"
    hdiutil create \
      -quiet \
      -volname "${app_name%.app}" \
      -srcfolder "${staging_dir}" \
      -format UDZO \
      "${BUILD_DIR}/${dmg_name}"
  )
  rm -rf "${staging_dir}"
}
OUTPUT_DIR="${OUTPUT_DIR:-${BUILD_DIR}}"
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
MAC_ARCH="$(uname -m)"

usage() {
  cat <<'EOF'
Uso:
  build-macos.sh [opciones] [-- <args extra de CMake>]

Opciones:
  --bundle       Genera el bundle .app (por defecto)
  --no-bundle    Compila sin empaquetar el bundle final
  --sign         Fuerza la firma del bundle
  --no-sign      Desactiva la firma del bundle
  --sftpfc16     Sube el artefacto final (.dmg) al destino SFTP configurado
  -h, --help     Muestra esta ayuda

Variables opcionales:
  SELF_SIGN_CERT_NAME  Nombre del certificado local
  KEYCHAIN_PASSWORD    Password del llavero para codesign
  ZFSMGR_SFTP_TARGET   Destino SFTP para --sftpfc16

Ejemplos:
  ./scripts/build-macos.sh
  ./scripts/build-macos.sh --bundle --no-sign
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

if [[ "${UPLOAD_SFTP}" -eq 1 && "${BUNDLE_APP}" -eq 0 ]]; then
  echo "Error: --sftpfc16 requiere que se genere el bundle (.app)." >&2
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

create_macos_dmg() {
  local app_path="$1"
  local dmg_path="$2"
  local arch="$3"
  local app_name volume staging_dir hdi_rc final_dmg
  app_name="$(basename "${app_path}")"
  volume="${app_name%.app}"
  staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/zfsmgr-dmg.XXXXXX")"
  trap 'rm -rf "${staging_dir}"' RETURN
  cp -R "${app_path}" "${staging_dir}/${app_name}"
  ln -s /Applications "${staging_dir}/Applications"
  rm -f "${dmg_path}"
  set +e
  hdiutil create \
    -quiet \
    -ov \
    -volname "${volume}" \
    -srcfolder "${staging_dir}" \
    -format UDZO \
    "${dmg_path}"
  hdi_rc=$?
  set -e
  trap - RETURN
  rm -rf "${staging_dir}"
  if [[ ${hdi_rc} -ne 0 ]]; then
    echo "Error: hdiutil falló al crear ${dmg_path}" >&2
    exit "${hdi_rc}"
  fi
  final_dmg="${dmg_path%.dmg}-${arch}.dmg"
  rm -f "${final_dmg}"
  mv "${dmg_path}" "${final_dmg}"
  printf '%s\n' "${final_dmg}"
}

OPENSSL_PREFIX=""

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
    security set-key-partition-list \
      -S apple-tool:,apple:,codesign: \
      -s \
      -k "${KEYCHAIN_PASSWORD}" \
      "${keychain_path}" >/dev/null 2>&1 || true
  fi
  security set-keychain-settings -lut 7200 "${keychain_path}" >/dev/null 2>&1 || true
}

codesign_path() {
  local path="$1"
  local cert_name="$2"
  [[ -e "${path}" ]] || return 0
  /usr/bin/codesign --remove-signature "${path}" >/dev/null 2>&1 || true
  /usr/bin/codesign --force --sign "${cert_name}" --timestamp=none -vvv "${path}"
}

codesign_bundle_contents() {
  local app_bundle="$1"
  local cert_name="$2"
  local main_bin="$3"
  local frameworks_dir="${app_bundle}/Contents/Frameworks"
  local plugins_dir="${app_bundle}/Contents/PlugIns"
  local file

  if [[ -d "${frameworks_dir}" ]]; then
    while IFS= read -r -d '' file; do
      codesign_path "${file}" "${cert_name}"
    done < <(find "${frameworks_dir}" -type f \( -name "*.dylib" -o -perm -111 \) -print0)

    while IFS= read -r -d '' file; do
      codesign_path "${file}" "${cert_name}"
    done < <(find "${frameworks_dir}" -type d -name "*.framework" -print0)
  fi

  if [[ -d "${plugins_dir}" ]]; then
    while IFS= read -r -d '' file; do
      codesign_path "${file}" "${cert_name}"
    done < <(find "${plugins_dir}" -type f \( -name "*.dylib" -o -perm -111 \) -print0)
  fi

  codesign_path "${main_bin}" "${cert_name}"
  codesign_path "${app_bundle}" "${cert_name}"
}

# Soporte Homebrew Apple Silicon e Intel, y Qt instalado manualmente.
QT_PREFIX=""
for candidate in \
  "/opt/homebrew/opt/qt" \
  "/usr/local/opt/qt" \
  "/opt/homebrew/opt/qt@6" \
  "/usr/local/opt/qt@6"; do
  if [[ -d "${candidate}" ]]; then
    QT_PREFIX="${candidate}"
    break
  fi
done

if [[ -z "${QT_PREFIX}" && -d "/Users/linarese/Qt" ]]; then
  latest_qt_macos="$(find /Users/linarese/Qt -maxdepth 2 -type d -path '/Users/linarese/Qt/*/macos' | sort -V | tail -n1 || true)"
  if [[ -n "${latest_qt_macos}" ]]; then
    QT_PREFIX="${latest_qt_macos}"
  fi
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
EXTRA_LIB_SEARCH_DIRS=()
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

add_extra_lib_search_dir() {
  local libdir="$1"
  if [[ -d "${libdir}" ]]; then
    local existing
    for existing in "${EXTRA_LIB_SEARCH_DIRS[@]:-}"; do
      if [[ "${existing}" == "${libdir}" ]]; then
        return
      fi
    done
    EXTRA_LIB_SEARCH_DIRS+=("${libdir}")
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

for brew_libdir in /opt/homebrew/lib /usr/local/lib; do
  add_extra_lib_search_dir "${brew_libdir}"
done

framework_bundle_root_from_path() {
  local path="$1"
  if [[ "${path}" == *.framework ]]; then
    echo "${path}"
    return 0
  fi
  if [[ "${path}" == *".framework/"* ]]; then
    echo "${path%%.framework/*}.framework"
    return 0
  fi
  return 1
}

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

copy_framework_bundle() {
  local framework_src="$1"
  local frameworks_dst="$2"
  local framework_name resolved_framework_src current_link
  framework_name="$(basename "${framework_src}")"
  if [[ -L "${framework_src}" ]]; then
    resolved_framework_src="$(cd "${framework_src}" && pwd -P)"
  else
    resolved_framework_src="${framework_src}"
  fi
  mkdir -p "${frameworks_dst}"
  echo "  copy framework: ${resolved_framework_src} -> ${frameworks_dst}/${framework_name}"
  rm -rf "${frameworks_dst:?}/${framework_name}"
  cp -R "${resolved_framework_src}" "${frameworks_dst}/${framework_name}"
  current_link="${frameworks_dst}/${framework_name}/${framework_name%.*}"
  if [[ ! -e "${current_link}" ]]; then
    local version_dir
    version_dir="$(find "${frameworks_dst}/${framework_name}/Versions" -mindepth 1 -maxdepth 1 -type d | sort | head -n1 || true)"
    if [[ -n "${version_dir}" ]]; then
      ln -sfn "Versions/$(basename "${version_dir}")/${framework_name%.*}" "${current_link}"
    fi
  fi
}

resolve_dep_path() {
  local dep="$1"
  local source_file="$2"
  local source_dir source_framework_dir candidate libdir dep_basename
  source_dir="$(cd "$(dirname "${source_file}")" && pwd)"
  source_framework_dir="$(cd "${source_dir}/../Frameworks" 2>/dev/null && pwd || true)"
  dep_basename="$(basename "${dep}")"

  if [[ "${dep}" == @executable_path/* ]]; then
    candidate="${APP_BUNDLE}/Contents/MacOS/${dep#@executable_path/}"
    [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    if [[ -n "${QT_PREFIX}" ]]; then
      candidate="${QT_PREFIX}/lib/${dep_basename}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    fi
    if [[ -n "${OPENSSL_PREFIX}" ]]; then
      candidate="${OPENSSL_PREFIX}/lib/${dep_basename}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    fi
    for libdir in "${QT_EXTRA_LIB_DIRS[@]:-}"; do
      candidate="${libdir}/${dep_basename}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    done
    for libdir in "${EXTRA_LIB_SEARCH_DIRS[@]:-}"; do
      candidate="${libdir}/${dep_basename}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    done
  fi
  if [[ "${dep}" == @loader_path/* ]]; then
    candidate="${source_dir}/${dep#@loader_path/}"
    [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
  fi
  if [[ "${dep}" == @rpath/* ]]; then
    candidate="${APP_BUNDLE}/Contents/Frameworks/${dep#@rpath/}"
    [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    if [[ -n "${source_framework_dir}" ]]; then
      candidate="${source_framework_dir}/${dep#@rpath/}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    fi
    if [[ -n "${QT_PREFIX}" ]]; then
      candidate="${QT_PREFIX}/lib/${dep#@rpath/}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    fi
    for libdir in "${QT_EXTRA_LIB_DIRS[@]:-}"; do
      candidate="${libdir}/${dep#@rpath/}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    done
    for libdir in "${EXTRA_LIB_SEARCH_DIRS[@]:-}"; do
      candidate="${libdir}/${dep#@rpath/}"
      [[ -e "${candidate}" ]] && { echo "${candidate}"; return 0; }
    done
  fi
  if [[ -e "${dep}" ]]; then
    echo "${dep}"
    return 0
  fi
  return 1
}

copy_binary_or_dylib() {
  local src="$1"
  local frameworks_dst="$2"
  local name
  name="$(basename "${src}")"
  mkdir -p "${frameworks_dst}"
  echo "  copy dylib: ${src} -> ${frameworks_dst}/${name}"
  cp -f "${src}" "${frameworks_dst}/${name}"
  chmod 755 "${frameworks_dst}/${name}" || true
}

fix_install_names() {
  local target="$1"
  local dep resolved dep_name framework_root framework_name framework_bin new_ref
  while IFS= read -r dep; do
    dep="$(echo "${dep}" | sed 's/^[[:space:]]*//; s/ (.*$//')"
    [[ -z "${dep}" ]] && continue
    [[ "${dep}" == /System/* || "${dep}" == /usr/lib/* ]] && continue
    if ! resolved="$(resolve_dep_path "${dep}" "${target}")"; then
      continue
    fi
    dep_name="$(basename "${resolved}")"
    if framework_root="$(framework_bundle_root_from_path "${resolved}")"; then
      framework_name="$(basename "${framework_root}")"
      framework_bin="${framework_name%.*}"
      new_ref="@executable_path/../Frameworks/${framework_name}/Versions/A/${framework_bin}"
    else
      new_ref="@executable_path/../Frameworks/${dep_name}"
    fi
    install_name_tool -change "${dep}" "${new_ref}" "${target}" >/dev/null 2>&1 || true
  done < <(otool -L "${target}" | tail -n +2)
}

manual_deploy_bundle() {
  local main_bin="$1"
  local frameworks_dst="${APP_BUNDLE}/Contents/Frameworks"
  local plugins_dst="${APP_BUNDLE}/Contents/PlugIns"
  local queue=("${main_bin}")
  local seen=""
  mkdir -p "${frameworks_dst}"
  mkdir -p "${plugins_dst}"
  while [[ ${#queue[@]} -gt 0 ]]; do
    local current="${queue[0]}"
    queue=("${queue[@]:1}")
    if [[ "|${seen}|" == *"|${current}|"* ]]; then
      continue
    fi
    seen="${seen}|${current}"
    while IFS= read -r dep; do
      dep="$(echo "${dep}" | sed 's/^[[:space:]]*//; s/ (.*$//')"
      [[ -z "${dep}" ]] && continue
      [[ "${dep}" == /System/* || "${dep}" == /usr/lib/* ]] && continue
      local resolved dep_target framework_name framework_bin
      if ! resolved="$(resolve_dep_path "${dep}" "${current}")"; then
        echo "Aviso: dependencia no resuelta: ${dep}" >&2
        continue
      fi
      if [[ "${resolved}" == *.framework ]] || [[ "${resolved}" == *.framework/* ]]; then
        local framework_root
        framework_root="${resolved%%.framework*}.framework"
        framework_name="$(basename "${framework_root}")"
        framework_bin="${framework_name%.*}"
        dep_target="${frameworks_dst}/${framework_name}/Versions/A/${framework_bin}"
        if [[ ! -e "${dep_target}" ]]; then
          copy_framework_bundle "${framework_root}" "${frameworks_dst}"
        fi
      else
        dep_target="${frameworks_dst}/$(basename "${resolved}")"
        if [[ ! -e "${dep_target}" ]]; then
          copy_binary_or_dylib "${resolved}" "${frameworks_dst}"
        fi
      fi
      [[ -e "${dep_target}" ]] && queue+=("${dep_target}")
    done < <(otool -L "${current}" | tail -n +2)
  done

  while IFS= read -r file; do
    fix_install_names "${file}"
  done < <(find "${frameworks_dst}" -type f \( -perm -111 -o -name "*.dylib" \))
  while IFS= read -r file; do
    fix_install_names "${file}"
  done < <(find "${plugins_dst}" -type f \( -perm -111 -o -name "*.dylib" \))
  fix_install_names "${main_bin}"
}

copy_qt_plugin_dir() {
  local dir_name="$1"
  local src=""
  local dst="${APP_BUNDLE}/Contents/PlugIns/${dir_name}"
  if [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/share/qt/plugins/${dir_name}" ]]; then
    src="${QT_PREFIX}/share/qt/plugins/${dir_name}"
  elif [[ -n "${QT_PREFIX}" && -d "${QT_PREFIX}/plugins/${dir_name}" ]]; then
    src="${QT_PREFIX}/plugins/${dir_name}"
  fi
  if [[ -z "${src}" ]]; then
    for plugin_root in /opt/homebrew/share/qt/plugins /usr/local/share/qt/plugins /Users/linarese/Qt/*/macos/plugins; do
      if [[ -d "${plugin_root}/${dir_name}" ]]; then
        src="${plugin_root}/${dir_name}"
        break
      fi
    done
  fi
  if [[ -z "${src}" ]]; then
    echo "Aviso: no se encontró el directorio de plugins Qt '${dir_name}'." >&2
    return 0
  fi
  mkdir -p "${dst}"
  cp -RL "${src}/." "${dst}/"
}

write_qt_conf() {
  local qt_conf="${APP_BUNDLE}/Contents/Resources/qt.conf"
  cat >"${qt_conf}" <<'EOF'
[Paths]
Plugins = PlugIns
EOF
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
  OPENSSL_PREFIX="/opt/homebrew/opt/openssl@3"
  export CMAKE_PREFIX_PATH="${OPENSSL_PREFIX}:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/usr/local/opt/openssl@3" ]]; then
  OPENSSL_PREFIX="/usr/local/opt/openssl@3"
  export CMAKE_PREFIX_PATH="${OPENSSL_PREFIX}:${CMAKE_PREFIX_PATH:-}"
fi

cmake_cmd=(cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
  cmake_cmd+=("${EXTRA_CMAKE_ARGS[@]}")
fi
"${cmake_cmd[@]}"

APP_VERSION="$(resolve_app_version)"
BUNDLE_NAME="ZFSMgr-${APP_VERSION}"

if [[ "${BUNDLE_APP}" -eq 1 && -d "${BUILD_DIR}/${BUNDLE_NAME}.app" ]]; then
  # El deploy manual reescribe install_names dentro del bundle; borrar la app fuerza un relink limpio.
  rm -rf "${BUILD_DIR}/${BUNDLE_NAME}.app"
fi

cmake --build "${BUILD_DIR}" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Build completado: ${BUILD_DIR}/${BUNDLE_NAME}.app"
if [[ "${BUNDLE_APP}" -eq 1 ]]; then
  APP_BUNDLE="${BUILD_DIR}/${BUNDLE_NAME}.app"
  if [[ ! -d "${APP_BUNDLE}" ]]; then
    echo "Error: no se ha generado ${APP_BUNDLE}" >&2
    exit 1
  fi

  MAIN_BIN="${APP_BUNDLE}/Contents/MacOS/${BUNDLE_NAME}"
  echo "macOS manual deploy debug:"
  echo "  QT_PREFIX=${QT_PREFIX}"
  echo "  QT_PLUGIN_PATH=${QT_PLUGIN_PATH:-}"
  echo "  QML2_IMPORT_PATH=${QML2_IMPORT_PATH:-}"
  echo "  DYLD_FRAMEWORK_PATH=${DYLD_FRAMEWORK_PATH:-}"
  echo "  DYLD_LIBRARY_PATH=${DYLD_LIBRARY_PATH:-}"
  if [[ ${#QT_EXTRA_LIB_DIRS[@]} -gt 0 ]]; then
    echo "  QT_EXTRA_LIB_DIRS:"
    for libdir in "${QT_EXTRA_LIB_DIRS[@]}"; do
      echo "    - ${libdir}"
    done
  else
    echo "  QT_EXTRA_LIB_DIRS: (none)"
  fi
  manual_deploy_bundle "${MAIN_BIN}"
  copy_qt_plugin_dir "platforms"
  copy_qt_plugin_dir "styles"
  copy_qt_plugin_dir "imageformats"
  copy_qt_plugin_dir "iconengines"
  copy_qt_plugin_dir "networkinformation"
  copy_qt_plugin_dir "tls"
  write_qt_conf

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
    echo "codesign debug:"
    security find-identity -v -p codesigning || true
    security show-keychain-info "${HOME}/Library/Keychains/login.keychain-db" || true
    codesign_bundle_contents "${APP_BUNDLE}" "${SELF_SIGN_CERT_NAME}" "${MAIN_BIN}"
    /usr/bin/codesign --verify --strict --verbose=4 "${MAIN_BIN}"
    /usr/bin/codesign --verify --deep --strict --verbose=4 "${APP_BUNDLE}"
    echo "App macOS creada y firmada con certificado autofirmado: ${APP_BUNDLE}"
  else
    echo "App macOS creada sin firma: ${APP_BUNDLE}"
  fi

  if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
    local dmg_arch="${ARCH:-$(uname -m)}"
    dmg_path="$(create_macos_dmg "${APP_BUNDLE}")"
    local final_dmg="${BUILD_DIR}/${BUNDLE_NAME}_${dmg_arch}.dmg"
    mv "${BUILD_DIR}/${BUNDLE_NAME}.dmg" "${final_dmg}"
    echo "DMG creado: ${final_dmg}"
    upload_to_sftp "${final_dmg}"
    local daemon_dir="${DOWNLOADS_DIR}/daemons"
    if [[ -d "${daemon_dir}" ]]; then
      while IFS= read -r -d '' daemon; do
        upload_to_sftp "${daemon}"
      done < <(find "${daemon_dir}" -type f -name 'zfsmgr_daemon*' -print0)
    fi
  fi

else
  echo "Empaquetado .app omitido (usa --bundle para generarlo)."
fi
