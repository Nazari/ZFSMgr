#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMAKE_FILE="${PROJECT_ROOT}/resources/CMakeLists.txt"
GIT_REMOTE="${GIT_REMOTE:-github}"
ARTIFACTS_ROOT="${ARTIFACTS_DIR:-${PROJECT_ROOT}/.release-artifacts}"
DRY_RUN=0
RESUME=0
SKIP_BUILD=0
ONLY_RELEASE=0
BUILD_PLATFORMS_OVERRIDE=""
BUILDALL_PASSWORD=""

preferred_downloads_dir() {
  if [[ -d "${HOME}/Descargas" ]]; then
    printf '%s\n' "${HOME}/Descargas"
    return 0
  fi
  if [[ -d "${HOME}/Downloads" ]]; then
    printf '%s\n' "${HOME}/Downloads"
    return 0
  fi
  printf '%s\n' "${HOME}"
}

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
  release-github.sh [--dry-run] [--resume] [--skip-build] [--only-release] [--platforms mac,linux,windows] [--password <keychain-password>] <version>

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
  --password, -p Password del login keychain a reenviar a buildall.sh para firmar en macOS

Artefactos publicados:
  - Windows .exe
  - Linux .AppImage
  - Linux .deb
  - macOS .dmg (uno o varios, según arquitectura)
  - FreeBSD .tar.gz/.tar.bz2

