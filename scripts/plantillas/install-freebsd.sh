#!/bin/sh
# Installs the ZFSMgr @VERSION@ CLI on FreeBSD amd64. EXPERIMENTAL.
#
# Cross-compiled from Linux and tested on ONE machine: FreeBSD 15.1 with OpenZFS 2.4.2, CLI and
# daemon, installing and updating. That is why it still asks for confirmation. The FreeBSD GUI
# has never been built and is not in the package.
set -eu
ZM_HERE="$(cd "$(dirname "$0")" && pwd)"
ZM_SCRIPT="install-freebsd.sh"
ZM_USAGE_EXTRA="  --accept-experimental Install without asking, knowing it is experimental"
ZM_USAGE_EXTRA_ES="  --accept-experimental Instala sin preguntar, sabiendo que es experimental"
. "${ZM_HERE}/lib/common.sh"

zm_options "$@"
zm_require_system FreeBSD
zm_key freebsd

if [ "${UNINSTALL}" -eq 0 ] && [ "${ACCEPT_EXPERIMENTAL}" -eq 0 ]; then
  if [ "${ZM_LANG}" = es ]; then
    cat <<'WARNING'

  ┌─────────────────────────────────────────────────────────────────────┐
  │  EXPERIMENTAL: probado en una sola máquina, FreeBSD 15.1 con         │
  │  OpenZFS 2.4.2. Se compila en cruzado desde Linux. Si algo falla,    │
  │  cuéntelo.                                                           │
  └─────────────────────────────────────────────────────────────────────┘

WARNING
  else
    cat <<'WARNING'

  ┌─────────────────────────────────────────────────────────────────────┐
  │  EXPERIMENTAL: tested on one machine only, FreeBSD 15.1 with         │
  │  OpenZFS 2.4.2. Cross-compiled from Linux. If something fails,       │
  │  please report it.                                                   │
  └─────────────────────────────────────────────────────────────────────┘

WARNING
  fi
  [ -t 0 ] || zm_die "$(zm_t 'sin terminal para confirmar: use --accept-experimental' \
                             'no terminal to confirm: use --accept-experimental')"
  zm_ask '¿Instalar igualmente?' 'Install anyway?' n || { zm_log "$(zm_t 'cancelado' 'cancelled')"; exit 0; }
fi

zm_elevate_for "${PREFIX}/bin"

if [ "${UNINSTALL}" -eq 1 ]; then
  zm_log "$(zm_t "desinstalando de ${PREFIX}" "uninstalling from ${PREFIX}")"
  zm_uninstall_daemon "${ZM_BIN}"
  zm_uninstall_cli
  zm_done_uninstall
  exit 0
fi

zm_checksums
zm_openzfs
zm_check_jobs
zm_install_cli
zm_test_cli
if [ -d "${ZM_HERE}/gui" ]; then
  zm_log "$(zm_t 'la interfaz gráfica no existe para FreeBSD: solo se instala el CLI.' \
                 'there is no GUI for FreeBSD: only the CLI is installed.')"
fi
zm_daemon "${ZM_BIN}"

zm_done_dry_run
exit 0
