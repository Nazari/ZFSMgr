#!/bin/sh
# Instala ZFSMgr @VERSION@ en Linux x86_64: el CLI y, si el paquete la trae, la interfaz.
#
# La interfaz va como AppDir —el árbol que `linuxdeploy` despliega con Qt dentro— y no como
# .deb ni como AppImage. El .deb enlaza el Qt de la distribución y no lo declara, así que
# solo arranca donde casualmente lo haya; y una AppImage necesita FUSE para montarse. El
# AppDir no necesita ninguna de las dos cosas.
set -eu
ZM_AQUI="$(cd "$(dirname "$0")" && pwd)"
ZM_GUION="install-linux.sh"
. "${ZM_AQUI}/lib/comun.sh"

zm_opciones "$@"
zm_exige_sistema Linux
zm_clave linux

ZM_GUI_DIR="${PREFIJO}/lib/zfsmgr/gui"
ZM_GUI_BIN="${PREFIJO}/bin/zfsmgr-gui"
ZM_DESKTOP="${PREFIJO}/share/applications/zfsmgr.desktop"
ZM_ICONO="${PREFIJO}/share/icons/hicolor/512x512/apps/ZFSMgr.png"

zm_eleva_para "${PREFIJO}/bin"

if [ "${DESINSTALAR}" -eq 1 ]; then
  zm_log "desinstalando de ${PREFIJO}"
  # El daemon PRIMERO: lo quita el propio CLI, que hay que quitar después.
  zm_desinstala_daemon "${ZM_BIN}"
  zm_corre rm -f "${ZM_GUI_BIN}" "${ZM_DESKTOP}" "${ZM_ICONO}"
  zm_corre rm -rf "${PREFIJO}/lib/zfsmgr"
  # `mimeinfo.cache` NO se borra: lo comparten todas las aplicaciones de ese prefijo. Se
  # regenera, para que deje de mencionar un .desktop que ya no está.
  if [ -d "$(dirname "${ZM_DESKTOP}")" ] && command -v update-desktop-database >/dev/null 2>&1; then
    zm_corre update-desktop-database "$(dirname "${ZM_DESKTOP}")" >/dev/null 2>&1 || true
  fi
  zm_desinstala_cli_suelto
  zm_log "hecho. La configuración de ~/.config/ZFSMgr NO se toca."
  exit 0
fi

zm_sumas
zm_openzfs
zm_comprueba_trabajos

zm_instala_cli_suelto
zm_prueba_cli

if [ "${SOLO_CLI}" -eq 1 ]; then
  zm_log "--solo-cli: la interfaz no se instala."
elif ! zm_hay_gui; then
  zm_log "este paquete es el de SOLO CLI: no trae la interfaz gráfica."
else
  tarball="${ZM_AQUI}/gui/${ZM_CLAVE}/ZFSMgr-AppDir.tar.gz"
  [ -f "${tarball}" ] || zm_muere "el paquete no trae la interfaz de ${ZM_CLAVE}"
  zm_log "interfaz -> ${ZM_GUI_DIR}"
  # Se REEMPLAZA entera: dejar bibliotecas de una versión anterior al lado de las nuevas
  # es como acaba cargándose la que no toca.
  zm_corre rm -rf "${ZM_GUI_DIR}"
  zm_corre mkdir -p "${ZM_GUI_DIR}" "$(dirname "${ZM_DESKTOP}")" "$(dirname "${ZM_ICONO}")"
  zm_corre tar -xzf "${tarball}" -C "${ZM_GUI_DIR}"
  # El enlace apunta al binario y no a AppRun, que ya es otro enlace: uno menos que seguir.
  # Funciona a través del enlace porque las bibliotecas se buscan por RPATH relativo
  # ($ORIGIN) y `qt.conf` junto a la ruta REAL del ejecutable.
  zm_corre ln -sf "${ZM_GUI_DIR}/usr/bin/zfsmgr-gui" "${ZM_GUI_BIN}"
  zm_corre install -m 0644 "${ZM_GUI_DIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png" "${ZM_ICONO}"
  # El .desktop con la ruta COMPLETA: el lanzador del escritorio no usa el PATH del
  # terminal, y con un prefijo como ~/.local el `Exec=zfsmgr-gui` a secas no se encuentra.
  escritorio="$(mktemp)"
  cat > "${escritorio}" <<DESKTOP
[Desktop Entry]
Type=Application
Name=ZFSMgr
Comment=OpenZFS manager
Exec=${ZM_GUI_BIN}
Icon=${ZM_ICONO}
Categories=System;Utility;
Terminal=false
DESKTOP
  zm_corre install -m 0644 "${escritorio}" "${ZM_DESKTOP}"
  rm -f "${escritorio}"
  if command -v update-desktop-database >/dev/null 2>&1; then
    zm_corre update-desktop-database "$(dirname "${ZM_DESKTOP}")" >/dev/null 2>&1 || true
  fi
  zm_prueba_gui "${ZM_GUI_BIN}"
fi

zm_daemon "${ZM_BIN}"

[ "${SIMULAR}" -eq 1 ] && zm_log "simulacro terminado; no se ha tocado nada"
exit 0
