# What the Unix installers share: Linux, macOS and FreeBSD.
#
# POSIX sh, no bash: FreeBSD does not ship it, and an installer that needs something installed
# first is not an installer. Each install-<system>.sh loads it with `.`.
#
# Here is what does NOT change between systems: the options, the checksums, the OpenZFS
# version, elevation, the standalone CLI and the daemon. What does change —where the GUI goes
# and how it hooks into the desktop— lives in each system's script.
#
# Messages come in the system's language: Spanish when the locale says so, English otherwise.
# Each one is written in both, side by side (zm_t), so they cannot drift apart.

ZM_VERSION="@VERSION@"
ZM_ZFS_MIN_MAJOR=@ZFSMIN_MAYOR@
ZM_ZFS_MIN_MINOR=@ZFSMIN_MENOR@

PREFIX="${PREFIX:-/usr/local}"
DRY_RUN=0
UNINSTALL=0
CLI_ONLY=0
DAEMON=ask                  # ask | yes | no
ACCEPT_EXPERIMENTAL=0
FORCE=0                     # reinstall the daemon even with jobs in progress

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
  es*) ZM_LANG=es ;;
  *) ZM_LANG=en ;;
esac

# zm_t <spanish> <english>: the text in the system's language.
zm_t() {
  if [ "${ZM_LANG}" = es ]; then printf '%s' "$1"; else printf '%s' "$2"; fi
}
zm_log() { echo "[zfsmgr] $*"; }
zm_err() { echo "[zfsmgr] $*" >&2; }
zm_die() { zm_err "$*"; exit 1; }
# zm_ask <spanish> <english> <default y|n>: a yes/no question; answers s/S/y/Y mean yes.
zm_ask() {
  if [ "$3" = y ]; then
    printf '[zfsmgr] %s %s ' "$(zm_t "$1" "$2")" "$(zm_t '[S/n]' '[Y/n]')"
  else
    printf '[zfsmgr] %s %s ' "$(zm_t "$1" "$2")" "$(zm_t '[s/N]' '[y/N]')"
  fi
  read -r resp || resp=""
  case "${resp}" in
    [sSyY]*) return 0 ;;
    [nN]*) return 1 ;;
    *) [ "$3" = y ] ;;
  esac
}

zm_usage() {
  if [ "${ZM_LANG}" = es ]; then
    cat <<USAGE
Uso: sh ${ZM_SCRIPT} [opciones]

  --prefix <ruta>       Dónde instalar el CLI (por omisión, /usr/local)
  --cli-only            No instala la interfaz gráfica aunque el paquete la traiga
  --with-daemon         Instala o actualiza el daemon local sin preguntar
  --no-daemon           No toca el daemon local
  --force               Actualiza el daemon aunque tenga trabajos en curso (se cortan)
  --dry-run             Dice lo que haría, sin tocar nada
  --uninstall           Quita lo que instaló (la configuración ~/.config/ZFSMgr no se toca)
  -h, --help            Esta ayuda
USAGE
  else
    cat <<USAGE
Usage: sh ${ZM_SCRIPT} [options]

  --prefix <path>       Where to install the CLI (default /usr/local)
  --cli-only            Do not install the GUI even if the package has it
  --with-daemon         Install or update the local daemon without asking
  --no-daemon           Leave the local daemon alone
  --force               Update the daemon even with jobs in progress (they are cut)
  --dry-run             Say what would be done, touching nothing
  --uninstall           Remove what was installed (~/.config/ZFSMgr is never touched)
  -h, --help            This help
USAGE
  fi
  [ -n "${ZM_USAGE_EXTRA:-}" ] && zm_t "${ZM_USAGE_EXTRA_ES}" "${ZM_USAGE_EXTRA}" && echo
  return 0
}

