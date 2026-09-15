#!/usr/bin/env bash
# Un solo artefacto con el intérprete para Linux y macOS.
#
# Dentro van los tres ejecutables de consola —Linux x86_64, macOS Intel y macOS Apple
# Silicon—, sus tres agentes y los catálogos, más un instalador. Es UN fichero que descargar
# y dos pasos que dar: descomprimir y ejecutar.
#
# Windows y FreeBSD quedan FUERA, y no por una limitación técnica: el cruce de los dos
# funciona y se compiló. Es que no hay ninguna máquina de esos sistemas donde probar lo que
# se repartiría, y un instalador que reparte binarios de los que nadie sabe si arrancan es
# peor que no tener instalador. Sus agentes se van con ellos: un agente solo sirve para
# instalarlo en una máquina de ese sistema. Volver a meterlos es añadir sus filas a las dos
# tablas de abajo y recompilar con `build-cross.sh --target windows|freebsd`.
#
# `.tar.gz` y no `.zip`, y la razón salió probándolo: unib —Arch Linux— NO TRAE `unzip`. Un
# instalador que exige instalar algo antes de instalar no es un instalador. `tar` y `gzip`
# están en el sistema base de Linux y de macOS, y en macOS el Finder además lo abre con doble
# clic. El ZIP solo aportaba algo mientras Windows estuvo en el alcance, y ya no lo está.
#
# (Se valoró también un solo fichero POLÍGLOTA: un `.cmd` que fuera a la vez guion de shell y
# archivo ZIP, aprovechando que el directorio central del ZIP está al final y admite lo que
# sea por delante. Funciona —se probó— pero en Unix habría que invocarlo como `sh fichero`,
# porque no puede llevar `#!` sin atragantar a cmd.exe. Sin Windows, no compensa.)
#
# Los TRES agentes viajan a propósito, aunque sean la mitad del peso: sin ellos, desde la
# máquina donde se instala no se puede instalar un daemon en otra de distinto sistema, que
# es media razón de ser de la herramienta.
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SALIDA="${RAIZ}/dist"
PODAR=1

uso() {
  cat <<'USO'
Uso: empaqueta_cli.sh [--salida <dir>] [--sin-podar]

  --salida <dir>   Dónde dejar el .zip (por omisión, dist/)
  --sin-podar      No quitar los símbolos de los binarios. Con ellos el paquete
                   pasa de ~22 MB a ~35 MB; sirve para depurar un cuelgue.
USO
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --salida) shift; SALIDA="${1:?falta el directorio}"; shift ;;
    --sin-podar) PODAR=0; shift ;;
    -h|--help) uso; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; uso >&2; exit 2 ;;
  esac
done

