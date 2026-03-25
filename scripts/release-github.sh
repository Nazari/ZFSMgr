#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMAKE_FILE="${PROJECT_ROOT}/resources/CMakeLists.txt"
GIT_REMOTE="${GIT_REMOTE:-github}"
ARTIFACTS_ROOT="${ARTIFACTS_DIR:-${PROJECT_ROOT}/.release-artifacts}"
DRY_RUN=0
RESUME=0

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
  echo "Error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "No se encontró el comando requerido: $1"
}

usage() {
  cat <<'USAGE'
Uso:
  release-github.sh [--dry-run] [--resume] <version>

Ejemplo:
  release-github.sh 0.10.1rc1

Variables opcionales:
  GIT_REMOTE     remoto git para push y tag. Por defecto: github
  ARTIFACTS_DIR  directorio base temporal de artefactos. Por defecto: .release-artifacts
  OUTPUT_DIR     si se define, se pasa a buildall.sh tal cual
  LINUX_REMOTE   host Linux remoto para buildall.sh
  MAC_REMOTE     host macOS remoto para buildall.sh si local no es macOS
  WINDOWS_REMOTE host Windows remoto para buildall.sh
  RELEASE_LOG_DIR directorio donde guardar logs por fase
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --resume)
      RESUME=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      break
      ;;
  esac
done

[[ $# -eq 1 ]] || {
  usage
  exit 1
}

VERSION="$1"
TAG="v${VERSION}"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([A-Za-z0-9.-]*)?$ ]] || fail "Versión no válida: ${VERSION}"
BASE_VERSION="$(printf '%s' "${VERSION}" | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
[[ -n "${BASE_VERSION}" ]] || fail "No se pudo resolver la versión base desde ${VERSION}"
LOG_DIR="${RELEASE_LOG_DIR:-${ARTIFACTS_ROOT}/logs/${VERSION}}"

require_cmd git
require_cmd gh
require_cmd perl
require_cmd sed
require_cmd tee

cd "${PROJECT_ROOT}"
[[ -f "${CMAKE_FILE}" ]] || fail "No se encontró ${CMAKE_FILE}"

gh auth status >/dev/null 2>&1 || fail "gh no está autenticado. Ejecuta 'gh auth login' primero."
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "No es un repositorio git"

git diff --quiet || fail "El árbol de trabajo tiene cambios sin commit"
git diff --cached --quiet || fail "Hay cambios staged sin commit"
[[ -z "$(git status --short --untracked-files=all)" ]] || fail "Hay cambios pendientes o ficheros sin versionar"

git rev-parse --verify "${TAG}" >/dev/null 2>&1 && fail "El tag ${TAG} ya existe en local"
git ls-remote --tags "${GIT_REMOTE}" "refs/tags/${TAG}" | grep -q . && fail "El tag ${TAG} ya existe en ${GIT_REMOTE}"
if gh release view "${TAG}" >/dev/null 2>&1; then
  fail "La release ${TAG} ya existe en GitHub"
fi

CURRENT_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${CMAKE_FILE}" | head -n1)"
[[ -n "${CURRENT_VERSION}" ]] || fail "No se pudo leer ZFSMGR_APP_VERSION_STRING"

run_logged() {
  local phase="$1"
  shift
  mkdir -p "${LOG_DIR}"
  log "Fase ${phase}: guardando log en ${LOG_DIR}/${phase}.log"
  "$@" 2>&1 | tee "${LOG_DIR}/${phase}.log"
}

if [[ "${DRY_RUN}" -eq 1 ]]; then
  cat <<EOF
Dry run de release GitHub
  version actual: ${CURRENT_VERSION}
  version objetivo: ${VERSION}
  version base CMake: ${BASE_VERSION}
  modo resume: ${RESUME}
  remoto git: ${GIT_REMOTE}
  tag: ${TAG}
  directorio de artefactos: ${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}
  directorio de logs: ${LOG_DIR}

Fases previstas:
  1. Actualizar resources/CMakeLists.txt a ${VERSION}
  2. Crear commit \"Release ${VERSION}\" si hay cambios
  3. Hacer push a ${GIT_REMOTE}
  4. Ejecutar scripts/buildall.sh
  5. Crear y subir tag ${TAG}
  6. Crear release ${TAG} en GitHub y subir artefactos
EOF
  exit 0
fi

