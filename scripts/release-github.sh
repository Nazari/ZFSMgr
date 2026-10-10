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
  release-github.sh [--dry-run] [--resume] [--skip-build] [--only-release] [--platforms linux,windows,freebsd,macos] <version>

Ejemplo:
  release-github.sh 0.10.1rc1

Variables opcionales:
  GIT_REMOTE     remoto git para push y tag. Por defecto: github
  ARTIFACTS_DIR  directorio base temporal de artefactos. Por defecto: .release-artifacts
  OUTPUT_DIR     si se define, se usa como directorio local de artefactos
  RELEASE_LOG_DIR directorio donde guardar logs por fase

Artefactos publicados (los genera scripts/empaqueta.sh, que llama buildall-cross.sh):
  - zfsmgr-cli-<version>.zip    solo CLI, un instalador por sistema (desde la 1.0.0 es lo
                                único que se publica; el completo con interfaz no se sube)
  - y la suma SHA-256 de cada uno

Resolución de artefactos:
  Primero se buscan en ARTIFACTS_DIR y, si no están ahí, en builds/ locales.
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
ZIP_COMPLETO=""
ZIP_CLI=""
RELEASE_FILES=()

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



resolve_release_artifacts() {
  # Dos zip y sus sumas, y nada más. Antes eran cinco artefactos sueltos —Setup, AppImage,
  # .deb, .app.zip por arquitectura y .pkg de FreeBSD—; ahora todo va dentro de estos dos,
  # con un instalador por sistema. Ver scripts/empaqueta.sh.
  local artifacts_dir="$1"
  ZIP_COMPLETO="${artifacts_dir}/zfsmgr-${VERSION}.zip"
  ZIP_CLI="${artifacts_dir}/zfsmgr-cli-${VERSION}.zip"
  RELEASE_FILES=()
  local f
  # Desde la 1.0.0 se publica SOLO el CLI: la interfaz gráfica no está probada y no se
  # espera hasta la 2.0.0. El zip completo se sigue generando, pero no se sube.
  for f in "${ZIP_CLI}" "${ZIP_CLI}.sha256"; do
    [[ -f "${f}" ]] && RELEASE_FILES+=("${f}")
  done
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
  "build_platforms": "$(json_escape "${BUILD_PLATFORMS_OVERRIDE:-linux,windows,freebsd,macos}")",
  "git_remote": "$(json_escape "${GIT_REMOTE}")",
  "local_tag_exists": ${LOCAL_TAG_EXISTS},
  "remote_tag_exists": ${REMOTE_TAG_EXISTS},
  "release_exists": ${RELEASE_EXISTS},
  "state_file": "$(json_escape "${STATE_FILE}")",
  "artifacts_dir": "$(json_escape "${ARTIFACTS_DIR}")",
  "buildall_platform_log_dir": "$(json_escape "${BUILDALL_PLATFORM_LOG_DIR}")",
  "artifacts": {
    "zip_completo": "$(json_escape "${ZIP_COMPLETO:-}")",
    "zip_cli": "$(json_escape "${ZIP_CLI:-}")"
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
  plataformas build: ${BUILD_PLATFORMS_OVERRIDE:-linux,windows,freebsd,macos}
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
  4. Ejecutar scripts/buildall-cross.sh, reutilizar artefactos o saltar build
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
log "Commit exacto de release: ${BUILD_REF}"
write_state "git-push" "completed" "commit publicado"

# Los agentes que se empaquetan salen de builds/agents/, que no está versionado y
# que nada mantiene al día. Una vez se quedó en 0.90.0 mientras la aplicación
# esperaba 0.90.5, y el efecto fue que reinstalar el daemon no lo actualizaba nunca.
# Aquí se comprueba que los cinco existen y declaran ESTA versión; si no, se
# aborta antes de generar ningún artefacto.
log "Comprobando los agentes desplegables"
agents_ok=1
for _rel in linux-x86_64/zfsmgr_agent windows-x86_64/zfsmgr_agent.exe \
            freebsd-x86_64/zfsmgr_agent macos-arm64/zfsmgr_agent macos-amd64/zfsmgr_agent; do
  _bin="${PROJECT_ROOT}/builds/agents/${_rel}"
  if [[ ! -f "${_bin}" ]]; then
    echo "Error: falta el agente ${_rel}." >&2
    agents_ok=0
    continue
  fi
  # Sin -q y contando coincidencias a propósito: con "set -o pipefail", grep -q sale
  # en cuanto encuentra la primera, strings recibe SIGPIPE y la tubería se reporta
  # como fallida AUNQUE la coincidencia existiera. Es decir, la comprobación decía
  # que ningún agente declaraba la versión justo cuando todos la declaraban.
  _hits="$(strings -a "${_bin}" | grep -cE "${VERSION//./\\.}\.[0-9]{6,9}" || true)"
  if [[ "${_hits}" -eq 0 ]]; then
    echo "Error: ${_rel} no declara la versión ${VERSION}." >&2
    agents_ok=0
  fi
done
if [[ "${agents_ok}" -ne 1 ]]; then
  echo "Recompílelos, o tráigalos de la integración continua con:" >&2
  echo "  scripts/fetch-agents.sh --ref ${BUILD_REF}" >&2
  exit 1
fi
log "Agentes correctos para ${VERSION}"

ARTIFACTS_DIR="${OUTPUT_DIR:-${ARTIFACTS_ROOT}/${VERSION}}"
mkdir -p "${ARTIFACTS_DIR}"
mkdir -p "${LOG_DIR}"
resolve_release_artifacts "${ARTIFACTS_DIR}" ""

HAS_ALL_ARTIFACTS=0
[[ ${#RELEASE_FILES[@]} -eq 4 ]] && HAS_ALL_ARTIFACTS=1

if [[ "${SKIP_BUILD}" -eq 1 ]]; then
  log "Modo skip-build: no se ejecuta buildall-cross.sh"
  write_state "buildall" "skipped" "build omitido por opción"
elif [[ "${RESUME}" -eq 1 && "${HAS_ALL_ARTIFACTS}" -eq 1 ]]; then
  log "Modo resume: se reutilizan los artefactos ya existentes en ${ARTIFACTS_DIR}"
  write_state "buildall" "reused" "artefactos reutilizados"
else
  if [[ "${RESUME}" -eq 0 ]]; then
    rm -rf "${ARTIFACTS_DIR}"
  fi
  mkdir -p "${ARTIFACTS_DIR}"
  log "Ejecutando buildall-cross.sh con artefactos en ${ARTIFACTS_DIR}"
  BUILDALL_CMD=(env
    OUTPUT_DIR="${ARTIFACTS_DIR}"
    BUILDALL_LOG_DIR="${BUILDALL_PLATFORM_LOG_DIR}"
    BUILD_PLATFORMS="${BUILD_PLATFORMS_OVERRIDE:-linux,windows,freebsd,macos}"
    "${SCRIPT_DIR}/buildall-cross.sh"
  )
  run_logged buildall "${BUILDALL_CMD[@]}"
  resolve_release_artifacts "${ARTIFACTS_DIR}" ""
fi

[[ ${#RELEASE_FILES[@]} -eq 2 ]] \
  || fail "falta el zip de CLI de ${VERSION} o su suma en ${ARTIFACTS_DIR} (los genera scripts/empaqueta.sh)"
log "Artefactos resueltos:"
for _f in "${RELEASE_FILES[@]}"; do
  log "  ${_f}"
done
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
  "${RELEASE_FILES[@]}"
  --title "${TAG}"
  --verify-tag
  --generate-notes
)
# Las notas escritas a mano van DELANTE de las generadas: son las que dicen el alcance.
NOTAS="${PROJECT_ROOT:-${SCRIPT_DIR}/..}/docs/notas/${VERSION}.md"
if [[ -f "${NOTAS}" ]]; then
  GH_RELEASE_CMD+=(--notes-file "${NOTAS}")
fi
run_logged github-release "${GH_RELEASE_CMD[@]}"

log "Release creada correctamente"
write_state "github-release" "completed" "release creada correctamente"
printf 'Tag: %s\n' "${TAG}"
printf 'Logs:\n'
printf '  %s\n' "${LOG_DIR}/git-push.log" "${LOG_DIR}/buildall.log" "${LOG_DIR}/git-push-tag.log" "${LOG_DIR}/github-release.log"
printf 'Logs buildall por plataforma:\n'
printf '  %s\n' "${BUILDALL_PLATFORM_LOG_DIR}/linux-local.log" "${BUILDALL_PLATFORM_LOG_DIR}/windows-local.log" "${BUILDALL_PLATFORM_LOG_DIR}/freebsd-local.log" "${BUILDALL_PLATFORM_LOG_DIR}/macos-local.log"
printf 'Estado:\n'
printf '  %s\n' "${STATE_FILE}"
printf 'Artefactos:\n'
printf '  %s\n' "${RELEASE_FILES[@]}"