# La versión sale del MISMO sitio que la de los binarios, no de un número escrito aquí.
VERSION="$(grep -oE 'ZFSMGR_APP_VERSION_STRING +"[^"]+"' \
             "${RAIZ}/builds/linux/CMakeCache.txt" 2>/dev/null | head -1 | cut -d'"' -f2 || true)"
if [[ -z "${VERSION}" ]]; then
  VERSION="$("${RAIZ}/builds/linux/zfsmgr-cli" version 2>/dev/null | awk '{print $2}')"
fi
[[ -n "${VERSION}" ]] || { echo "no se pudo averiguar la versión" >&2; exit 1; }

# Qué binario va a cada sitio. La clave es «<sistema>-<arquitectura>», la misma que usa
# `rutaDelAgente()` en el cliente para buscar el agente: si aquí se escribiera otra, el
# paquete quedaría bien formado y el `install-daemon` no encontraría nada.
declare -A CLI=(
  [linux-x86_64]="builds/linux/zfsmgr-cli"
  [macos-amd64]="builds/cross-macos-amd64/zfsmgr-cli"
  [macos-arm64]="builds/cross-macos-arm64/zfsmgr-cli"
)
declare -A AGENTE=(
  [linux-x86_64]="builds/agents/linux-x86_64/zfsmgr_agent"
  [macos-amd64]="builds/agents/macos-amd64/zfsmgr_agent"
  [macos-arm64]="builds/agents/macos-arm64/zfsmgr_agent"
)

TRABAJO="$(mktemp -d)"
trap 'rm -rf "${TRABAJO}"' EXIT
ARBOL="${TRABAJO}/zfsmgr-cli-${VERSION}"
mkdir -p "${ARBOL}/bin" "${ARBOL}/agents" "${ARBOL}/i18n"

# Podar NO es gratis en macOS: en Apple Silicon el sistema exige firma para ejecutar, y
# tocar el binario la invalida. `llvm-strip` la reajusta y el binario sigue arrancando
# —comprobado en vivo sobre mmela, arm64—, pero si algún día deja de hacerlo, esto es lo
# primero que hay que mirar, y `--sin-podar` es la salida.
poda() {
  [[ "${PODAR}" -eq 1 ]] || return 0
  llvm-strip "$1" 2>/dev/null || strip "$1" 2>/dev/null || true
}

falta=0
for clave in "${!CLI[@]}"; do
  ext=""; [[ "${clave}" == windows-* ]] && ext=".exe"
  for par in "CLI:${CLI[$clave]}:bin/zfsmgr-cli-${clave}${ext}" \
             "AGENTE:${AGENTE[$clave]}:agents/${clave}/zfsmgr_agent${ext}"; do
    IFS=: read -r que origen destino <<< "${par}"
    if [[ ! -f "${RAIZ}/${origen}" ]]; then
      echo "FALTA el ${que} de ${clave}: ${origen}" >&2
      falta=1
      continue
    fi
    mkdir -p "$(dirname "${ARBOL}/${destino}")"
    cp -f "${RAIZ}/${origen}" "${ARBOL}/${destino}"
    chmod 0755 "${ARBOL}/${destino}"
    poda "${ARBOL}/${destino}"
  done
done
[[ "${falta}" -eq 0 ]] || { echo "" >&2; echo "Compílalos antes: scripts/build-cross.sh --target <t>" >&2; exit 1; }

cp -f "${RAIZ}"/i18n/*.json "${ARBOL}/i18n/"
cp -f "${RAIZ}/scripts/plantillas/instalar.sh" "${ARBOL}/instalar.sh"
chmod 0755 "${ARBOL}/instalar.sh"
sed -i "s/@VERSION@/${VERSION}/g" "${ARBOL}/instalar.sh"

# El manifiesto con las sumas. No es ceremonia: el instalador las comprueba antes de copiar
# nada, así que un ZIP a medias se detiene ANTES de dejar medio instalado el sistema.
( cd "${ARBOL}" && find bin agents i18n -type f | LC_ALL=C sort \
    | xargs sha256sum > MANIFIESTO.sha256 )

mkdir -p "${SALIDA}"
ZIP="${SALIDA}/zfsmgr-cli-${VERSION}.tar.gz"
rm -f "${ZIP}"
# Dueño y permisos NORMALIZADOS. Sin esto el paquete lleva dentro el uid de quien lo
# construyó, y al desempaquetarlo como root los ficheros salen con ese dueño; y el bit de
# ejecución de `instalar.sh` es justo lo que no puede perderse.
tar -czf "${ZIP}" -C "${TRABAJO}" \
    --owner=0 --group=0 --numeric-owner \
    "zfsmgr-cli-${VERSION}"

echo "${ZIP}"
echo "  versión:   ${VERSION}"
# `du` da BLOQUES de disco, no el tamaño del fichero, y en un sistema con compresión
# transparente —este proyecto se desarrolla sobre ZFS— miente: dijo «1.0K» de un ZIP de
# 22 MB. `stat` da los bytes de verdad.
echo "  tamaño:    $(awk -v b="$(stat -c%s "${ZIP}")" 'BEGIN{printf "%.1f MB", b/1048576}')"
echo "  contenido: $(tar -tzf "${ZIP}" | grep -vc '/$') ficheros"
