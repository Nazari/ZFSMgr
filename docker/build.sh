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

# builds/ aislado: el contenedor y el equipo anfitrión usan toolchains distintas —Qt de
# /opt/toolchain frente al del sistema, OpenSSL de la imagen frente al de ~/opt— y
# compartir los directorios de compilación dejaba cachés de CMake apuntando a rutas que
# dentro del contenedor no existen. Se manifestaba de formas dispares y confusas: un
# rcc de una versión de Qt ejecutándose contra las bibliotecas de otra, u OpenSSL
# "encontrado" en una ruta del host.
#
# Se monta encima en vez de parametrizar los directorios, que están fijados en una
# docena de sitios.
mkdir -p "${PROJECT_ROOT}/builds-docker"

# El bundle que se empaqueta dentro de la app lleva los agentes de las CINCO
# plataformas, pero la imagen no compila macOS —el SDK de Xcode no es
# redistribuible—. Se siembra la caché con los agentes que haya en el equipo para que
# los de macOS estén disponibles; los que el contenedor compile sobrescriben los suyos.
if [[ -d "${PROJECT_ROOT}/builds/agents" ]]; then
  mkdir -p "${PROJECT_ROOT}/builds-docker/agents"
  cp -a "${PROJECT_ROOT}/builds/agents/." "${PROJECT_ROOT}/builds-docker/agents/" 2>/dev/null || true
fi

DOCKER_ARGS+=(-v "${PROJECT_ROOT}/builds-docker:${PROJECT_ROOT}/builds")

if [[ ",${PLATFORMS}," == *",macos,"* ]]; then
  if [[ ! -d "${OSXCROSS_DIR}" ]]; then
    echo "macos solicitado pero no existe ${OSXCROSS_DIR} en el host." >&2
    echo "Aprovisione osxcross con su propio SDK o quite macos de --platforms." >&2
    exit 1
  fi
  DOCKER_ARGS+=(-v "${OSXCROSS_DIR}:/opt/osxcross:ro")

  # --- Las bibliotecas que osxcross necesita y la imagen no tiene.
  #
  # osxcross se compila EN EL ANFITRIÓN, contra las bibliotecas del anfitrión; la imagen es
  # otra distribución. Aquí eso se traduce en que su enlazador —`ld` de cctools, y no hay
  # alternativa: no incluye lld— pide `libxml2.so.16` (libxml2 2.13) mientras la imagen
  # trae la 2.9, que es `libxml2.so.2`.
  #
  # El síntoma sin esto es de los que cuestan una tarde: el cruce arranca, compila OpenSSL
  # durante minutos y muere al enlazar con «error while loading shared libraries», que no
  # menciona ni osxcross ni macOS.
  #
  # Se detecta preguntándole al propio binario qué le falta, en vez de escribir aquí una
  # lista que envejecería con cada actualización del anfitrión. Lo suyo a la larga es
  # compilar osxcross DENTRO de la imagen, y entonces esto sobra.
  OSX_LIBS_DIR="$(mktemp -d)"
  trap 'rm -rf "${OSX_LIBS_DIR}"' EXIT
  _osx_ld="$(ls "${OSXCROSS_DIR}"/target/bin/*-apple-darwin*-ld 2>/dev/null | head -1)"
  if [[ -n "${_osx_ld}" ]]; then
    faltan="$(docker run --rm -v "${OSXCROSS_DIR}:/opt/osxcross:ro" "${IMAGE}" -lc \
      "ldd '${_osx_ld/${OSXCROSS_DIR}//opt/osxcross}' 2>/dev/null | awk '/not found/{print \$1}'" || true)"
    for lib in $(printf '%s\n' ${faltan} | sort -u); do
      # El orden de búsqueda es EXPLÍCITO y empieza por x86_64: con un `ls` de comodines,
      # la de 32 bits gana por orden alfabético —`/lib/i386-linux-gnu/…`— y además suele ser
      # un enlace roto, así que la copia fallaba y el guion moría sin decir por qué.
      origen=""
      for cand in "/usr/lib/x86_64-linux-gnu/${lib}" "/lib/x86_64-linux-gnu/${lib}" \
                  "/usr/lib/${lib}" "/usr/local/lib/${lib}"; do
        if [[ -r "${cand}" ]] && cp -L "${cand}" "${OSX_LIBS_DIR}/${lib}" 2>/dev/null; then
          origen="${cand}"
          break
        fi
      done
      if [[ -z "${origen}" ]]; then
        echo "osxcross necesita ${lib} y no hay una copia legible de 64 bits en este equipo." >&2
        echo "Instálela, o compile osxcross dentro de la imagen." >&2
        exit 1
      fi
      echo "[docker] osxcross: se aporta ${lib} desde ${origen}"
    done
  fi
  if [[ -n "$(ls -A "${OSX_LIBS_DIR}" 2>/dev/null)" ]]; then
    DOCKER_ARGS+=(-v "${OSX_LIBS_DIR}:/opt/osxcross-libs:ro")
    DOCKER_ARGS+=(-e "LD_LIBRARY_PATH=/opt/osxcross-libs")
  fi
