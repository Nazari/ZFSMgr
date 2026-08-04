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
  # sort -u sin head: con "set -o pipefail", head cierra la tubería al primer
  # resultado, quien escribe recibe SIGPIPE y el conjunto se reporta como fallido
  # aunque hubiera encontrado la versión.
  found="$(strings -a "${tmp}/${rel}" | grep -oE "${app_version//./\\.}\.[0-9]{6,9}" | sort -u || true)"
  found="${found%%$'"'"'\n'"'"'*}"
  if [[ -z "${found}" ]]; then
    echo "Error: ${rel} no declara la versión ${app_version}.*" >&2
    echo "El artefacto es de otra versión del proyecto." >&2
    bad=1
  else
    printf '  %-32s %s\n' "${rel}" "${found}"
  fi
done
[[ "${bad}" -eq 0 ]] || exit 1

# El agente de Windows se despliega SOLO en la máquina remota: no va acompañado de
# ninguna otra biblioteca y allí no hay gestor de paquetes que le resuelva nada. Si
# depende del runtime de Visual C++ o de las DLL de OpenSSL, arranca con 0xC0000135
# ("DLL no encontrada") sin decir cuál, y el síntoma que se ve es que instalar el
# daemon falla en el paso de generar el material TLS.
#
# Pasó: la integración continua lo compila con MSVC y OpenSSL dinámico, mientras que
# el cruce con MinGW lo enlaza estáticamente. Traer el de CI rompía la instalación en
# cualquier Windows sin el redistribuible de Visual C++.
win_bin="${tmp}/windows-x86_64/zfsmgr_agent.exe"
forbidden="$(strings -a "${win_bin}" \
  | grep -oiE '(vcruntime[0-9]*|msvcp[0-9]*|libssl-[0-9]+-x64|libcrypto-[0-9]+-x64)\.dll' \
  | sort -u || true)"
if [[ -n "${forbidden}" ]]; then
  echo "Error: el agente de Windows no es autosuficiente. Necesita:" >&2
  printf '  %s\n' ${forbidden} >&2
  echo "Se despliega solo en la máquina remota, así que arrancaría con 0xC0000135." >&2
  echo "Use el compilado con MinGW: scripts/build-cross.sh --target windows" >&2
  exit 1
fi

mkdir -p "${DEST}"
for rel in "${expected[@]}"; do
  mkdir -p "${DEST}/$(dirname "${rel}")"
  cp -f "${tmp}/${rel}" "${DEST}/${rel}"
  chmod +x "${DEST}/${rel}"
done

echo "[agents] listos en ${DEST}"
