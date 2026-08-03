#!/usr/bin/env bash
# Compila ZFSMgr para varias plataformas dentro del contenedor de toolchain.
#
# Sustituye a buildall.sh, que lanzaba los builds por SSH en máquinas que ya no
# existen. Aquí no hay máquinas remotas ni dependencias del home: la toolchain vive
# en la imagen y el código se monta.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

IMAGE="${ZFSMGR_BUILD_IMAGE:-zfsmgr-cross:latest}"
PLATFORMS="${BUILD_PLATFORMS:-linux,windows,freebsd}"
OSXCROSS_DIR="${OSXCROSS_DIR:-/opt/osxcross}"

usage() {
  cat <<'EOF'
Uso:
  docker/build.sh [--platforms linux,windows,freebsd[,macos]] [--rebuild-image] [--shell]

Descripción:
  Construye la imagen de toolchain si hace falta y ejecuta buildall-cross.sh dentro,
  con el repositorio montado en /src. Los artefactos aparecen en builds/ del repo,
  como en un build local.

Opciones:
  --platforms <lista>  Plataformas a compilar (default: linux,windows,freebsd)
  --rebuild-image      Reconstruye la imagen aunque ya exista
  --shell              Abre una shell dentro del contenedor en lugar de compilar

macOS:
  No va en la imagen porque el SDK de Xcode no es redistribuible. Si tiene osxcross
  aprovisionado en el host, añada 'macos' a --platforms y se montará desde
  OSXCROSS_DIR (default: /opt/osxcross).

Variables:
  ZFSMGR_BUILD_IMAGE   Nombre de la imagen (default: zfsmgr-cross:latest)
  OSXCROSS_DIR         Ruta de osxcross en el host, para macOS
EOF
}

REBUILD=0
OPEN_SHELL=0
# Todo lo que venga tras -- se le pasa tal cual a buildall-cross.sh dentro del
# contenedor. Sin esto no había forma de usar ninguna de sus opciones; hizo falta
# para --windows-installer 0, porque el instalador Inno necesita Wine y Wine no
# está en la imagen.
PASSTHROUGH_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --platforms) shift; PLATFORMS="${1:?falta valor para --platforms}"; shift ;;
    --rebuild-image) REBUILD=1; shift ;;
    --shell) OPEN_SHELL=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; PASSTHROUGH_ARGS=("$@"); break ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if ! command -v docker >/dev/null 2>&1; then
  echo "docker no está disponible en el PATH" >&2
  exit 1
fi

if [[ ${REBUILD} -eq 1 ]] || ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
  echo "[docker] construyendo ${IMAGE} (la primera vez descarga Qt y el sysroot de FreeBSD; tarda)"
  QT_VERSION="$(tr -d '[:space:]' < "${PROJECT_ROOT}/qt-version.txt" 2>/dev/null || echo 6.8.3)"
  docker build -f "${SCRIPT_DIR}/Dockerfile" --build-arg "QT_VERSION=${QT_VERSION}" \
    -t "${IMAGE}" "${PROJECT_ROOT}"
fi

# Se monta en la MISMA ruta absoluta que en el host, no en /src: CMake graba rutas
# absolutas en CMakeCache.txt, así que montar en otro sitio invalida las cachés y
# obliga a elegir entre compilar dentro o fuera. Con la ruta idéntica, los mismos
# directorios de builds/ sirven para ambos.
DOCKER_ARGS=(--rm -v "${PROJECT_ROOT}:${PROJECT_ROOT}" -w "${PROJECT_ROOT}")

# El build escribe en builds/ del repo montado: sin --user, los artefactos saldrían
# como root y el siguiente build nativo del host fallaría por permisos.
DOCKER_ARGS+=(--user "$(id -u):$(id -g)" -e HOME=/tmp/zfsmgrhome)

if [[ ",${PLATFORMS}," == *",macos,"* ]]; then
  if [[ ! -d "${OSXCROSS_DIR}" ]]; then
    echo "macos solicitado pero no existe ${OSXCROSS_DIR} en el host." >&2
    echo "Aprovisione osxcross con su propio SDK o quite macos de --platforms." >&2
    exit 1
  fi
  DOCKER_ARGS+=(-v "${OSXCROSS_DIR}:/opt/osxcross:ro")
fi

if [[ ${OPEN_SHELL} -eq 1 ]]; then
  exec docker run -it "${DOCKER_ARGS[@]}" "${IMAGE}"
fi

extra=""
if [[ ${#PASSTHROUGH_ARGS[@]} -gt 0 ]]; then
  extra="$(printf ' %q' "${PASSTHROUGH_ARGS[@]}")"
fi

echo "[docker] compilando: ${PLATFORMS}${extra:+ (extra:${extra})}"
exec docker run "${DOCKER_ARGS[@]}" "${IMAGE}" -lc \
  "mkdir -p \"\${HOME}\" && ln -sfn /opt/toolchain/Qt \"\${HOME}/Qt\" && scripts/buildall-cross.sh --platforms '${PLATFORMS}'${extra}"