fi

if [[ ${OPEN_SHELL} -eq 1 ]]; then
  exec docker run -it "${DOCKER_ARGS[@]}" "${IMAGE}"
fi

extra=""
if [[ ${#PASSTHROUGH_ARGS[@]} -gt 0 ]]; then
  extra="$(printf ' %q' "${PASSTHROUGH_ARGS[@]}")"
fi

echo "[docker] compilando: ${PLATFORMS}${extra:+ (extra:${extra})}"
# El `|| rc=$?` no es adorno: con `set -e`, un cruce que falle a mitad mataría el guion
# AQUÍ y no llegaría a ejecutarse la copia de los agentes de vuelta —que es justo cuando
# más falta hace, porque lo que sí se compiló se quedaría sin llegar a `builds/agents`—.
rc=0
docker run "${DOCKER_ARGS[@]}" "${IMAGE}" -lc \
  "mkdir -p \"\${HOME}\" && ln -sfn /opt/toolchain/Qt \"\${HOME}/Qt\" && scripts/buildall-cross.sh --platforms '${PLATFORMS}'${extra}" || rc=$?

# --- Los agentes VUELVEN a `builds/agents`, que es donde los busca el cliente.
#
# Dentro del contenedor, `builds/` es en realidad `builds-docker/` del anfitrión, así que
# lo que se compila aquí NO llega solo a `builds/agents/`, que es la ruta que consultan el
# CLI y la interfaz al instalar el daemon en otra máquina.
#
# Sin esta copia pasaba lo peor que puede pasar en un despliegue: se instalaba el agente de
# una compilación ANTERIOR, la máquina quedaba atendida por un daemon de otra versión de
# protocolo, y el único síntoma era un asterisco en el listado.
#
# Se copia AUNQUE el cruce haya fallado a mitad: lo que sí se compiló es válido y reciente,
# y quedarse con lo viejo por un fallo en otra plataforma es justo el problema que esto
# viene a quitar.
if [[ -d "${PROJECT_ROOT}/builds-docker/agents" ]]; then
  mkdir -p "${PROJECT_ROOT}/builds/agents"
  cp -a "${PROJECT_ROOT}/builds-docker/agents/." "${PROJECT_ROOT}/builds/agents/"
  echo "[docker] agentes disponibles para instalar (builds/agents):"
  for d in "${PROJECT_ROOT}"/builds/agents/*/; do
    [[ -d "${d}" ]] || continue
    bin="$(ls "${d}"zfsmgr_agent* 2>/dev/null | head -1)"
    [[ -n "${bin}" ]] || continue
    # La versión sale del propio binario: es la única que no puede mentir.
    ver="$(strings "${bin}" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]{6,}' | sort -u | head -1)"
    printf '           %-18s %s\n' "$(basename "${d%/}")" "${ver:-?}"
  done
fi

exit ${rc}