Resolución de artefactos:
  Primero se buscan en ARTIFACTS_DIR/.release-artifacts y, si no están ahí,
  en ~/Descargas/z o ~/Downloads/z, que es donde buildall.sh deja los ficheros
  al usar --sftpfc16.
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
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --only-release)
      ONLY_RELEASE=1
      shift
      ;;
    --platforms)
      shift
      [[ $# -gt 0 ]] || fail "--platforms requiere una lista separada por comas"
      BUILD_PLATFORMS_OVERRIDE="$1"
      shift
      ;;
    --password|-p)
      shift
      [[ $# -gt 0 ]] || fail "--password requiere un valor"
      BUILDALL_PASSWORD="$1"
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
STATE_FILE="${ARTIFACTS_ROOT}/state/${VERSION}/release-state.json"
ARTIFACTS_DIR="${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}"
BUILDALL_PLATFORM_LOG_DIR="${LOG_DIR}/buildall-platforms"
MAC_ARTIFACTS=()
MAC_ARTIFACTS_JSON=""
WIN_ARTIFACT=""
LINUX_APPIMAGE=""
LINUX_DEB=""
FREEBSD_PKG=""

if [[ "${ONLY_RELEASE}" -eq 1 ]]; then
  RESUME=1
  SKIP_BUILD=1
fi

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

RELEASE_EXISTS=0
if gh release view "${TAG}" >/dev/null 2>&1; then
  RELEASE_EXISTS=1
fi

LOCAL_TAG_EXISTS=0
REMOTE_TAG_EXISTS=0
git rev-parse --verify "${TAG}" >/dev/null 2>&1 && LOCAL_TAG_EXISTS=1
git ls-remote --tags "${GIT_REMOTE}" "refs/tags/${TAG}" | grep -q . && REMOTE_TAG_EXISTS=1

if [[ "${RESUME}" -eq 0 ]]; then
  [[ "${LOCAL_TAG_EXISTS}" -eq 0 ]] || fail "El tag ${TAG} ya existe en local"
  [[ "${REMOTE_TAG_EXISTS}" -eq 0 ]] || fail "El tag ${TAG} ya existe en ${GIT_REMOTE}"
fi

CURRENT_VERSION="$(sed -nE 's/^[[:space:]]*set\([[:space:]]*ZFSMGR_APP_VERSION_STRING[[:space:]]*"([^"]+)".*/\1/p' "${CMAKE_FILE}" | head -n1)"
[[ -n "${CURRENT_VERSION}" ]] || fail "No se pudo leer ZFSMGR_APP_VERSION_STRING"

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/\\n}"
  s="${s//$'\r'/\\r}"
  s="${s//$'\t'/\\t}"
  printf '%s' "${s}"
}

join_by() {
  local sep="$1"
  shift || true
  local out=""
  local first=1
  local item
  for item in "$@"; do
    if [[ ${first} -eq 1 ]]; then
      out="${item}"
      first=0
    else
      out="${out}${sep}${item}"
    fi
  done
  printf '%s' "${out}"
}

downloads_artifacts_dir() {
  printf '%s/z\n' "$(preferred_downloads_dir)"
}

find_one_release_artifact() {
  local dir1="$1"
  local dir2="$2"
  local dir3="$3"
  shift 3
  local dir pattern found=""
  for dir in "${dir1}" "${dir2}" "${dir3}"; do
    [[ -n "${dir}" && -d "${dir}" ]] || continue
    for pattern in "$@"; do
      found="$(find "${dir}" -maxdepth 1 -type f -name "${pattern}" | sort -V | tail -n1)"
      if [[ -n "${found}" ]]; then
        printf '%s\n' "${found}"
        return 0
      fi
    done
  done
  return 1
}

find_many_release_artifacts() {
  local dir1="$1"
  local dir2="$2"
  local dir3="$3"
  shift 3
  local dir pattern
  local -a found=()
  for dir in "${dir1}" "${dir2}" "${dir3}"; do
    [[ -n "${dir}" && -d "${dir}" ]] || continue
    while IFS= read -r path; do
      [[ -n "${path}" ]] || continue
      found+=("${path}")
    done < <(
      for pattern in "$@"; do
        find "${dir}" -maxdepth 1 -type f -name "${pattern}"
      done | sort -u
    )
    if [[ ${#found[@]} -gt 0 ]]; then
      printf '%s\n' "${found[@]}"
      return 0
    fi
  done
  return 1
}

resolve_release_artifacts() {
  local artifacts_dir="$1"
  local fallback_dir="$2"
  local linux_build_dir="${PROJECT_ROOT}/build-linux"
  local freebsd_build_dir="${PROJECT_ROOT}/build-freebsd"
  MAC_ARTIFACTS=()
  while IFS= read -r path; do
    [[ -n "${path}" ]] || continue
    MAC_ARTIFACTS+=("${path}")
  done < <(find_many_release_artifacts "${artifacts_dir}" "${fallback_dir}" "" "ZFSMgr-${VERSION}_*.dmg" "ZFSMgr-${VERSION}.dmg" || true)
  MAC_ARTIFACTS_JSON="$(join_by ";" "${MAC_ARTIFACTS[@]}")"
  WIN_ARTIFACT="$(find_one_release_artifact "${artifacts_dir}" "${fallback_dir}" "" "ZFSMgr-Setup-${VERSION}*.exe" || true)"
  LINUX_APPIMAGE="$(find_one_release_artifact "${artifacts_dir}" "${fallback_dir}" "${linux_build_dir}" "ZFSMgr-${VERSION}-*.AppImage" || true)"
  LINUX_DEB="$(find_one_release_artifact "${artifacts_dir}" "${fallback_dir}" "${linux_build_dir}" "zfsmgr_${VERSION}_*.deb" || true)"
  FREEBSD_PKG="$(find_one_release_artifact "${artifacts_dir}" "${fallback_dir}" "${freebsd_build_dir}" "*${VERSION}*-FreeBSD.tar.gz" "*${VERSION}*-FreeBSD.tar.bz2" "*${VERSION}*.tar.gz" "*${VERSION}*.tar.bz2" || true)"
}

write_state() {
  local phase="$1"
  local status="$2"
  local message="${3:-}"
  mkdir -p "$(dirname "${STATE_FILE}")"
  cat > "${STATE_FILE}" <<EOF
{
  "version": "$(json_escape "${VERSION}")",
  "tag": "$(json_escape "${TAG}")",
  "phase": "$(json_escape "${phase}")",
  "status": "$(json_escape "${status}")",
  "message": "$(json_escape "${message}")",
  "current_version": "$(json_escape "${CURRENT_VERSION}")",
  "resume": ${RESUME},
  "dry_run": ${DRY_RUN},
  "skip_build": ${SKIP_BUILD},
  "only_release": ${ONLY_RELEASE},
  "build_platforms": "$(json_escape "${BUILD_PLATFORMS_OVERRIDE:-mac,linux,windows}")",
  "git_remote": "$(json_escape "${GIT_REMOTE}")",
  "local_tag_exists": ${LOCAL_TAG_EXISTS},
  "remote_tag_exists": ${REMOTE_TAG_EXISTS},
  "release_exists": ${RELEASE_EXISTS},
  "state_file": "$(json_escape "${STATE_FILE}")",
  "artifacts_dir": "$(json_escape "${ARTIFACTS_DIR}")",
  "buildall_platform_log_dir": "$(json_escape "${BUILDALL_PLATFORM_LOG_DIR}")",
  "artifacts": {
    "mac_dmg": "$(json_escape "${MAC_ARTIFACTS_JSON}")",
    "windows_exe": "$(json_escape "${WIN_ARTIFACT}")",
    "linux_appimage": "$(json_escape "${LINUX_APPIMAGE}")",
    "linux_deb": "$(json_escape "${LINUX_DEB}")",
    "freebsd_pkg": "$(json_escape "${FREEBSD_PKG}")"
  }
}
EOF
}

on_error() {
  local exit_code=$?
  write_state "failed" "failed" "ejecución abortada con código ${exit_code}"
  exit "${exit_code}"
}

trap on_error ERR

run_logged() {
  local phase="$1"
  shift
  mkdir -p "${LOG_DIR}"
  log "Fase ${phase}: guardando log en ${LOG_DIR}/${phase}.log"
  write_state "${phase}" "running" "fase en ejecución"
  "$@" 2>&1 | tee "${LOG_DIR}/${phase}.log"
  write_state "${phase}" "completed" "fase completada"
}

if [[ "${DRY_RUN}" -eq 1 ]]; then
  write_state "dry-run" "planned" "simulación sin cambios"
  cat <<EOF
Dry run de release GitHub
  version actual: ${CURRENT_VERSION}
  version objetivo: ${VERSION}
  version base CMake: ${BASE_VERSION}
  modo resume: ${RESUME}
  skip build: ${SKIP_BUILD}
  only release: ${ONLY_RELEASE}
  plataformas build: ${BUILD_PLATFORMS_OVERRIDE:-mac,linux,windows}
  password buildall: $([[ -n "${BUILDALL_PASSWORD}" ]] && printf 'sí' || printf 'no')
  remoto git: ${GIT_REMOTE}
  tag: ${TAG}
  directorio de artefactos: ${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}
  directorio de logs: ${LOG_DIR}
  directorio de logs buildall por plataforma: ${BUILDALL_PLATFORM_LOG_DIR}
  fichero de estado: ${STATE_FILE}
  tag local existente: ${LOCAL_TAG_EXISTS}
  tag remoto existente: ${REMOTE_TAG_EXISTS}
  release existente: ${RELEASE_EXISTS}

Fases previstas:
  1. Actualizar resources/CMakeLists.txt a ${VERSION}
  2. Crear commit \"Release ${VERSION}\" si hay cambios
  3. Hacer push a ${GIT_REMOTE}
  4. Ejecutar scripts/buildall.sh, reutilizar artefactos o saltar build
  5. Crear y subir tag ${TAG} o reutilizarlo si ya existe en resume
  6. Crear release ${TAG} en GitHub y subir artefactos
EOF
  exit 0
fi

[[ "${RELEASE_EXISTS}" -eq 0 ]] || fail "La release ${TAG} ya existe en GitHub"
write_state "preflight" "completed" "preflight correcto"

if [[ "${RESUME}" -eq 1 ]]; then
  [[ "${CURRENT_VERSION}" == "${VERSION}" ]] || fail "--resume requiere que la versión actual ya sea ${VERSION}"
  log "Modo resume: se reutiliza la versión ${VERSION} ya aplicada"
  if [[ "${REMOTE_TAG_EXISTS}" -eq 1 && "${LOCAL_TAG_EXISTS}" -eq 0 ]]; then
    log "Modo resume: recuperando tag ${TAG} desde ${GIT_REMOTE}"
    git fetch "${GIT_REMOTE}" "refs/tags/${TAG}:refs/tags/${TAG}"
    LOCAL_TAG_EXISTS=1
  fi
else
  log "Actualizando versión ${CURRENT_VERSION} -> ${VERSION}"
  perl -0pi -e 's/project\(ZFSMgrQt VERSION\s+[^ )]+/project(ZFSMgrQt VERSION '"${BASE_VERSION}"'/; s/set\(ZFSMGR_APP_VERSION_STRING\s+"[^"]+"/set(ZFSMGR_APP_VERSION_STRING "'"${VERSION}"'"/' "${CMAKE_FILE}"
fi
write_state "version" "completed" "versión preparada"

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
write_state "commit" "completed" "commit de release preparado"

log "Publicando commit de release en ${GIT_REMOTE}"
run_logged git-push git push "${GIT_REMOTE}" HEAD
BUILD_REF="$(git rev-parse HEAD)"
log "Commit exacto de release para builders remotos: ${BUILD_REF}"
write_state "git-push" "completed" "commit publicado"

ARTIFACTS_DIR="${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}"
mkdir -p "${ARTIFACTS_DIR}"
mkdir -p "${LOG_DIR}"
SFTP_FALLBACK_DIR="$(downloads_artifacts_dir)"
resolve_release_artifacts "${ARTIFACTS_DIR}" "${SFTP_FALLBACK_DIR}"

HAS_ALL_ARTIFACTS=0
if [[ ${#MAC_ARTIFACTS[@]} -gt 0 && -n "${WIN_ARTIFACT}" && -f "${WIN_ARTIFACT}" && -n "${LINUX_APPIMAGE}" && -f "${LINUX_APPIMAGE}" && -n "${LINUX_DEB}" && -f "${LINUX_DEB}" && -n "${FREEBSD_PKG}" && -f "${FREEBSD_PKG}" ]]; then
  HAS_ALL_ARTIFACTS=1
fi

if [[ "${SKIP_BUILD}" -eq 1 ]]; then
  log "Modo skip-build: no se ejecuta buildall.sh"
  write_state "buildall" "skipped" "build omitido por opción"
elif [[ "${RESUME}" -eq 1 && "${HAS_ALL_ARTIFACTS}" -eq 1 ]]; then
  log "Modo resume: se reutilizan los artefactos ya existentes en ${ARTIFACTS_DIR}"
  write_state "buildall" "reused" "artefactos reutilizados"
else
  if [[ "${RESUME}" -eq 0 ]]; then
    rm -rf "${ARTIFACTS_DIR}"
  fi
  mkdir -p "${ARTIFACTS_DIR}"
  log "Ejecutando buildall.sh con artefactos en ${ARTIFACTS_DIR}"
  BUILDALL_CMD=(env
    OUTPUT_DIR="${ARTIFACTS_DIR}"
    BUILDALL_LOG_DIR="${BUILDALL_PLATFORM_LOG_DIR}"
    BUILD_GIT_REMOTE="${GIT_REMOTE}"
    BUILD_GIT_REF="${BUILD_REF}"
    BUILD_PLATFORMS="${BUILD_PLATFORMS_OVERRIDE:-mac,linux,windows}"
    "${SCRIPT_DIR}/buildall.sh"
  )
  if [[ -n "${BUILDALL_PASSWORD}" ]]; then
    BUILDALL_CMD+=("--password" "${BUILDALL_PASSWORD}")
  fi
  run_logged buildall "${BUILDALL_CMD[@]}"
  resolve_release_artifacts "${ARTIFACTS_DIR}" "${SFTP_FALLBACK_DIR}"
fi

[[ ${#MAC_ARTIFACTS[@]} -gt 0 ]] || fail "No se encontró ningún artefacto macOS (.dmg)"
[[ -n "${WIN_ARTIFACT}" && -f "${WIN_ARTIFACT}" ]] || fail "No se encontró el artefacto Windows (.exe)"
[[ -n "${LINUX_APPIMAGE}" && -f "${LINUX_APPIMAGE}" ]] || fail "No se encontró el artefacto Linux (.AppImage)"
[[ -n "${LINUX_DEB}" && -f "${LINUX_DEB}" ]] || fail "No se encontró el artefacto Linux (.deb)"
[[ -n "${FREEBSD_PKG}" && -f "${FREEBSD_PKG}" ]] || fail "No se encontró el artefacto FreeBSD (.tar.gz/.tar.bz2)"
log "Artefactos resueltos:"
for _mac in "${MAC_ARTIFACTS[@]}"; do
  log "  macOS: ${_mac}"
done
log "  Windows: ${WIN_ARTIFACT}"
log "  Linux AppImage: ${LINUX_APPIMAGE}"
log "  Linux DEB: ${LINUX_DEB}"
log "  FreeBSD: ${FREEBSD_PKG}"
write_state "artifacts" "completed" "artefactos disponibles"

if [[ "${LOCAL_TAG_EXISTS}" -eq 1 ]]; then
  log "Modo resume: reutilizando tag local ${TAG}"
elif [[ "${REMOTE_TAG_EXISTS}" -eq 1 ]]; then
  log "Modo resume: el tag ${TAG} ya existe en ${GIT_REMOTE}"
else
  [[ "${ONLY_RELEASE}" -eq 0 ]] || fail "--only-release requiere que el tag ${TAG} ya exista"
  log "Creando tag ${TAG}"
  git tag "${TAG}"
  LOCAL_TAG_EXISTS=1
fi

if [[ "${REMOTE_TAG_EXISTS}" -eq 1 ]]; then
  log "Modo resume: el tag ${TAG} ya estaba publicado en ${GIT_REMOTE}"
  write_state "git-push-tag" "reused" "tag ya publicado"
else
  run_logged git-push-tag git push "${GIT_REMOTE}" "${TAG}"
fi

log "Creando release ${TAG} en GitHub"
GH_RELEASE_CMD=(gh release create "${TAG}"
  "${WIN_ARTIFACT}"
  "${LINUX_APPIMAGE}"
  "${LINUX_DEB}"
  "${FREEBSD_PKG}"
  --title "${TAG}"
  --verify-tag
  --generate-notes
)
for _mac in "${MAC_ARTIFACTS[@]}"; do
  GH_RELEASE_CMD+=("${_mac}")
done
run_logged github-release "${GH_RELEASE_CMD[@]}"

log "Release creada correctamente"
write_state "github-release" "completed" "release creada correctamente"
printf 'Tag: %s\n' "${TAG}"
printf 'Logs:\n'
printf '  %s\n' "${LOG_DIR}/git-push.log" "${LOG_DIR}/buildall.log" "${LOG_DIR}/git-push-tag.log" "${LOG_DIR}/github-release.log"
printf 'Logs buildall por plataforma:\n'
printf '  %s\n' "${BUILDALL_PLATFORM_LOG_DIR}/macos-local.log" "${BUILDALL_PLATFORM_LOG_DIR}/macos-remote.log" "${BUILDALL_PLATFORM_LOG_DIR}/linux-remote.log" "${BUILDALL_PLATFORM_LOG_DIR}/windows-remote.log"
printf 'Estado:\n'
printf '  %s\n' "${STATE_FILE}"
printf 'Artefactos:\n'
for _mac in "${MAC_ARTIFACTS[@]}"; do
  printf '  %s\n' "${_mac}"
done
printf '  %s\n' "${WIN_ARTIFACT}" "${LINUX_APPIMAGE}" "${LINUX_DEB}" "${FREEBSD_PKG}"