if [[ "${RESUME}" -eq 1 ]]; then
  [[ "${CURRENT_VERSION}" == "${VERSION}" ]] || fail "--resume requiere que la versión actual ya sea ${VERSION}"
  log "Modo resume: se reutiliza la versión ${VERSION} ya aplicada"
else
  log "Actualizando versión ${CURRENT_VERSION} -> ${VERSION}"
  perl -0pi -e 's/project\(ZFSMgrQt VERSION\s+[^ )]+/project(ZFSMgrQt VERSION '"${BASE_VERSION}"'/; s/set\(ZFSMGR_APP_VERSION_STRING\s+"[^"]+"/set(ZFSMGR_APP_VERSION_STRING "'"${VERSION}"'"/' "${CMAKE_FILE}"
fi

UPDATED_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${CMAKE_FILE}" | head -n1)"
[[ "${UPDATED_VERSION}" == "${VERSION}" ]] || fail "No se pudo actualizar la versión en CMakeLists.txt"

git add "${CMAKE_FILE}"
if git diff --cached --quiet; then
  if [[ "${RESUME}" -eq 1 ]]; then
    log "Modo resume: no se crea commit nuevo"
  else
    log "La versión ${VERSION} ya estaba aplicada; continúo sin crear commit nuevo"
  fi
else
  git commit -m "Release ${VERSION}"
fi

log "Publicando commit de release en ${GIT_REMOTE}"
run_logged git-push git push "${GIT_REMOTE}" HEAD
BUILD_REF="$(git rev-parse HEAD)"
log "Commit exacto de release para builders remotos: ${BUILD_REF}"

ARTIFACTS_DIR="${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}"
rm -rf "${ARTIFACTS_DIR}"
mkdir -p "${ARTIFACTS_DIR}"
mkdir -p "${LOG_DIR}"

log "Ejecutando buildall.sh con artefactos en ${ARTIFACTS_DIR}"
run_logged buildall env OUTPUT_DIR="${ARTIFACTS_DIR}" BUILD_GIT_REMOTE="${GIT_REMOTE}" BUILD_GIT_REF="${BUILD_REF}" "${SCRIPT_DIR}/buildall.sh"

MAC_ARTIFACT="$(find "${ARTIFACTS_DIR}" -maxdepth 1 -type f -name "ZFSMgr-${VERSION}.app.zip" | head -n1)"
WIN_ARTIFACT="$(find "${ARTIFACTS_DIR}" -maxdepth 1 -type f -name "ZFSMgr-Setup-${VERSION}*.exe" | head -n1)"
LINUX_APPIMAGE="$(find "${ARTIFACTS_DIR}" -maxdepth 1 -type f -name "ZFSMgr-${VERSION}-*.AppImage" | head -n1)"
LINUX_DEB="$(find "${ARTIFACTS_DIR}" -maxdepth 1 -type f -name "zfsmgr_${VERSION}_*.deb" | head -n1)"

[[ -n "${MAC_ARTIFACT}" && -f "${MAC_ARTIFACT}" ]] || fail "No se encontró el artefacto macOS (.app.zip)"
[[ -n "${WIN_ARTIFACT}" && -f "${WIN_ARTIFACT}" ]] || fail "No se encontró el artefacto Windows (.exe)"
[[ -n "${LINUX_APPIMAGE}" && -f "${LINUX_APPIMAGE}" ]] || fail "No se encontró el artefacto Linux (.AppImage)"
[[ -n "${LINUX_DEB}" && -f "${LINUX_DEB}" ]] || fail "No se encontró el artefacto Linux (.deb)"

log "Creando tag ${TAG}"
git tag "${TAG}"
run_logged git-push-tag git push "${GIT_REMOTE}" "${TAG}"

log "Creando release ${TAG} en GitHub"
run_logged github-release gh release create "${TAG}" \
  "${WIN_ARTIFACT}" \
  "${LINUX_APPIMAGE}" \
  "${LINUX_DEB}" \
  "${MAC_ARTIFACT}" \
  --title "${TAG}" \
  --verify-tag \
  --generate-notes

log "Release creada correctamente"
printf 'Tag: %s\n' "${TAG}"
printf 'Logs:\n'
printf '  %s\n' "${LOG_DIR}/git-push.log" "${LOG_DIR}/buildall.log" "${LOG_DIR}/git-push-tag.log" "${LOG_DIR}/github-release.log"
printf 'Artefactos:\n'
printf '  %s\n' "${WIN_ARTIFACT}" "${LINUX_APPIMAGE}" "${LINUX_DEB}" "${MAC_ARTIFACT}"
