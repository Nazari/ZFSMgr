#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ZFSMGR_BUILD_DIR:-${PROJECT_ROOT}/builds/macos}"
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
UNIVERSAL=0

usage() {
  cat <<'EOF'
Uso:
  build-macos.sh [opciones] [-- <args extra de CMake>]

Opciones:
  --bundle       Genera el bundle .app (por defecto)
  --no-bundle    Compila sin empaquetar el bundle final
  --universal    Compila el .app universal (arm64 + x86_64). Requiere un OpenSSL
                 universal: si no se indica ZFSMGR_OPENSSL_PREFIX, se construye
                 con scripts/build-openssl-universal-macos.sh
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
  elif [[ "${arg}" == "--universal" ]]; then
    UNIVERSAL=1
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
  local app_name volume staging_dir hdi_rc dmg_path
  app_name="$(basename "${app_path}")"
  volume="${app_name%.app}"
  dmg_path="${BUILD_DIR}/${volume}.dmg"
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
  printf '%s\n' "${dmg_path}"
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
  # Los ficheros sueltos que no son Mach-O (DWARF dentro de los .dSYM, recursos)
  # no se pueden firmar y harían abortar el script. Se detectan dejando que lipo
  # falle sobre ellos. Los directorios sí pasan: son bundles y frameworks.
  if [[ -f "${path}" ]] && ! lipo -archs "${path}" >/dev/null 2>&1; then
    return 0
  fi
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

# Primero, lo que exporta un entorno con Qt ya activado (install-qt-action en CI,
# o un shell donde se haya cargado Qt a mano). Sin esto, en cualquier máquina que
# no fuera la del autor QT_PREFIX quedaba vacío y el despliegue de frameworks y
# plugins se saltaba ENTERO emitiendo solo avisos: el .app resultante no llevaba
# Qt dentro y solo arrancaba donde ya estuviera instalado.
if [[ -n "${QT_ROOT_DIR:-}" && -d "${QT_ROOT_DIR}" ]]; then
  QT_PREFIX="${QT_ROOT_DIR}"
elif [[ -n "${QT_PLUGIN_PATH:-}" && -d "${QT_PLUGIN_PATH}" ]]; then
  QT_PREFIX="$(cd "${QT_PLUGIN_PATH}/.." && pwd -P)"
elif command -v qmake6 >/dev/null 2>&1; then
  QT_PREFIX="$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || true)"
elif command -v qmake >/dev/null 2>&1; then
  QT_PREFIX="$(qmake -query QT_INSTALL_PREFIX 2>/dev/null || true)"
fi

if [[ -z "${QT_PREFIX}" ]]; then
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
fi

# ${HOME}/Qt, no una ruta personal fija: antes esto era /Users/linarese/Qt.
if [[ -z "${QT_PREFIX}" && -d "${HOME}/Qt" ]]; then
  latest_qt_macos="$(find "${HOME}/Qt" -maxdepth 2 -type d -path "${HOME}/Qt/*/macos" | sort -V | tail -n1 || true)"
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
        # Las de /System y /usr/lib ya se han descartado arriba, así que lo que llega
        # aquí es una biblioteca que el binario necesita y que NO va a ir dentro del
        # bundle: la aplicación no arrancará. Era un aviso y se seguía adelante.
        echo "Error: dependencia no resuelta: ${dep}" >&2
        echo "Requerida por: ${current}" >&2
        exit 1
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

# El segundo argumento marca si el directorio es obligatorio. "platforms" y "tls" lo
# son: sin el primero una app Qt no arranca ("could not load the Qt platform plugin"),
# y sin el segundo no hay TLS, que es por donde habla con el daemon. Faltar cualquiera
# de los dos producía un .app roto con la compilación en verde.
copy_qt_plugin_dir() {
  local dir_name="$1"
  local required="${2:-0}"
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
    if [[ "${required}" == "1" ]]; then
      echo "Error: no se encontró el directorio de plugins Qt '${dir_name}', que es" >&2
      echo "obligatorio: sin él la aplicación no arranca en macOS." >&2
      exit 1
    fi
    echo "Aviso: no se encontró el directorio de plugins Qt opcional '${dir_name}'." >&2
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

# En universal no vale el OpenSSL de Homebrew: solo trae la arquitectura de la
# máquina y el enlazado de la otra fallaría. Se usa el prefijo universal, que se
# construye si no está.
if [[ ${UNIVERSAL} -eq 1 && -z "${ZFSMGR_OPENSSL_PREFIX:-}" ]]; then
  "${SCRIPT_DIR}/build-openssl-universal-macos.sh"
  ZFSMGR_OPENSSL_PREFIX="${PROJECT_ROOT}/builds/openssl-universal"
fi

if [[ -n "${ZFSMGR_OPENSSL_PREFIX:-}" ]]; then
  if [[ ! -d "${ZFSMGR_OPENSSL_PREFIX}" ]]; then
    echo "Error: ZFSMGR_OPENSSL_PREFIX apunta a ${ZFSMGR_OPENSSL_PREFIX}, que no existe." >&2
    exit 1
  fi
  OPENSSL_PREFIX="${ZFSMGR_OPENSSL_PREFIX}"
  export CMAKE_PREFIX_PATH="${OPENSSL_PREFIX}:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/opt/homebrew/opt/openssl@3" ]]; then
  OPENSSL_PREFIX="/opt/homebrew/opt/openssl@3"
  export CMAKE_PREFIX_PATH="${OPENSSL_PREFIX}:${CMAKE_PREFIX_PATH:-}"
elif [[ -d "/usr/local/opt/openssl@3" ]]; then
  OPENSSL_PREFIX="/usr/local/opt/openssl@3"
  export CMAKE_PREFIX_PATH="${OPENSSL_PREFIX}:${CMAKE_PREFIX_PATH:-}"
fi

cmake_cmd=(cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ ${UNIVERSAL} -eq 1 ]]; then
  MAC_ARCH="universal"
  cmake_cmd+=(-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64")
fi
if [[ -n "${OPENSSL_PREFIX}" ]]; then
  cmake_cmd+=(-DOPENSSL_ROOT_DIR="${OPENSSL_PREFIX}")
fi
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
  cmake_cmd+=("${EXTRA_CMAKE_ARGS[@]}")
fi
"${cmake_cmd[@]}"

APP_VERSION="$(resolve_app_version)"
# Debe coincidir con ZFSMGR_BUNDLE_NAME de resources/CMakeLists.txt, que ya no
# lleva la versión: el bundle se llama ZFSMgr.app siempre, para que al copiarlo a
# /Applications reemplace al anterior. La versión va en el nombre del .zip y en el
# .dmg, que sí se construyen con APP_VERSION.
BUNDLE_NAME="ZFSMgr"

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
  copy_qt_plugin_dir "platforms" 1
  copy_qt_plugin_dir "styles"
  copy_qt_plugin_dir "imageformats"
  copy_qt_plugin_dir "iconengines"
  copy_qt_plugin_dir "networkinformation"
  copy_qt_plugin_dir "tls" 1
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

  # El despliegue de Qt solo emitía avisos, así que un bundle SIN Qt dentro salía
  # como build correcto y solo fallaba al abrirlo en un Mac sin Qt instalado. Aquí
  # se comprueba lo mínimo imprescindible para que arranque: QtCore y el plugin de
  # plataforma cocoa. Si falta, es un fallo de build, no un aviso.
  missing_deploy=""
  if [[ ! -d "${APP_BUNDLE}/Contents/Frameworks/QtCore.framework" ]]; then
    missing_deploy="Contents/Frameworks/QtCore.framework"
  elif [[ ! -e "${APP_BUNDLE}/Contents/PlugIns/platforms/libqcocoa.dylib" ]]; then
    missing_deploy="Contents/PlugIns/platforms/libqcocoa.dylib"
  fi
  if [[ -n "${missing_deploy}" ]]; then
    echo "Error: el bundle no es autocontenido, falta ${missing_deploy}." >&2
    echo "       QT_PREFIX=${QT_PREFIX:-<vacío>} — si está vacío, Qt no se localizó." >&2
    exit 1
  fi

  # Agentes de las demás plataformas dentro del bundle. Sin esto, la app de macOS
  # no puede instalar ni actualizar el daemon en un host remoto: los busca en
  # Contents/Resources/agents/<plataforma>-<arch>/ (ver findDeployableAgentBinaryPath
  # en mainwindow_connections.cpp) y no encontraba nada, porque la regla install()
  # que los coloca en share/zfsmgr/agents solo la usan CPack y el .deb, no el .app.
  #
  # Va ANTES de firmar a propósito: añadir ficheros a un bundle ya firmado rompe el
  # sello de recursos, que es la misma clase de error que hacía que la app ni
  # arrancara.
  # libssl al bundle. La GUI no la enlaza (solo usa libcrypto), así que el despliegue
  # automático no la copia y Qt se queda sin backend de OpenSSL: en el .app publicado
  # availableBackends() devolvía [securetransport, cert-only]. Con SecureTransport,
  # macOS exige la clave privada del cliente en el llavero y pide contraseña, y su
  # validación PKI ignora la CA que se le pasa.
  #
  # Va antes de firmar, como los agentes: añadir binarios después rompe el sello.
  if [[ -n "${OPENSSL_PREFIX}" ]]; then
    for lib in libssl.3.dylib libcrypto.3.dylib; do
      src="${OPENSSL_PREFIX}/lib/${lib}"
      dst="${APP_BUNDLE}/Contents/Frameworks/${lib}"
      if [[ -f "${src}" && ! -f "${dst}" ]]; then
        cp -f "${src}" "${dst}"
        chmod u+w "${dst}"
        install_name_tool -id "@executable_path/../Frameworks/${lib}" "${dst}" 2>/dev/null || true
        echo "  copiada al bundle: ${lib}"
      fi
    done
    # libssl depende de libcrypto: reapuntarla dentro del bundle o buscaría la del
    # prefijo de compilación, que en la máquina del usuario no existe.
    libssl_dst="${APP_BUNDLE}/Contents/Frameworks/libssl.3.dylib"
    if [[ -f "${libssl_dst}" ]]; then
      while IFS= read -r dep; do
        case "${dep}" in
          *libcrypto*)
            install_name_tool -change "${dep}" \
              "@executable_path/../Frameworks/libcrypto.3.dylib" "${libssl_dst}" 2>/dev/null || true
            ;;
        esac
      done < <(otool -L "${libssl_dst}" | awk 'NR>1 {print $1}')
    fi
  fi

  # El intérprete, dentro del .app, y sus catálogos al lado.
  #
  # macOS no tiene instalador: se arrastra el .app. Así que el intérprete viaja donde
  # viaja la aplicación, y quien lo quiera en el PATH pone un enlace:
  #
  #   ln -s /Applications/ZFSMgr.app/Contents/MacOS/zfsmgr_cli /usr/local/bin/zfsmgr_cli
  #
  # En Contents/MacOS y no en otro sitio: es lo que hace que
  # `<ejecutable>/../Resources/i18n` —una de las cuatro rutas que src/cli/main.cpp ya
  # busca— dé con los catálogos. El intérprete no enlaza Qt y los lee del disco.
  #
  # Antes de firmar, como los agentes.
  # El servidor web viaja igual y por el mismo motivo: los dos son clientes sin Qt y en
  # macOS no hay instalador que los ponga en el PATH.
  for cliente in zfsmgr_cli zfsmgr_web; do
    if [[ ! -f "${BUILD_DIR}/${cliente}" ]]; then
      echo "Error: no se encontró ${BUILD_DIR}/${cliente} para meter en el bundle." >&2
      echo "       Sin él, el .app de macOS sale incompleto." >&2
      exit 1
    fi
    cp -f "${BUILD_DIR}/${cliente}" "${APP_BUNDLE}/Contents/MacOS/${cliente}"
    chmod 0755 "${APP_BUNDLE}/Contents/MacOS/${cliente}"
    echo "  empaquetado: Contents/MacOS/${cliente}"
  done
  mkdir -p "${APP_BUNDLE}/Contents/Resources/i18n"
  cp -f "${PROJECT_ROOT}"/i18n/*.json "${APP_BUNDLE}/Contents/Resources/i18n/"

  AGENT_BUNDLE_SRC="${ZFSMGR_AGENT_BUNDLE_DIR:-${PROJECT_ROOT}/builds/agent-bundle}"
  agents_dst="${APP_BUNDLE}/Contents/Resources/agents"
  rm -rf "${agents_dst}"
  mkdir -p "${agents_dst}"
  if [[ -d "${AGENT_BUNDLE_SRC}" ]]; then
    cp -R "${AGENT_BUNDLE_SRC}/." "${agents_dst}/"
  fi
  # El agente de macOS lo produce este mismo build, así que no puede venir del
  # bundle externo. En universal sirve para las dos arquitecturas, y se coloca bajo
  # los dos nombres porque el GUI busca por macos-arm64 o macos-amd64 según el host.
  if [[ -f "${BUILD_DIR}/zfsmgr_agent" ]]; then
    for macos_key in macos-arm64 macos-amd64; do
      mkdir -p "${agents_dst}/${macos_key}"
      cp -f "${BUILD_DIR}/zfsmgr_agent" "${agents_dst}/${macos_key}/zfsmgr_agent"
    done
  fi
  find "${agents_dst}" -type f -name 'zfsmgr_agent*' -exec chmod +x {} + 2>/dev/null || true
  agent_count="$(find "${agents_dst}" -type f -name 'zfsmgr_agent*' | wc -l | tr -d ' ')"
  if [[ "${agent_count}" -eq 0 ]]; then
    echo "Error: no se empaquetó ningún agente en el bundle." >&2
    echo "       Sin ellos la app no puede instalar el daemon en hosts remotos." >&2
    echo "       Origen esperado: ${AGENT_BUNDLE_SRC}" >&2
    exit 1
  fi
  echo "Agentes empaquetados en el bundle (${agent_count}):"
  find "${agents_dst}" -type f -name 'zfsmgr_agent*' | sed "s|^${agents_dst}/|  |"

  # En universal, comprobar que TODO lo que se lleva el bundle trae las dos
  # arquitecturas. Basta con que una biblioteca se cuele con una sola para que la
  # app muera al arrancar en el otro Mac, y eso no se ve hasta probarlo allí.
  if [[ ${UNIVERSAL} -eq 1 ]]; then
    thin_files=""
    while IFS= read -r candidate; do
      # No se filtra por nombre ni por permisos: el icono .icns lleva el bit de
      # ejecución y colaba como si fuera un binario. Se recorre todo y se deja que
      # lipo decida — falla en lo que no es Mach-O, y eso es justo lo que hay que
      # saltarse.
      archs="$(lipo -archs "${candidate}" 2>/dev/null)" || continue
      [[ -z "${archs}" ]] && continue
      if [[ "${archs}" != *arm64* || "${archs}" != *x86_64* ]]; then
        thin_files+="  ${candidate#${APP_BUNDLE}/} (${archs})"$'\n'
      fi
      # Contents/Resources/agents queda fuera: son binarios de OTRAS plataformas que
      # se despliegan en hosts remotos, no código que macOS vaya a cargar. Los de
      # macOS del bundle cruzado son además de una sola arquitectura, así que
      # incluirlos aquí haría fallar el build sin motivo.
    done < <(find "${APP_BUNDLE}" -path "${APP_BUNDLE}/Contents/Resources/agents" -prune -o -type f -print)
    if [[ -n "${thin_files}" ]]; then
      echo "Error: el bundle universal contiene binarios de una sola arquitectura:" >&2
      printf '%s' "${thin_files}" >&2
      exit 1
    fi
    echo "Bundle universal verificado: arm64 + x86_64 en todos los binarios."
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
    # Sin identidad NO se puede dejar el bundle sin firmar. El despliegue reescribe
    # los install_name de los frameworks de Qt y de libcrypto, y eso invalida la
    # firma con la que venían; en Apple Silicon dyld mata el proceso al cargar la
    # primera dependencia con firma inválida:
    #   EXC_BAD_ACCESS (SIGKILL (Code Signature Invalid))
    #   Termination Reason: Namespace CODESIGNING, Code 2, Invalid Page
    # La firma ad-hoc no necesita certificado ni cuota de Apple y satisface esa
    # exigencia. No sustituye a Developer ID: Gatekeeper sigue pidiendo quitar la
    # cuarentena, pero la app arranca.
    echo "Sin identidad de firma: firmando ad-hoc (obligatorio en Apple Silicon)"
    codesign_bundle_contents "${APP_BUNDLE}" "-" "${MAIN_BIN}"
    /usr/bin/codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}"
    echo "App macOS creada con firma ad-hoc: ${APP_BUNDLE}"
  fi

  if [[ "${UPLOAD_SFTP}" -eq 1 ]]; then
    dmg_arch="${ARCH:-$(uname -m)}"
    dmg_path="$(create_macos_dmg "${APP_BUNDLE}")"
    # La versión se pone aquí explícitamente: BUNDLE_NAME ya no la lleva, y un DMG
    # sin versión en el nombre sobrescribiría al de la entrega anterior.
    final_dmg="${BUILD_DIR}/ZFSMgr-${APP_VERSION}_${dmg_arch}.dmg"
    mv "${BUILD_DIR}/${BUNDLE_NAME}.dmg" "${final_dmg}"
    echo "DMG creado: ${final_dmg}"
    upload_to_sftp "${final_dmg}"
  fi

else
  echo "Empaquetado .app omitido (usa --bundle para generarlo)."
fi