zm_options() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --prefix) shift; PREFIX="${1:?$(zm_t 'falta la ruta' 'missing path')}"; shift ;;
      --cli-only) CLI_ONLY=1; shift ;;
      --with-daemon) DAEMON=yes; shift ;;
      --no-daemon) DAEMON=no; shift ;;
      --force) FORCE=1; shift ;;
      --dry-run) DRY_RUN=1; shift ;;
      --uninstall) UNINSTALL=1; shift ;;
      --accept-experimental) ACCEPT_EXPERIMENTAL=1; shift ;;
      -h|--help) zm_usage; exit 0 ;;
      *) zm_err "$(zm_t 'opción desconocida' 'unknown option'): $1"; zm_usage >&2; exit 2 ;;
    esac
  done
  ZM_BIN="${PREFIX}/bin/zfsmgr-cli"
  ZM_SHARE="${PREFIX}/share/zfsmgr"
}

# Each script is for ONE system. Run on another, it refuses and names the right one: there is
# nothing to gain from installing macOS binaries on Linux.
zm_require_system() {
  expected="$1"
  actual="$(uname -s)"
  [ "${actual}" = "${expected}" ] && return 0
  case "${actual}" in
    Linux) right=install-linux.sh ;;
    Darwin) right=install-macos.sh ;;
    FreeBSD) right=install-freebsd.sh ;;
    *) right="" ;;
  esac
  if [ -n "${right}" ]; then
    zm_die "$(zm_t "este guion es para ${expected} y esto es ${actual}: use «sh ${right}»" \
                   "this script is for ${expected} and this is ${actual}: use \"sh ${right}\"")"
  fi
  zm_die "$(zm_t "este guion es para ${expected} y esto es ${actual}, que no está soportado" \
                 "this script is for ${expected} and this is ${actual}, which is not supported")"
}

# <system>-<architecture>: the SAME key the client uses to find its agent.
zm_key() {
  os="$1"
  machine="$(uname -m)"
  case "${os}-${machine}" in
    macos-arm64)                         ZM_KEY=macos-arm64 ;;
    macos-x86_64)                        ZM_KEY=macos-amd64 ;;
    linux-x86_64|linux-amd64)            ZM_KEY=linux-x86_64 ;;
    freebsd-amd64|freebsd-x86_64)        ZM_KEY=freebsd-x86_64 ;;
    *) zm_die "$(zm_t 'arquitectura no soportada' 'unsupported architecture'): $(uname -s) ${machine}" ;;
  esac
  zm_log "$(uname -s) ${machine} -> ${ZM_KEY}"
}

# Elevate only when needed, and say so BEFORE the password prompt. The question is not «can I
# write to <prefix>/bin?» —it may not exist yet— but «can I create that tree?», answered by
# walking up to the first directory that exists. Asked wrongly, a perfectly writable
# `--prefix $HOME/.local` asked for sudo.
zm_writable() {
  d="$1"
  while [ ! -d "${d}" ] && [ "${d}" != "/" ] && [ -n "${d}" ]; do
    d="$(dirname "${d}")"
  done
  [ -w "${d}" ]
}

zm_elevate_for() {
  path="$1"
  if [ "$(id -u)" -eq 0 ] || zm_writable "${path}"; then
    ZM_SUDO=""
  elif command -v sudo >/dev/null 2>&1; then
    ZM_SUDO="sudo"
    zm_log "$(zm_t "${path} es de root: se usará sudo y pedirá su contraseña." \
                   "${path} belongs to root: sudo will be used and will ask for your password.")"
  else
    zm_die "$(zm_t "no se puede escribir en ${path} y no hay sudo; use --prefix \$HOME/.local" \
                   "cannot write to ${path} and there is no sudo; use --prefix \$HOME/.local")"
  fi
}

# The daemon is ALWAYS root's, wherever the CLI goes.
zm_elevate_root() {
  if [ "$(id -u)" -eq 0 ]; then
    ZM_SUDO_ROOT=""
  elif command -v sudo >/dev/null 2>&1; then
    ZM_SUDO_ROOT="sudo"
  else
    ZM_SUDO_ROOT="__none__"
  fi
}

zm_run() {
  if [ "${DRY_RUN}" -eq 1 ]; then
    echo "  [dry-run] ${ZM_SUDO} $*"
  else
    ${ZM_SUDO} "$@"
  fi
}

