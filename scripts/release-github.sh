#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMAKE_FILE="${PROJECT_ROOT}/resources/CMakeLists.txt"
GIT_REMOTE="${GIT_REMOTE:-github}"
ARTIFACTS_ROOT="${ARTIFACTS_DIR:-${PROJECT_ROOT}/.release-artifacts}"

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
  release-github.sh <version>

Ejemplo:
  release-github.sh 0.10.1rc1

Variables opcionales:
  GIT_REMOTE     remoto git para push y tag. Por defecto: github
  ARTIFACTS_DIR  directorio base temporal de artefactos. Por defecto: .release-artifacts
  OUTPUT_DIR     si se define, se pasa a buildall.sh tal cual
  LINUX_REMOTE   host Linux remoto para buildall.sh
  MAC_REMOTE     host macOS remoto para buildall.sh si local no es macOS
  WINDOWS_REMOTE host Windows remoto para buildall.sh
USAGE
}

[[ $# -eq 1 ]] || {
  usage
  exit 1
}

VERSION="$1"
TAG="v${VERSION}"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([A-Za-z0-9.-]*)?$ ]] || fail "Versión no válida: ${VERSION}"
BASE_VERSION="$(printf '%s' "${VERSION}" | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
[[ -n "${BASE_VERSION}" ]] || fail "No se pudo resolver la versión base desde ${VERSION}"

require_cmd git
require_cmd gh
require_cmd perl
require_cmd sed

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

log "Actualizando versión ${CURRENT_VERSION} -> ${VERSION}"
perl -0pi -e 's/project\(ZFSMgrQt VERSION\s+[^ )]+/project(ZFSMgrQt VERSION '"${BASE_VERSION}"'/; s/set\(ZFSMGR_APP_VERSION_STRING\s+"[^"]+"/set(ZFSMGR_APP_VERSION_STRING "'"${VERSION}"'"/' "${CMAKE_FILE}"

UPDATED_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${CMAKE_FILE}" | head -n1)"
[[ "${UPDATED_VERSION}" == "${VERSION}" ]] || fail "No se pudo actualizar la versión en CMakeLists.txt"

git add "${CMAKE_FILE}"
git commit -m "Release ${VERSION}"

log "Publicando commit de release en ${GIT_REMOTE}"
git push "${GIT_REMOTE}" HEAD

ARTIFACTS_DIR="${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}"
rm -rf "${ARTIFACTS_DIR}"
mkdir -p "${ARTIFACTS_DIR}"

log "Ejecutando buildall.sh con artefactos en ${ARTIFACTS_DIR}"
OUTPUT_DIR="${ARTIFACTS_DIR}" "${SCRIPT_DIR}/buildall.sh"

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
git push "${GIT_REMOTE}" "${TAG}"

log "Creando release ${TAG} en GitHub"
gh release create "${TAG}" \
  "${WIN_ARTIFACT}" \
  "${LINUX_APPIMAGE}" \
  "${LINUX_DEB}" \
  "${MAC_ARTIFACT}" \
  --title "${TAG}" \
  --verify-tag \
  --generate-notes

log "Release creada correctamente"
printf 'Tag: %s\n' "${TAG}"
printf 'Artefactos:\n'
printf '  %s\n' "${WIN_ARTIFACT}" "${LINUX_APPIMAGE}" "${LINUX_DEB}" "${MAC_ARTIFACT}"
