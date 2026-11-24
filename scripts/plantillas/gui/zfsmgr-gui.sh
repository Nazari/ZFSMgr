#!/bin/sh
# Starts the ZFSMgr GUI: a Java 21+ program, the same .jar on every system. The installer
# writes the paths below; the rest is finding a Java that is 21 or later.
#
#   zfsmgr-gui [options of the GUI]      starts it
#   zfsmgr-gui --check                   says which Java it would use, and exits 1 if none
#
# ZFSMGR_JAVA=<path to java> chooses one by hand.
ZM_JAR="%%JAR%%"
ZM_CLI="%%CLI%%"

# 21 or later? `java -version` writes «… version "21.0.5" …» (or "1.8.0_x" for the old ones)
# on stderr.
zm_java_ok() {
  [ -x "$1" ] || return 1
  "$1" -version 2>&1 | awk -F'"' '/version/ { split($2, v, "."); m = v[1]; if (m == "1") m = v[2];
                                              exit !(m + 0 >= 21) }'
}

zm_find_java() {
  for j in "${ZFSMGR_JAVA:-}" "${JAVA_HOME:+${JAVA_HOME}/bin/java}"; do
    [ -n "${j}" ] && zm_java_ok "${j}" && { echo "${j}"; return 0; }
  done
  if [ "$(uname -s)" = Darwin ]; then
    # Never /usr/bin/java on a Mac without Java: it is a stub that opens a «install Java»
    # dialog. java_home answers only with a real one.
    h="$(/usr/libexec/java_home -v 21+ 2>/dev/null)" && zm_java_ok "${h}/bin/java" && { echo "${h}/bin/java"; return 0; }
    for j in /Library/Java/JavaVirtualMachines/*/Contents/Home/bin/java; do
      zm_java_ok "${j}" && { echo "${j}"; return 0; }
    done
    return 1
  fi
  j="$(command -v java 2>/dev/null)" && zm_java_ok "${j}" && { echo "${j}"; return 0; }
  # Installed but not the default one (Debian's alternatives, FreeBSD's ports): the newest.
  for j in $(ls -d /usr/lib/jvm/*/bin/java /usr/local/openjdk*/bin/java 2>/dev/null | sort -r); do
    zm_java_ok "${j}" && { echo "${j}"; return 0; }
  done
  return 1
}

if ! java="$(zm_find_java)"; then
  msg_es="ZFSMgr necesita Java 21 o posterior y no se encuentra ninguno. Instale uno (por ejemplo Temurin 21, https://adoptium.net) o indique cuál con ZFSMGR_JAVA=<ruta de java>."
  msg_en="ZFSMgr needs Java 21 or later and none was found. Install one (for instance Temurin 21, https://adoptium.net) or say which with ZFSMGR_JAVA=<path to java>."
  case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in es*) msg="${msg_es}" ;; *) msg="${msg_en}" ;; esac
  echo "zfsmgr-gui: ${msg}" >&2
  # Started from a desktop menu nobody sees stderr: say it in a window if there is a way.
  if [ "${1:-}" != --check ] && [ ! -t 2 ]; then
    if [ "$(uname -s)" = Darwin ]; then
      osascript -e "display alert \"ZFSMgr\" message \"${msg}\"" >/dev/null 2>&1 || true
    elif command -v zenity >/dev/null 2>&1; then
      zenity --error --title=ZFSMgr --text="${msg}" >/dev/null 2>&1 || true
    elif command -v kdialog >/dev/null 2>&1; then
      kdialog --error "${msg}" --title ZFSMgr >/dev/null 2>&1 || true
    fi
  fi
  exit 1
fi

if [ "${1:-}" = --check ]; then
  echo "$("${java}" -version 2>&1 | head -1)  (${java})"
  [ -f "${ZM_JAR}" ] || { echo "zfsmgr-gui: falta / missing ${ZM_JAR}" >&2; exit 1; }
  exit 0
fi

# ZFSMGR_JAVA_OPTS: options for the JVM, unquoted on purpose (several words). The macOS .app
# gives its Dock name and icon this way.
# shellcheck disable=SC2086
exec "${java}" ${ZFSMGR_JAVA_OPTS:-} -jar "${ZM_JAR}" --cli "${ZM_CLI}" "$@"
