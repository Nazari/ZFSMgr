#!/bin/sh
# Instala ZFSMgr @VERSION@ en macOS, Intel o Apple Silicon: el CLI y, si el paquete la trae,
# la interfaz.
#
# Con interfaz, el CLI NO se copia aparte: `<prefijo>/bin/zfsmgr-cli` es un ENLACE al que va
# dentro de /Applications/ZFSMgr.app. El programa resuelve ese enlace al arrancar
# (`std::filesystem::canonical`) y encuentra sus catálogos y agentes dentro del bundle. Una
# sola copia, que se actualiza con el .app. Por eso el bundle se llama `ZFSMgr.app` sin
# versión: si la llevara, cada actualización rompería el enlace.
set -eu
ZM_AQUI="$(cd "$(dirname "$0")" && pwd)"
ZM_GUION="instalar-macos.sh"
. "${ZM_AQUI}/lib/comun.sh"

zm_opciones "$@"
zm_exige_sistema Darwin
zm_clave macos

ZM_APP="/Applications/ZFSMgr.app"
ZM_APP_CLI="${ZM_APP}/Contents/MacOS/zfsmgr-cli"
ZM_APP_GUI="${ZM_APP}/Contents/MacOS/ZFSMgr"

zm_eleva_para "${PREFIJO}/bin"

if [ "${DESINSTALAR}" -eq 1 ]; then
  zm_log "desinstalando"
  # El daemon PRIMERO: lo quita el propio CLI, que hay que quitar después.
  if [ -x "${ZM_BIN}" ]; then
    zm_desinstala_daemon "${ZM_BIN}"
  elif [ -x "${ZM_APP_CLI}" ]; then
    zm_desinstala_daemon "${ZM_APP_CLI}"
  fi
  zm_desinstala_cli_suelto
  if [ -d "${ZM_APP}" ]; then
    eleva_cli="${ZM_ELEVA}"
    zm_eleva_para /Applications
    zm_corre rm -rf "${ZM_APP}"
    ZM_ELEVA="${eleva_cli}"
  fi
  zm_log "hecho. La configuración de ~/.config/ZFSMgr NO se toca."
  exit 0
fi

zm_sumas
zm_openzfs
zm_comprueba_trabajos

con_gui=1
if [ "${SOLO_CLI}" -eq 1 ]; then
  zm_log "--solo-cli: la interfaz no se instala."
  con_gui=0
elif ! zm_hay_gui; then
  zm_log "este paquete es el de SOLO CLI: no trae la interfaz gráfica."
  con_gui=0
fi

if [ "${con_gui}" -eq 0 ]; then
  zm_instala_cli_suelto
else
  tarball="${ZM_AQUI}/gui/${ZM_CLAVE}/ZFSMgr.app.tar.gz"
  [ -f "${tarball}" ] || zm_muere "el paquete no trae la interfaz de ${ZM_CLAVE}"
  if pgrep -x ZFSMgr >/dev/null 2>&1; then
    zm_err "aviso: ZFSMgr está abierto. Se reemplaza igual; ciérrelo y vuelva a abrirlo."
  fi
  zm_log "interfaz -> ${ZM_APP}"
  # Se descomprime APARTE y se cambia de una vez: si algo falla a medias, el .app que había
  # sigue entero. Y con `tar`: el bundle lleva enlaces simbólicos (los frameworks de Qt) que
  # un descompresor de ZIP no siempre respeta.
  temporal="$(mktemp -d)"
  tar -xzf "${tarball}" -C "${temporal}"
  [ -x "${temporal}/ZFSMgr.app/Contents/MacOS/zfsmgr-cli" ] \
    || { rm -rf "${temporal}"; zm_muere "el .app del paquete no trae el CLI dentro"; }
  [ -d "${temporal}/ZFSMgr.app/Contents/Resources/agents" ] \
    || zm_err "aviso: el .app no trae agentes: desde él no se podrán instalar daemons en otras máquinas"
  eleva_cli="${ZM_ELEVA}"
  zm_eleva_para /Applications
  zm_corre rm -rf "${ZM_APP}"
  zm_corre mv "${temporal}/ZFSMgr.app" "${ZM_APP}"
  # La cuarentena. Lo que llegó por un navegador la arrastra, y Gatekeeper bloquea lo que
  # no está notarizado: «no se puede abrir porque procede de un desarrollador no
  # identificado», con la instalación por lo demás perfecta.
  [ "${SIMULAR}" -eq 0 ] && ${ZM_ELEVA} xattr -dr com.apple.quarantine "${ZM_APP}" 2>/dev/null || true
  ZM_ELEVA="${eleva_cli}"
  rm -rf "${temporal}"

  # El CLI: un enlace al del bundle. Si antes había un CLI suelto con su share/zfsmgr, se
  # quita: se quedaría viejo al lado del bueno y desorientaría a quien lo encontrara.
  zm_corre mkdir -p "${PREFIJO}/bin"
  zm_corre rm -f "${ZM_BIN}"
  zm_corre ln -s "${ZM_APP_CLI}" "${ZM_BIN}"
  [ -d "${ZM_COMPARTIDO}" ] && zm_corre rm -rf "${ZM_COMPARTIDO}"
  zm_prueba_gui "${ZM_APP_GUI}"
fi

zm_prueba_cli
zm_daemon "${ZM_BIN}"

[ "${SIMULAR}" -eq 1 ] && zm_log "simulacro terminado; no se ha tocado nada"
exit 0
