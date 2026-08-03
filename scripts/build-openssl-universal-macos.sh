#!/usr/bin/env bash
# Compila OpenSSL para macOS como binario universal (arm64 + x86_64).
#
# Por qué existe: Qt distribuye sus frameworks de macOS ya universales, así que la
# única pieza que impedía generar un .app universal era OpenSSL. El de Homebrew trae
# solo la arquitectura de la máquina donde corre, lo que obligaba a mantener un job
# de CI por arquitectura y a publicar dos descargas distintas.
#
# OpenSSL no sabe construirse universal de una vez: se compila dos veces, cada una
# para su arquitectura, y se fusionan las bibliotecas con lipo. Ambas compilaciones
# usan el MISMO --prefix a propósito, para que el install_name (LC_ID_DYLIB) de las
# dos coincida y el resultado de lipo sea coherente; se separan con DESTDIR.
#
# Uso:
#   scripts/build-openssl-universal-macos.sh [--force] [--prefix <dir>]
#
# Deja el prefijo listo para pasárselo a CMake como OPENSSL_ROOT_DIR.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Versión fijada con su checksum contrastado contra el .sha256 publicado por el
# proyecto. Al subirla hay que actualizar AMBOS: si el hash no cuadra, el script
# aborta en vez de compilar lo que sea que se haya descargado.
OPENSSL_VERSION="${ZFSMGR_OPENSSL_VERSION:-3.5.7}"
OPENSSL_SHA256="${ZFSMGR_OPENSSL_SHA256:-a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8}"

PREFIX="${ZFSMGR_OPENSSL_UNIVERSAL_PREFIX:-${PROJECT_ROOT}/builds/openssl-universal}"
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) FORCE=1; shift ;;
    --prefix) PREFIX="${2:?--prefix necesita un directorio}"; shift 2 ;;
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; exit 2 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Error: este script solo tiene sentido en macOS (uname -s = $(uname -s))." >&2
  exit 1
fi

STAMP="${PREFIX}/.zfsmgr-universal-${OPENSSL_VERSION}"
if [[ ${FORCE} -eq 0 && -f "${STAMP}" ]]; then
  echo "OpenSSL universal ${OPENSSL_VERSION} ya presente en ${PREFIX} (usa --force para rehacerlo)."
  exit 0
fi

WORK="${PREFIX}.build"
rm -rf "${WORK}" "${PREFIX}"
mkdir -p "${WORK}" "${PREFIX}"

TARBALL="${WORK}/openssl-${OPENSSL_VERSION}.tar.gz"
URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"

echo "==> Descargando OpenSSL ${OPENSSL_VERSION}"
curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors -o "${TARBALL}" "${URL}"

echo "==> Verificando checksum"
actual_sha="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [[ "${actual_sha}" != "${OPENSSL_SHA256}" ]]; then
  echo "Error: checksum de OpenSSL no coincide." >&2
  echo "  esperado: ${OPENSSL_SHA256}" >&2
  echo "  obtenido: ${actual_sha}" >&2
  exit 1
fi

build_one_arch() {
  local arch="$1"          # arm64 | x86_64
  local src="${WORK}/src-${arch}"
  local dest="${WORK}/dest-${arch}"

  echo "==> Compilando OpenSSL para ${arch}"
  rm -rf "${src}" "${dest}"
  mkdir -p "${src}"
  tar xzf "${TARBALL}" -C "${src}" --strip-components=1

  (
    cd "${src}"
    # no-shared NO: el bundle ya sabe llevarse libcrypto.3.dylib dentro y reescribir
    # su install_name, así que se mantiene el enlazado dinámico que ya había con el
    # OpenSSL de Homebrew. no-tests recorta bastante tiempo de compilación, y no se
    # pueden ejecutar de todos modos al compilar x86_64 desde un Mac ARM.
    ./Configure "darwin64-${arch}-cc" \
      --prefix="${PREFIX}" \
      --openssldir="${PREFIX}/ssl" \
      no-tests \
      no-docs
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    make DESTDIR="${dest}" install_dev
  )
}

build_one_arch arm64
build_one_arch x86_64

# DESTDIR antepone su ruta al prefijo, así que lo instalado queda en $dest/$PREFIX.
ARM_ROOT="${WORK}/dest-arm64${PREFIX}"
X86_ROOT="${WORK}/dest-x86_64${PREFIX}"