# Checksums BEFORE copying. A ZIP downloaded halfway stops here, not halfway through the
# installation with half the files in place.
zm_checksums() {
  damaged="$(zm_t 'el paquete está dañado: las sumas no cuadran' 'the package is damaged: checksums do not match')"
  [ -f "${ZM_HERE}/MANIFIESTO.sha256" ] \
    || zm_die "$(zm_t 'falta MANIFIESTO.sha256: el paquete está incompleto' 'MANIFIESTO.sha256 is missing: the package is incomplete')"
  if command -v sha256sum >/dev/null 2>&1; then
    ( cd "${ZM_HERE}" && sha256sum -c --quiet MANIFIESTO.sha256 ) || zm_die "${damaged}"
  elif command -v shasum >/dev/null 2>&1; then
    ( cd "${ZM_HERE}" && shasum -a 256 -c MANIFIESTO.sha256 >/dev/null ) || zm_die "${damaged}"
  elif command -v sha256 >/dev/null 2>&1; then
    # FreeBSD: `sha256 -c` checks ONE sum, so the manifest is walked.
    while read -r sum file; do
      [ "$(cd "${ZM_HERE}" && sha256 -q "${file}")" = "${sum}" ] \
        || zm_die "$(zm_t "el paquete está dañado: no cuadra ${file}" "the package is damaged: ${file} does not match")"
    done < "${ZM_HERE}/MANIFIESTO.sha256"
  else
    zm_log "$(zm_t 'aviso: sin sha256sum, shasum ni sha256: no se comprueba la integridad' \
                   'warning: no sha256sum, shasum or sha256: integrity is not checked')"
    return 0
  fi
  zm_log "$(zm_t 'sumas comprobadas' 'checksums verified')"
}

# --- OpenZFS: if present it must be good enough. If absent, the client installs anyway.
#
# This machine may have no ZFS and still be useful: the CLI and the GUI are CLIENTS, talking
# to other machines' daemons. Where ZFS is needed is on the machine that runs the DAEMON, so
# the daemon is offered only with a good OpenZFS.
#
# The paths are named by hand: a script's PATH is not the terminal's, and on macOS OpenZFS
# lives in /usr/local/zfs/bin. stderr is joined to stdout: what tells «not there» from
# «there but the module does not load» comes on the former.
zm_openzfs() {
  ZM_ZFS=no
  raw=""
  found=0
  for z in zfs /usr/local/zfs/bin/zfs /usr/local/bin/zfs /sbin/zfs /usr/sbin/zfs; do
    if command -v "${z}" >/dev/null 2>&1; then
      raw="$("${z}" version 2>&1 || true)"
      found=1
      break
    fi
  done
  version="$(printf '%s' "${raw}" | sed -n 's/.*zfs-\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)"
  if [ "${found}" -eq 0 ]; then
    zm_log "$(zm_t 'sin OpenZFS en esta máquina: se instala como CLIENTE.' \
                   'no OpenZFS on this machine: installing as a CLIENT.')"
    zm_log "$(zm_t '  podrá manejar otras máquinas; la conexión Local no tendrá nada que enseñar.' \
                   '  it can manage other machines; the Local connection will have nothing to show.')"
    return 0
  fi
  if [ -z "${version}" ]; then
    # Blocking on something that could not be measured would be making up a verdict.
    zm_err "$(zm_t 'aviso: hay OpenZFS pero no se pudo leer su versión. Dijo:' \
                   'warning: OpenZFS is there but its version could not be read. It said:') ${raw}"
    zm_err "$(zm_t '  se instala igual; el daemon local no se ofrece hasta que se arregle.' \
                   '  installing anyway; the local daemon is not offered until that is fixed.')"
    ZM_ZFS=unreadable
    return 0
  fi
  major="${version%%.*}"
  minor="${version#*.}"
  if [ "${major}" -lt "${ZM_ZFS_MIN_MAJOR}" ] \
     || { [ "${major}" -eq "${ZM_ZFS_MIN_MAJOR}" ] && [ "${minor}" -lt "${ZM_ZFS_MIN_MINOR}" ]; }; then
    zm_err ""
    zm_err "$(zm_t "ZFSMgr no se instala: esta máquina tiene OpenZFS ${version} y hace falta" \
                   "ZFSMgr is not installed: this machine has OpenZFS ${version} and it needs")"
    zm_err "$(zm_t "${ZM_ZFS_MIN_MAJOR}.${ZM_ZFS_MIN_MINOR} o posterior. Actualice OpenZFS y vuelva a ejecutar esto." \
                   "${ZM_ZFS_MIN_MAJOR}.${ZM_ZFS_MIN_MINOR} or later. Update OpenZFS and run this again.")"
    exit 1
  fi
  zm_log "OpenZFS ${version}: $(zm_t 'vale' 'ok')"
  ZM_ZFS=ok
}

