#!/bin/sh
# Installs ZFSMgr @VERSION@ on Linux x86_64: the CLI and, if the package has it, the GUI.
#
# The GUI comes as an AppDir —the tree `linuxdeploy` lays out with Qt inside— and not as a .deb
# or an AppImage. The .deb links the distribution's Qt without declaring it, so it only starts
# where it happens to be there; and an AppImage needs FUSE to mount itself. The AppDir needs
# neither.
set -eu
ZM_HERE="$(cd "$(dirname "$0")" && pwd)"
ZM_SCRIPT="install-linux.sh"
. "${ZM_HERE}/lib/common.sh"

zm_options "$@"
zm_require_system Linux
zm_key linux

ZM_GUI_DIR="${PREFIX}/lib/zfsmgr/gui"
ZM_GUI_BIN="${PREFIX}/bin/zfsmgr-gui"
ZM_DESKTOP="${PREFIX}/share/applications/zfsmgr.desktop"
ZM_ICON="${PREFIX}/share/icons/hicolor/512x512/apps/ZFSMgr.png"

zm_elevate_for "${PREFIX}/bin"

if [ "${UNINSTALL}" -eq 1 ]; then
  zm_log "$(zm_t "desinstalando de ${PREFIX}" "uninstalling from ${PREFIX}")"
  # The daemon FIRST: the CLI removes it, and the CLI goes after.
  zm_uninstall_daemon "${ZM_BIN}"
  zm_run rm -f "${ZM_GUI_BIN}" "${ZM_DESKTOP}" "${ZM_ICON}"
  zm_run rm -rf "${PREFIX}/lib/zfsmgr"
  # `mimeinfo.cache` is NOT deleted: every application of that prefix shares it. It is
  # regenerated, so it stops naming a .desktop that is gone.
  if [ -d "$(dirname "${ZM_DESKTOP}")" ] && command -v update-desktop-database >/dev/null 2>&1; then
    zm_run update-desktop-database "$(dirname "${ZM_DESKTOP}")" >/dev/null 2>&1 || true
  fi
  zm_uninstall_cli
  zm_done_uninstall
  exit 0
fi

zm_checksums
zm_openzfs
zm_check_jobs

zm_install_cli
zm_test_cli

if [ "${CLI_ONLY}" -eq 1 ]; then
  zm_log "$(zm_t '--cli-only: la interfaz no se instala.' '--cli-only: the GUI is not installed.')"
elif ! zm_has_gui; then
  zm_log "$(zm_t 'este paquete es el de SOLO CLI: no trae la interfaz gráfica.' \
                 'this is the CLI-ONLY package: it has no graphical interface.')"
else
  tarball="${ZM_HERE}/gui/${ZM_KEY}/ZFSMgr-AppDir.tar.gz"
  [ -f "${tarball}" ] || zm_die "$(zm_t "el paquete no trae la interfaz de ${ZM_KEY}" "the package has no GUI for ${ZM_KEY}")"
  zm_log "$(zm_t 'interfaz' 'GUI') -> ${ZM_GUI_DIR}"
  # REPLACED whole: leaving an older version's libraries beside the new ones is how the wrong
  # one ends up loaded.
  zm_run rm -rf "${ZM_GUI_DIR}"
  zm_run mkdir -p "${ZM_GUI_DIR}" "$(dirname "${ZM_DESKTOP}")" "$(dirname "${ZM_ICON}")"
  zm_run tar -xzf "${tarball}" -C "${ZM_GUI_DIR}"
  # The link points at the binary and not at AppRun, which is another link: one less to
  # follow. It works through the link because libraries are found by relative RPATH ($ORIGIN)
  # and `qt.conf` beside the executable's REAL path.
  zm_run ln -sf "${ZM_GUI_DIR}/usr/bin/zfsmgr-gui" "${ZM_GUI_BIN}"
  zm_run install -m 0644 "${ZM_GUI_DIR}/usr/share/icons/hicolor/512x512/apps/ZFSMgr.png" "${ZM_ICON}"
  # The .desktop with the FULL path: the desktop launcher does not use the terminal's PATH, and
  # with a prefix like ~/.local a bare `Exec=zfsmgr-gui` is not found.
  desktop="$(mktemp)"
  cat > "${desktop}" <<DESKTOP
[Desktop Entry]
Type=Application
Name=ZFSMgr
Comment=OpenZFS manager
Exec=${ZM_GUI_BIN}
Icon=${ZM_ICON}
Categories=System;Utility;
Terminal=false
DESKTOP
  zm_run install -m 0644 "${desktop}" "${ZM_DESKTOP}"
  rm -f "${desktop}"
  if command -v update-desktop-database >/dev/null 2>&1; then
    zm_run update-desktop-database "$(dirname "${ZM_DESKTOP}")" >/dev/null 2>&1 || true
  fi
  zm_test_gui "${ZM_GUI_BIN}"
fi

zm_daemon "${ZM_BIN}"

zm_done_dry_run
exit 0
