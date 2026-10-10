#!/bin/sh
# Instala el CLI de ZFSMgr @VERSION@ en FreeBSD amd64. EXPERIMENTAL.
#
# Se compila en cruzado desde Linux y se ha probado en UNA sola máquina: FreeBSD 15.1 con
# OpenZFS 2.4.2, CLI y daemon, instalando y actualizando. Por eso sigue pidiendo
# confirmación. La interfaz gráfica de FreeBSD no se ha compilado nunca y no va en el paquete.
set -eu
ZM_AQUI="$(cd "$(dirname "$0")" && pwd)"
ZM_GUION="instalar-freebsd.sh"
ZM_USO_EXTRA="  --acepto-experimental Instala sin preguntar, sabiendo que es experimental"
. "${ZM_AQUI}/lib/comun.sh"

zm_opciones "$@"
zm_exige_sistema FreeBSD
zm_clave freebsd

if [ "${DESINSTALAR}" -eq 0 ] && [ "${ACEPTO_EXPERIMENTAL}" -eq 0 ]; then
  cat <<'AVISO'

  ┌─────────────────────────────────────────────────────────────────────┐
  │  EXPERIMENTAL: probado en una sola máquina, FreeBSD 15.1 con         │
  │  OpenZFS 2.4.2. Se compila en cruzado desde Linux. Si algo falla,    │
  │  cuéntelo.                                                           │
  └─────────────────────────────────────────────────────────────────────┘

AVISO
  [ -t 0 ] || zm_muere "sin terminal para confirmar: use --acepto-experimental"
  printf '[zfsmgr] ¿Instalar igualmente? [s/N] '
  read -r resp || resp=""
  case "${resp}" in [sSyY]*) ;; *) zm_log "cancelado"; exit 0 ;; esac
fi

zm_eleva_para "${PREFIJO}/bin"

if [ "${DESINSTALAR}" -eq 1 ]; then
  zm_log "desinstalando de ${PREFIJO}"
  zm_desinstala_daemon "${ZM_BIN}"
  zm_desinstala_cli_suelto
  zm_log "hecho. La configuración de ~/.config/ZFSMgr NO se toca."
  exit 0
fi

zm_sumas
zm_openzfs
zm_comprueba_trabajos
zm_instala_cli_suelto
zm_prueba_cli
if [ -d "${ZM_AQUI}/gui" ]; then
  zm_log "la interfaz gráfica no existe para FreeBSD: solo se instala el CLI."
fi
zm_daemon "${ZM_BIN}"

[ "${SIMULAR}" -eq 1 ] && zm_log "simulacro terminado; no se ha tocado nada"
exit 0