# Does this package carry this machine's GUI? The CLI-only one has no `gui/`.
zm_has_gui() {
  [ -d "${ZM_HERE}/gui/${ZM_KEY}" ]
}

# The standalone CLI: binary in <prefix>/bin and, beside it, <prefix>/share/zfsmgr/{i18n,agents}.
# Those are the paths the program looks in from its own executable; moving them separately
# leaves it speaking only Spanish and unable to install daemons.
zm_install_cli() {
  source="${ZM_HERE}/cli/zfsmgr-cli-${ZM_KEY}"
  [ -f "${source}" ] || zm_die "$(zm_t "el paquete no trae el CLI de ${ZM_KEY}" "the package has no CLI for ${ZM_KEY}")"
  zm_run mkdir -p "${PREFIX}/bin" "${ZM_SHARE}/i18n" "${ZM_SHARE}/agents"
  # A LINK there —what the macOS GUI installation leaves— is removed first: `install` over a
  # link would write INTO the .app.
  [ -L "${ZM_BIN}" ] && zm_run rm -f "${ZM_BIN}"
  zm_run install -m 0755 "${source}" "${ZM_BIN}"
  zm_install_shared "${ZM_SHARE}"
}

# Catalogues and agents. ALL the agents, not only this machine's: they are what lets this
# machine install the daemon on one of another system.
zm_install_shared() {
  dest="$1"
  zm_run mkdir -p "${dest}/i18n" "${dest}/agents"
  for j in "${ZM_HERE}"/i18n/*.json; do
    zm_run install -m 0644 "${j}" "${dest}/i18n/"
  done
  for d in "${ZM_HERE}"/agents/*/; do
    k="$(basename "${d}")"
    zm_run mkdir -p "${dest}/agents/${k}"
    for a in "${d}"*; do
      zm_run install -m 0755 "${a}" "${dest}/agents/${k}/"
    done
  done
  # And the CLIs of every platform: the ones `install-daemon` sends to another machine to
  # update its zfsmgr-cli. In <share>/cli/<platform>/zfsmgr-cli[.exe].
  for c in "${ZM_HERE}"/cli/zfsmgr-cli-*; do
    [ -f "${c}" ] || continue
    n="$(basename "${c}")"
    k="${n#zfsmgr-cli-}"
    ext=""
    case "${k}" in *.exe) k="${k%.exe}"; ext=".exe" ;; esac
    zm_run mkdir -p "${dest}/cli/${k}"
    zm_run install -m 0755 "${c}" "${dest}/cli/${k}/zfsmgr-cli${ext}"
  done
}

zm_uninstall_cli() {
  zm_run rm -f "${ZM_BIN}"
  zm_run rm -rf "${ZM_SHARE}"
}

