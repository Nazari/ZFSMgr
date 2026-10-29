#!/bin/sh
# Installs ZFSMgr @VERSION@ on macOS, Intel or Apple Silicon: the CLI and, if the package has
# it, the GUI.
#
# With the GUI, the CLI is NOT copied apart: `<prefix>/bin/zfsmgr-cli` is a LINK to the one
# inside /Applications/ZFSMgr.app. The program resolves that link when it starts
# (`std::filesystem::canonical`) and finds its catalogues and agents inside the bundle. One
# copy, updated with the .app. That is why the bundle is `ZFSMgr.app` without a version: with
# one, every update would break the link.
set -eu
ZM_HERE="$(cd "$(dirname "$0")" && pwd)"
ZM_SCRIPT="install-macos.sh"
. "${ZM_HERE}/lib/common.sh"

zm_options "$@"
zm_require_system Darwin
zm_key macos

ZM_APP="/Applications/ZFSMgr.app"
ZM_APP_CLI="${ZM_APP}/Contents/MacOS/zfsmgr-cli"
ZM_APP_GUI="${ZM_APP}/Contents/MacOS/ZFSMgr"

zm_elevate_for "${PREFIX}/bin"

if [ "${UNINSTALL}" -eq 1 ]; then
  zm_log "$(zm_t 'desinstalando' 'uninstalling')"
  # The daemon FIRST: the CLI removes it, and the CLI goes after.
  if [ -x "${ZM_BIN}" ]; then
    zm_uninstall_daemon "${ZM_BIN}"
  elif [ -x "${ZM_APP_CLI}" ]; then
    zm_uninstall_daemon "${ZM_APP_CLI}"
  fi
  zm_uninstall_cli
  if [ -d "${ZM_APP}" ]; then
    sudo_cli="${ZM_SUDO}"
    zm_elevate_for /Applications
    zm_run rm -rf "${ZM_APP}"
    ZM_SUDO="${sudo_cli}"
  fi
  zm_done_uninstall
  exit 0
fi

zm_checksums
zm_openzfs
zm_check_jobs

with_gui=1
if [ "${CLI_ONLY}" -eq 1 ]; then
  zm_log "$(zm_t '--cli-only: la interfaz no se instala.' '--cli-only: the GUI is not installed.')"
  with_gui=0
elif ! zm_has_gui; then
  zm_log "$(zm_t 'este paquete es el de SOLO CLI: no trae la interfaz gráfica.' \
                 'this is the CLI-ONLY package: it has no graphical interface.')"
  with_gui=0
fi

if [ "${with_gui}" -eq 0 ]; then
  zm_install_cli
else
  tarball="${ZM_HERE}/gui/${ZM_KEY}/ZFSMgr.app.tar.gz"
  [ -f "${tarball}" ] || zm_die "$(zm_t "el paquete no trae la interfaz de ${ZM_KEY}" "the package has no GUI for ${ZM_KEY}")"
  if pgrep -x ZFSMgr >/dev/null 2>&1; then
    zm_err "$(zm_t 'aviso: ZFSMgr está abierto. Se reemplaza igual; ciérrelo y vuelva a abrirlo.' \
                   'warning: ZFSMgr is open. It is replaced anyway; close it and open it again.')"
  fi
  zm_log "$(zm_t 'interfaz' 'GUI') -> ${ZM_APP}"
  # Unpacked APART and swapped in one go: if something fails halfway, the .app that was there
  # stays whole. And with `tar`: the bundle carries symbolic links (Qt's frameworks) that a
  # ZIP unpacker does not always respect.
  tmp="$(mktemp -d)"
  tar -xzf "${tarball}" -C "${tmp}"
  [ -x "${tmp}/ZFSMgr.app/Contents/MacOS/zfsmgr-cli" ] \
    || { rm -rf "${tmp}"; zm_die "$(zm_t 'el .app del paquete no trae el CLI dentro' 'the package'"'"'s .app has no CLI inside')"; }
  [ -d "${tmp}/ZFSMgr.app/Contents/Resources/agents" ] \
    || zm_err "$(zm_t 'aviso: el .app no trae agentes: desde él no se podrán instalar daemons en otras máquinas' \
                      'warning: the .app has no agents: it will not install daemons on other machines')"
  sudo_cli="${ZM_SUDO}"
  zm_elevate_for /Applications
  zm_run rm -rf "${ZM_APP}"
  zm_run mv "${tmp}/ZFSMgr.app" "${ZM_APP}"
  # The quarantine. What came through a browser carries it, and Gatekeeper blocks what is not
  # notarised: «cannot be opened because it is from an unidentified developer», with the
  # installation otherwise perfect.
  [ "${DRY_RUN}" -eq 0 ] && ${ZM_SUDO} xattr -dr com.apple.quarantine "${ZM_APP}" 2>/dev/null || true
  ZM_SUDO="${sudo_cli}"
  rm -rf "${tmp}"

  # The CLI: a link to the bundle's. A standalone CLI with its share/zfsmgr from before is
  # removed: it would go stale beside the good one and mislead whoever found it.
  zm_run mkdir -p "${PREFIX}/bin"
  zm_run rm -f "${ZM_BIN}"
  zm_run ln -s "${ZM_APP_CLI}" "${ZM_BIN}"
  [ -d "${ZM_SHARE}" ] && zm_run rm -rf "${ZM_SHARE}"
  zm_test_gui "${ZM_APP_GUI}"
fi

zm_test_cli
zm_daemon "${ZM_BIN}"

zm_done_dry_run
exit 0
