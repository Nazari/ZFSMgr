#!/bin/sh
# Installs ZFSMgr @VERSION@ on macOS, Intel or Apple Silicon: the CLI and, if the package has
# it, the GUI.
#
# Since 2.0 the GUI is a Java 21+ program (one .jar for every system). The CLI is installed
# standalone, as in the CLI-only package; the GUI goes to <prefix>/lib/zfsmgr with its launcher
# <prefix>/bin/zfsmgr-gui, and /Applications/ZFSMgr.app is only a small bundle that runs that
# launcher —so it opens from Finder, Launchpad and the Dock—. The Qt bundle of 1.x, which had
# the CLI inside and <prefix>/bin/zfsmgr-cli linked to it, is always removed.
set -eu
ZM_HERE="$(cd "$(dirname "$0")" && pwd)"
ZM_SCRIPT="install-macos.sh"
. "${ZM_HERE}/lib/common.sh"

zm_options "$@"
zm_require_system Darwin
zm_key macos

ZM_APP="/Applications/ZFSMgr.app"
# The CLI the Qt bundle of 1.x carried inside: whoever still has it gets it uninstalled.
ZM_APP_CLI="${ZM_APP}/Contents/MacOS/zfsmgr-cli"

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
  zm_run rm -f "${PREFIX}/bin/zfsmgr-gui"
  zm_run rm -rf "${PREFIX}/lib/zfsmgr"
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

zm_install_cli

# The Qt bundle of 1.x —the one with Qt's frameworks inside—: ALWAYS removed, with the Java GUI
# or without it. Left behind it goes on talking to a CLI it no longer matches.
if [ -d "${ZM_APP}/Contents/Frameworks" ]; then
  zm_log "$(zm_t 'se quita la interfaz anterior (Qt)' 'the previous (Qt) GUI is removed')"
  sudo_cli="${ZM_SUDO}"
  zm_elevate_for /Applications
  zm_run rm -rf "${ZM_APP}"
  ZM_SUDO="${sudo_cli}"
  # Its launcher was a link into the bundle.
  [ -L "${PREFIX}/bin/zfsmgr-gui" ] && zm_run rm -f "${PREFIX}/bin/zfsmgr-gui"
fi

if [ "${CLI_ONLY}" -eq 1 ]; then
  zm_log "$(zm_t '--cli-only: la interfaz no se instala.' '--cli-only: the GUI is not installed.')"
  # A Java GUI of an earlier installation would be left with a CLI it may no longer match.
  if [ -f "${PREFIX}/lib/zfsmgr/zfsmgr-gui.jar" ] || [ -e "${PREFIX}/bin/zfsmgr-gui" ] || [ -d "${ZM_APP}" ]; then
    zm_log "$(zm_t 'se quita la interfaz instalada antes: con --cli-only se quedaría desfasada' \
                   'the GUI installed before is removed: with --cli-only it would fall out of step')"
    zm_run rm -f "${PREFIX}/bin/zfsmgr-gui"
    zm_run rm -rf "${PREFIX}/lib/zfsmgr"
    if [ -d "${ZM_APP}" ]; then
      sudo_cli="${ZM_SUDO}"
      zm_elevate_for /Applications
      zm_run rm -rf "${ZM_APP}"
      ZM_SUDO="${sudo_cli}"
    fi
  fi
elif ! zm_has_gui; then
  zm_log "$(zm_t 'este paquete es el de SOLO CLI: no trae la interfaz gráfica.' \
                 'this is the CLI-ONLY package: it has no graphical interface.')"
else
  if pgrep -x ZFSMgr >/dev/null 2>&1; then
    zm_err "$(zm_t 'aviso: ZFSMgr está abierto. Se reemplaza igual; ciérrelo y vuelva a abrirlo.' \
                   'warning: ZFSMgr is open. It is replaced anyway; close it and open it again.')"
  fi
  zm_install_gui
  # The bundle, built here: Info.plist, the icon, and an executable that is the launcher with
  # the Dock's name and icon. Built APART and swapped in one go.
  zm_log "$(zm_t 'interfaz' 'GUI') -> ${ZM_APP}"
  tmp="$(mktemp -d)"
  mkdir -p "${tmp}/ZFSMgr.app/Contents/MacOS" "${tmp}/ZFSMgr.app/Contents/Resources"
  cp "${ZM_HERE}/gui/ZFSMgr.icns" "${tmp}/ZFSMgr.app/Contents/Resources/ZFSMgr.icns"
  cat > "${tmp}/ZFSMgr.app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>ZFSMgr</string>
  <key>CFBundleDisplayName</key><string>ZFSMgr</string>
  <key>CFBundleIdentifier</key><string>org.zfsmgr.gui</string>
  <key>CFBundleExecutable</key><string>ZFSMgr</string>
  <key>CFBundleIconFile</key><string>ZFSMgr</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>@VERSION@</string>
  <key>CFBundleVersion</key><string>@VERSION@</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
  cat > "${tmp}/ZFSMgr.app/Contents/MacOS/ZFSMgr" <<APPRUN
#!/bin/sh
ZFSMGR_JAVA_OPTS="-Xdock:name=ZFSMgr -Xdock:icon=${ZM_APP}/Contents/Resources/ZFSMgr.icns"
export ZFSMGR_JAVA_OPTS
exec "${ZM_GUI_BIN}" "\$@"
APPRUN
  chmod 0755 "${tmp}/ZFSMgr.app/Contents/MacOS/ZFSMgr"
  sudo_cli="${ZM_SUDO}"
  zm_elevate_for /Applications
  zm_run rm -rf "${ZM_APP}"
  zm_run mv "${tmp}/ZFSMgr.app" "${ZM_APP}"
  # The quarantine: what came through a browser carries it, and Gatekeeper blocks what is not
  # notarised.
  [ "${DRY_RUN}" -eq 0 ] && ${ZM_SUDO} xattr -dr com.apple.quarantine "${ZM_APP}" 2>/dev/null || true
  ZM_SUDO="${sudo_cli}"
  rm -rf "${tmp}"
  zm_test_gui "${ZM_GUI_BIN}"
fi

zm_test_cli
zm_daemon "${ZM_BIN}"

zm_done_dry_run
exit 0