# And it is CHECKED that it starts: the only proof that the installation works.
zm_test_cli() {
  [ "${DRY_RUN}" -eq 1 ] && return 0
  if ! out="$("${ZM_BIN}" version 2>&1)"; then
    zm_err "$(zm_t "instalado en ${ZM_BIN}, pero NO arranca:" "installed at ${ZM_BIN}, but it does NOT start:")"
    zm_err "${out}"
    exit 1
  fi
  zm_log "${out}  ->  ${ZM_BIN}"
  # The PATH HERE is not the user's: under sudo it is `secure_path`, and over SSH without a
  # terminal the minimal one. It warned on mbp and mmela with /usr/local/bin in their
  # sessions' PATH. /etc/paths and /etc/paths.d are checked too: macOS takes it from there for
  # real sessions.
  in_path=0
  case ":${PATH}:" in *":${PREFIX}/bin:"*) in_path=1 ;; esac
  if [ "${in_path}" = 0 ] && cat /etc/paths /etc/paths.d/* 2>/dev/null | grep -qx "${PREFIX}/bin"; then
    in_path=1
  fi
  [ "${in_path}" = 1 ] || zm_log "$(zm_t "aviso: ${PREFIX}/bin no está en su PATH." "warning: ${PREFIX}/bin is not in your PATH.")"
}

# The GUI really starts: `--version` answers AFTER loading Qt and its platform plugin, which
# is what fails when a library is missing. Without a display, offscreen.
zm_test_gui() {
  gui="$1"
  [ "${DRY_RUN}" -eq 1 ] && return 0
  if ! out="$(QT_QPA_PLATFORM=offscreen "${gui}" --version 2>&1)"; then
    zm_err "$(zm_t "la interfaz se instaló en ${gui}, pero NO arranca:" "the GUI was installed at ${gui}, but it does NOT start:")"
    zm_err "${out}"
    exit 1
  fi
  zm_log "${out}  ->  ${gui}"
}

# --- Jobs in progress on the daemon already there, BEFORE touching anything.
#
# Reinstalling the daemon restarts the service, and whatever it was copying dies halfway: two
# copies of 16 and 50 GB were left half done on fc16 on 2026-10-10 by a reinstall. It is asked
# with the PACKAGE's CLI —the installed one may predate knowing how— and before installing
# anything, so a new CLI is not left beside an old daemon if it has to stop.
#
# With --with-daemon and jobs in progress, it stops touching nothing. If it was only going to
# ask, the CLI is installed and the daemon left as it is. --force reinstalls anyway.
zm_check_jobs() {
  [ "${DAEMON}" = no ] && return 0
  [ -f /usr/local/libexec/zfsmgr-agent ] || return 0
  [ "${DRY_RUN}" -eq 1 ] && return 0
  [ "${FORCE}" -eq 1 ] && return 0
  zm_elevate_root
  [ "${ZM_SUDO_ROOT}" = "__none__" ] && return 0
  new="${ZM_HERE}/cli/zfsmgr-cli-${ZM_KEY}"
  [ -x "${new}" ] || return 0
  # With `set -e` a non-zero code would end the script right here: it is collected apart.
  rc=0
  ${ZM_SUDO_ROOT} "${new}" --lang "${ZM_LANG}" install-daemon-local --check-jobs || rc=$?
  case ${rc} in
    0) return 0 ;;
    3)
      if [ "${DAEMON}" = yes ]; then
        zm_die "$(zm_t 'no se ha instalado nada. Espere a que terminen o repita con --force.' \
                       'nothing was installed. Wait for them to end, or repeat with --force.')"
      fi
      zm_err "$(zm_t 'el daemon NO se actualiza mientras tenga trabajos en curso (--force lo haría).' \
                     'the daemon is NOT updated while it has jobs in progress (--force would).')"
      DAEMON=no
      ;;
    *) return 0 ;;   # could not ask: no jobs to protect
  esac
}

# --- The local daemon.
#
# The CLI itself installs it with `install-daemon-local`, which runs THE SAME script that
# `install-daemon` sends over SSH. So the systemd, launchd and rc.d logic lives in one place.
# `$1` is the installed CLI, which finds the agent beside itself.
zm_daemon() {
  cli="$1"
  [ "${DAEMON}" = no ] && return 0
  if [ "${ZM_ZFS}" != ok ]; then
    [ "${DAEMON}" = yes ] && zm_err "$(zm_t 'aviso: --with-daemon, pero sin un OpenZFS válido el daemon no se instala.' \
                                            'warning: --with-daemon, but without a valid OpenZFS the daemon is not installed.')"
    return 0
  fi
  zm_elevate_root
  if [ "${ZM_SUDO_ROOT}" = "__none__" ]; then
    zm_err "$(zm_t 'aviso: el daemon necesita root y no hay sudo: no se instala.' \
                   'warning: the daemon needs root and there is no sudo: not installed.')"
    return 0
  fi
  there=0
  [ -f /usr/local/libexec/zfsmgr-agent ] && there=1
  if [ "${DAEMON}" = ask ]; then
    # Without a terminal nobody would answer, and it is not decided for the user.
    if [ ! -t 0 ]; then
      if [ "${there}" -eq 1 ]; then
        zm_log "$(zm_t 'hay un daemon instalado y NO se ha actualizado (sin terminal para preguntar).' \
                       'a daemon is installed and was NOT updated (no terminal to ask).')"
      fi
      zm_log "$(zm_t 'para instalar o actualizar el daemon local: --with-daemon' \
                     'to install or update the local daemon: --with-daemon')"
      return 0
    fi
    if [ "${there}" -eq 1 ]; then
      zm_ask 'Hay un daemon instalado en esta máquina. ¿Actualizarlo a esta versión?' \
             'A daemon is installed on this machine. Update it to this version?' y || return 0
    else
      zm_ask 'Esta máquina tiene OpenZFS. ¿Instalar el daemon, para manejarla desde otras?' \
             'This machine has OpenZFS. Install the daemon, to manage it from others?' n || return 0
    fi
  fi
  if [ "${DRY_RUN}" -eq 1 ]; then
    echo "  [dry-run] ${ZM_SUDO_ROOT} ${cli} install-daemon-local"
    return 0
  fi
  zm_log "$(zm_t 'instalando el daemon local…' 'installing the local daemon…')"
  failed="$(zm_t 'el daemon NO se instaló (ver arriba)' 'the daemon was NOT installed (see above)')"
  if [ "${FORCE}" -eq 1 ]; then
    ${ZM_SUDO_ROOT} "${cli}" --lang "${ZM_LANG}" install-daemon-local --force || zm_die "${failed}"
  else
    ${ZM_SUDO_ROOT} "${cli}" --lang "${ZM_LANG}" install-daemon-local || zm_die "${failed}"
  fi
}

zm_uninstall_daemon() {
  cli="$1"
  [ -f /usr/local/libexec/zfsmgr-agent ] || return 0
  [ "${DAEMON}" = no ] && return 0
  if [ "${DAEMON}" = ask ]; then
    [ -t 0 ] || {
      zm_log "$(zm_t 'el daemon local se deja (sin terminal para preguntar; --with-daemon lo quita)' \
                     'the local daemon is left (no terminal to ask; --with-daemon removes it)')"
      return 0
    }
    zm_ask '¿Quitar también el daemon de esta máquina?' 'Remove this machine'"'"'s daemon too?' n || return 0
  fi
  zm_elevate_root
  [ "${ZM_SUDO_ROOT}" = "__none__" ] && {
    zm_err "$(zm_t 'aviso: quitar el daemon necesita root y no hay sudo' 'warning: removing the daemon needs root and there is no sudo')"
    return 0
  }
  if [ "${DRY_RUN}" -eq 1 ]; then
    echo "  [dry-run] ${ZM_SUDO_ROOT} ${cli} uninstall-daemon-local"
    return 0
  fi
  ${ZM_SUDO_ROOT} "${cli}" --lang "${ZM_LANG}" uninstall-daemon-local \
    || zm_err "$(zm_t 'aviso: el daemon no se pudo quitar del todo' 'warning: the daemon could not be fully removed')"
}

# The end of every script.
zm_done_dry_run() {
  [ "${DRY_RUN}" -eq 1 ] && zm_log "$(zm_t 'simulacro terminado; no se ha tocado nada' 'dry run finished; nothing was touched')"
  return 0
}
zm_done_uninstall() {
  zm_log "$(zm_t 'hecho. La configuración de ~/.config/ZFSMgr NO se toca.' 'done. The configuration in ~/.config/ZFSMgr is NOT touched.')"
}