for root in "${ARM_ROOT}" "${X86_ROOT}"; do
  if [[ ! -d "${root}/lib" || ! -d "${root}/include/openssl" ]]; then
    echo "Error: install_dev no dejó lib/ e include/openssl/ en ${root}." >&2
    echo "       Si OpenSSL cambió la semántica de ese target, hay que ajustar el script." >&2
    exit 1
  fi
done

echo "==> Fusionando bibliotecas con lipo"
mkdir -p "${PREFIX}/lib"
merged=0
while IFS= read -r arm_lib; do
  rel="${arm_lib#${ARM_ROOT}/lib/}"
  x86_lib="${X86_ROOT}/lib/${rel}"
  out="${PREFIX}/lib/${rel}"
  mkdir -p "$(dirname "${out}")"
  if [[ -L "${arm_lib}" ]]; then
    cp -a "${arm_lib}" "${out}"          # enlaces de versión: libcrypto.dylib -> libcrypto.3.dylib
    continue
  fi
  if [[ ! -f "${x86_lib}" ]]; then
    echo "Error: ${rel} existe en arm64 pero no en x86_64." >&2
    exit 1
  fi
  lipo -create "${arm_lib}" "${x86_lib}" -output "${out}"
  merged=$((merged + 1))
done < <(find "${ARM_ROOT}/lib" -maxdepth 1 \( -name 'lib*.dylib' -o -name 'lib*.a' \))

if [[ ${merged} -eq 0 ]]; then
  echo "Error: no se fusionó ninguna biblioteca." >&2
  exit 1
fi

echo "==> Copiando cabeceras"
cp -R "${ARM_ROOT}/include" "${PREFIX}/"
if [[ -d "${ARM_ROOT}/lib/pkgconfig" ]]; then
  cp -R "${ARM_ROOT}/lib/pkgconfig" "${PREFIX}/lib/"
fi

# configuration.h es la única cabecera que puede diferir entre arquitecturas. En
# macOS ambas son LP64 y suele salir idéntica, pero si no lo es, copiar una sola
# rompería silenciosamente la otra arquitectura: se emite una cabecera que elige
# según el preprocesador.
ARM_CONF="${ARM_ROOT}/include/openssl/configuration.h"
X86_CONF="${X86_ROOT}/include/openssl/configuration.h"
if [[ -f "${ARM_CONF}" && -f "${X86_CONF}" ]] && ! cmp -s "${ARM_CONF}" "${X86_CONF}"; then
  echo "==> configuration.h difiere entre arquitecturas: generando cabecera de despacho"
  cp "${ARM_CONF}" "${PREFIX}/include/openssl/configuration-arm64.h"
  cp "${X86_CONF}" "${PREFIX}/include/openssl/configuration-x86_64.h"
  cat > "${PREFIX}/include/openssl/configuration.h" <<'EOF'
/* Generado por scripts/build-openssl-universal-macos.sh: las dos arquitecturas
   produjeron un configuration.h distinto, así que se elige en tiempo de
   preprocesado en vez de quedarse con uno solo. */
#if defined(__aarch64__) || defined(__arm64__)
#  include "configuration-arm64.h"
#elif defined(__x86_64__)
#  include "configuration-x86_64.h"
#else
#  error "Arquitectura no contemplada por el OpenSSL universal de ZFSMgr"
#endif
EOF
fi

echo "==> Verificando el resultado"
for lib in libcrypto.3.dylib libssl.3.dylib; do
  target="${PREFIX}/lib/${lib}"
  if [[ ! -f "${target}" ]]; then
    echo "Error: falta ${target}." >&2
    exit 1
  fi
  archs="$(lipo -archs "${target}")"
  if [[ "${archs}" != *arm64* || "${archs}" != *x86_64* ]]; then
    echo "Error: ${lib} no es universal (arquitecturas: ${archs})." >&2
    exit 1
  fi
  echo "  ${lib}: ${archs}"
done

touch "${STAMP}"
rm -rf "${WORK}"

echo
echo "OpenSSL universal ${OPENSSL_VERSION} listo en: ${PREFIX}"
echo "Pásaselo a CMake con -DOPENSSL_ROOT_DIR=${PREFIX}"
