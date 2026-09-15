#!/bin/sh
# Instala zfsmgr-cli @VERSION@ en Linux o macOS (Intel y Apple Silicon).
#
# POSIX sh a propósito, sin bash: FreeBSD no lo trae de serie y un instalador que necesita
# instalar algo antes de instalar no es un instalador.
set -eu

PREFIJO="${PREFIJO:-/usr/local}"
SIMULAR=0
DESINSTALAR=0
AQUI="$(cd "$(dirname "$0")" && pwd)"

uso() {
  cat <<'USO'
Uso: ./instalar.sh [--prefijo <ruta>] [--simular] [--desinstalar]

  --prefijo <ruta>  Dónde instalar (por omisión, /usr/local)
  --simular         Dice lo que haría, sin tocar nada
  --desinstalar     Quita lo que instaló

Deja el intérprete en <prefijo>/bin/zfsmgr-cli y, a su lado,
<prefijo>/share/zfsmgr/{i18n,agents}: son las rutas donde el propio
programa los busca a partir de dónde está su ejecutable. Moverlos por
separado lo deja hablando solo castellano y sin poder instalar daemons.
USO
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefijo) shift; PREFIJO="${1:?falta la ruta}"; shift ;;
    --simular) SIMULAR=1; shift ;;
    --desinstalar) DESINSTALAR=1; shift ;;
    -h|--help) uso; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; uso >&2; exit 2 ;;
  esac
done

# Qué máquina es esta. La pareja <sistema>-<arquitectura> es la MISMA cadena que usa el
# cliente para buscar su agente, así que aquí no se inventa un vocabulario nuevo.
sistema="$(uname -s)"
maquina="$(uname -m)"
case "${sistema}" in
  Linux)   so=linux ;;
  Darwin)  so=macos ;;
  FreeBSD) echo "este paquete no trae FreeBSD: no hay máquina donde probarlo, así que no se reparte" >&2; exit 1 ;;
  *) echo "sistema no soportado: ${sistema}" >&2; exit 1 ;;
esac
case "${so}-${maquina}" in
  macos-arm64)          clave=macos-arm64 ;;
  macos-x86_64)         clave=macos-amd64 ;;
  linux-x86_64|linux-amd64)     clave=linux-x86_64 ;;
  *) echo "arquitectura no soportada: ${sistema} ${maquina}" >&2; exit 1 ;;
esac

BIN="${PREFIJO}/bin/zfsmgr-cli"
COMPARTIDO="${PREFIJO}/share/zfsmgr"

# Elevar solo si hace falta, y decirlo ANTES de pedir la contraseña: que a uno le salga un
# «Password:» sin contexto en mitad de un guion es la forma más rápida de que lo cancele.
# Si se puede escribir donde va, NO se eleva. Y la pregunta no es «¿puedo escribir en
# <prefijo>/bin?» —que aún no existe, así que la respuesta es siempre no— sino «¿puedo crear
# ese árbol?», que se contesta subiendo hasta el primer directorio que sí exista. Con la
# pregunta mal hecha, un `--prefijo $HOME/.local` perfectamente escribible pedía sudo.
escribible() {
  d="$1"
  while [ ! -d "${d}" ] && [ "${d}" != "/" ] && [ -n "${d}" ]; do
    d="$(dirname "${d}")"
  done
  [ -w "${d}" ]
}

ELEVA=""
if [ "$(id -u)" -ne 0 ]; then
  if escribible "${PREFIJO}/bin"; then
    ELEVA=""
  elif command -v sudo >/dev/null 2>&1; then
    ELEVA="sudo"
    echo "[zfsmgr] ${PREFIJO} es de root: se usará sudo y pedirá su contraseña."
  else
    echo "no se puede escribir en ${PREFIJO} y no hay sudo; use --prefijo \$HOME/.local" >&2
    exit 1
  fi
fi

corre() {
  if [ "${SIMULAR}" -eq 1 ]; then
    echo "  [simulado] ${ELEVA} $*"
  else
    ${ELEVA} "$@"
  fi
}

if [ "${DESINSTALAR}" -eq 1 ]; then
  echo "[zfsmgr] desinstalando de ${PREFIJO}"
  corre rm -f "${BIN}"
  corre rm -rf "${COMPARTIDO}"
  echo "[zfsmgr] hecho. La configuración de ~/.config/ZFSMgr NO se toca."
  exit 0
fi

echo "[zfsmgr] ${sistema} ${maquina} -> ${clave}"

# Las sumas ANTES de copiar. Un ZIP que se bajó a medias se detiene aquí y no a mitad de la
# instalación, con la mitad de los ficheros puestos y la otra mitad no.
if [ -f "${AQUI}/MANIFIESTO.sha256" ]; then
  if command -v sha256sum >/dev/null 2>&1; then
    ( cd "${AQUI}" && sha256sum -c --quiet MANIFIESTO.sha256 ) \
      || { echo "el paquete está dañado: las sumas no cuadran" >&2; exit 1; }
  elif command -v shasum >/dev/null 2>&1; then
    ( cd "${AQUI}" && shasum -a 256 -c MANIFIESTO.sha256 >/dev/null ) \
      || { echo "el paquete está dañado: las sumas no cuadran" >&2; exit 1; }
  else
    echo "[zfsmgr] aviso: sin sha256sum ni shasum, no se comprueba la integridad"
  fi
  echo "[zfsmgr] sumas comprobadas"
fi

origen="${AQUI}/bin/zfsmgr-cli-${clave}"
[ -f "${origen}" ] || { echo "el paquete no trae el binario de ${clave}" >&2; exit 1; }

corre mkdir -p "${PREFIJO}/bin" "${COMPARTIDO}/i18n" "${COMPARTIDO}/agents"
corre install -m 0755 "${origen}" "${BIN}"
for j in "${AQUI}"/i18n/*.json; do
  corre install -m 0644 "${j}" "${COMPARTIDO}/i18n/"
done
# Los TRES agentes, no solo el de esta máquina: son los que permiten instalar el daemon en
# una máquina de otro sistema desde ésta.
for d in "${AQUI}"/agents/*/; do
  k="$(basename "${d}")"
  corre mkdir -p "${COMPARTIDO}/agents/${k}"
  for a in "${d}"*; do
    corre install -m 0644 "${a}" "${COMPARTIDO}/agents/${k}/"
  done
done

# En macOS, la cuarentena. Un fichero que llegó por navegador la arrastra, y Gatekeeper
# bloquea lo que no está notarizado: el binario se instala bien y al ejecutarlo sale
# «no se puede abrir porque procede de un desarrollador no identificado».
if [ "${so}" = macos ] && [ "${SIMULAR}" -eq 0 ]; then
  ${ELEVA} xattr -dr com.apple.quarantine "${BIN}" "${COMPARTIDO}" 2>/dev/null || true
fi

if [ "${SIMULAR}" -eq 1 ]; then
  echo "[zfsmgr] simulacro terminado; no se ha tocado nada"
  exit 0
fi

# Y se COMPRUEBA que arranca, que es lo único que demuestra que la instalación sirve.
if ! salida="$("${BIN}" version 2>&1)"; then
  echo "[zfsmgr] instalado en ${BIN}, pero NO arranca:" >&2
  echo "${salida}" >&2
  exit 1
fi
echo "[zfsmgr] ${salida}  ->  ${BIN}"
case ":${PATH}:" in
  *":${PREFIJO}/bin:"*) ;;
  *) echo "[zfsmgr] aviso: ${PREFIJO}/bin no está en su PATH." ;;
esac
