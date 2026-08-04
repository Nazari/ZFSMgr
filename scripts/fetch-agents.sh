#!/usr/bin/env bash
# Trae a builds/agents/ los agentes que compiló la integración continua.
#
# Por qué existe: los binarios de builds/agents/ no están versionados y nada los
# mantiene al día salvo acordarse de recompilarlos. Se quedaron una vez en 0.90.0
# mientras la aplicación esperaba 0.90.5, y el síntoma fue que reinstalar el daemon
# no lo actualizaba nunca: se desplegaba una y otra vez el mismo binario viejo.
#
# Con esto, publicar deja de depender de que las cinco plataformas se hayan compilado
# a mano en el equipo de quien publica. El flujo de trabajo ya las compila todas y las
# sube como artefacto "zfsmgr-agents"; aquí solo se descargan y se comprueban.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${PROJECT_ROOT}/builds/agents"
WORKFLOW="build-packages.yml"
REF=""
RUN_ID=""

usage() {
  cat <<'USAGE'
Uso:
  fetch-agents.sh [--ref <commit|rama>] [--run-id <id>] [--dest <dir>]

Descarga el artefacto "zfsmgr-agents" del flujo de trabajo y lo deja en
builds/agents/, comprobando que están las cinco plataformas y que su versión
coincide con la que espera esta copia del proyecto.

Opciones:
  --ref <ref>      Commit o rama cuyo run se busca (por defecto: HEAD actual)
  --run-id <id>    Usa un run concreto en vez de buscarlo
  --dest <dir>     Destino (por defecto: builds/agents)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref) REF="$2"; shift 2 ;;
    --run-id) RUN_ID="$2"; shift 2 ;;
    --dest) DEST="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; usage >&2; exit 2 ;;
  esac
done

command -v gh >/dev/null || { echo "Falta 'gh' (GitHub CLI)." >&2; exit 2; }

# La versión que espera ESTA copia del proyecto. Si no coincide con la de los
# binarios descargados, desplegarlos dejaría a la aplicación pidiendo eternamente
# una actualización que nunca llega, que es justo el fallo que motivó este script.
app_version="$(sed -n 's/^set(ZFSMGR_APP_VERSION_STRING "\([^"]*\)").*/\1/p' \
  "${PROJECT_ROOT}/resources/CMakeLists.txt" | head -n1)"
[[ -n "${app_version}" ]] || { echo "No se pudo leer la versión del proyecto." >&2; exit 1; }

if [[ -z "${RUN_ID}" ]]; then
  REF="${REF:-$(git -C "${PROJECT_ROOT}" rev-parse HEAD)}"
  echo "[agents] buscando un run correcto de ${WORKFLOW} para ${REF}"
  RUN_ID="$(gh run list --workflow "${WORKFLOW}" --commit "${REF}" \
              --status success --limit 1 --json databaseId --jq '.[0].databaseId' 2>/dev/null || true)"
  if [[ -z "${RUN_ID}" || "${RUN_ID}" == "null" ]]; then
    echo "Error: no hay ningún run correcto de ${WORKFLOW} para ${REF}." >&2
    echo "Empuje el commit y espere a que termine, o indique --run-id." >&2
    exit 1
  fi
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
echo "[agents] descargando el artefacto del run ${RUN_ID}"
gh run download "${RUN_ID}" --name zfsmgr-agents --dir "${tmp}"

# Las cinco son las que pregunta findDeployableAgentBinaryPath. Si falta una, la
# aplicación no puede desplegar el daemon en esa plataforma y solo se descubre al
# intentarlo contra una máquina real.
expected=(linux-x86_64/zfsmgr_agent windows-x86_64/zfsmgr_agent.exe
          freebsd-x86_64/zfsmgr_agent macos-arm64/zfsmgr_agent macos-amd64/zfsmgr_agent)
missing=0
for rel in "${expected[@]}"; do
  [[ -f "${tmp}/${rel}" ]] || { echo "Error: falta ${rel} en el artefacto." >&2; missing=1; }
done
[[ "${missing}" -eq 0 ]] || exit 1

# La versión va embebida en el binario, así que se puede comprobar sin ejecutarlo:
# los de Windows, macOS y FreeBSD no corren en el equipo que publica.
bad=0
for rel in "${expected[@]}"; do
  found="$(strings -a "${tmp}/${rel}" | grep -oE "${app_version//./\\.}\.[0-9]{6,9}" | sort -u | head -n1 || true)"
  if [[ -z "${found}" ]]; then
    echo "Error: ${rel} no declara la versión ${app_version}.*" >&2
    echo "El artefacto es de otra versión del proyecto." >&2
    bad=1
  else
    printf '  %-32s %s\n' "${rel}" "${found}"
  fi
done
[[ "${bad}" -eq 0 ]] || exit 1

mkdir -p "${DEST}"
for rel in "${expected[@]}"; do
  mkdir -p "${DEST}/$(dirname "${rel}")"
  cp -f "${tmp}/${rel}" "${DEST}/${rel}"
  chmod +x "${DEST}/${rel}"
done

echo "[agents] listos en ${DEST}"
