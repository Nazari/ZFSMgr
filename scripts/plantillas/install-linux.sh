#!/bin/sh
# Installs ZFSMgr @VERSION@ on Linux x86_64: the CLI and, if the package has it, the GUI.
#
# The GUI is a Java 21+ program (one .jar for every system, since 2.0): it needs a Java that
# the launcher finds at each start, and nothing of the system's own libraries. The Qt one of
# before —an AppDir in <prefix>/lib/zfsmgr/gui— is removed when this one is installed.
set -eu
ZM_HERE="$(cd "$(dirname "$0")" && pwd)"
ZM_SCRIPT="install-linux.sh"
. "${ZM_HERE}/lib/common.sh"

zm_options "$@"
zm_require_system Linux
zm_key linux

# Where the Qt GUI of 1.x went: removed when the Java one is installed or on uninstall.
ZM_OLD_GUI_DIR="${PREFIX}/lib/zfsmgr/gui"

zm_elevate_for "${PREFIX}/bin"

if [ "${UNINSTALL}" -eq 1 ]; then
  zm_log "$(zm_t "desinstalando de ${PREFIX}" "uninstalling from ${PREFIX}")"
  # The daemon FIRST: the CLI removes it, and the CLI goes after.
  zm_uninstall_daemon "${ZM_BIN}"
  zm_uninstall_desktop_entry
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
  [ -d "${ZM_OLD_GUI_DIR}" ] && zm_run rm -rf "${ZM_OLD_GUI_DIR}"
  zm_install_gui
  zm_install_desktop_entry
  zm_test_gui "${ZM_GUI_BIN}"
fi

zm_daemon "${ZM_BIN}"

zm_done_dry_run
exit 0
