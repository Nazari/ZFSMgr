# Lógica común de los instaladores de Unix: Linux, macOS y FreeBSD.
#
# POSIX sh, sin bash: FreeBSD no lo trae de serie, y un instalador que necesita instalar algo
# antes de instalar no es un instalador. Se carga con `.` desde cada install-<sistema>.sh.
#
# Lo que hay aquí es lo que NO cambia entre sistemas: las opciones, las sumas, la versión de
# OpenZFS, la elevación, el CLI suelto y el daemon. Lo que cambia —dónde va la GUI y cómo se
# engancha al escritorio— vive en el guion de cada sistema.

ZM_VERSION="@VERSION@"
ZM_ZFS_MIN_MAYOR=@ZFSMIN_MAYOR@
ZM_ZFS_MIN_MENOR=@ZFSMIN_MENOR@

PREFIJO="${PREFIJO:-/usr/local}"
SIMULAR=0
DESINSTALAR=0
SOLO_CLI=0
DAEMON=preguntar            # preguntar | si | no
ACEPTO_EXPERIMENTAL=0
FORZAR=0                    # reinstalar el daemon aunque tenga trabajos en curso

zm_log() { echo "[zfsmgr] $*"; }
zm_err() { echo "[zfsmgr] $*" >&2; }
zm_muere() { zm_err "$*"; exit 1; }

zm_uso() {
  cat <<USO
Uso: sh ${ZM_GUION} [opciones]

  --prefijo <ruta>      Dónde instalar el CLI (por omisión, /usr/local)
  --solo-cli            No instala la interfaz gráfica aunque el paquete la traiga
  --con-daemon          Instala o actualiza el daemon local sin preguntar
  --sin-daemon          No toca el daemon local
  --forzar              Actualiza el daemon aunque tenga trabajos en curso (se cortan)
  --simular             Dice lo que haría, sin tocar nada
  --desinstalar         Quita lo que instaló (la configuración ~/.config/ZFSMgr no se toca)
  -h, --help            Esta ayuda
USO
  [ -n "${ZM_USO_EXTRA:-}" ] && printf '%s\n' "${ZM_USO_EXTRA}"
  return 0
}

zm_opciones() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --prefijo) shift; PREFIJO="${1:?falta la ruta}"; shift ;;
      --solo-cli) SOLO_CLI=1; shift ;;
      --con-daemon) DAEMON=si; shift ;;
      --sin-daemon) DAEMON=no; shift ;;
      --forzar) FORZAR=1; shift ;;
      --simular) SIMULAR=1; shift ;;
      --desinstalar) DESINSTALAR=1; shift ;;
      --acepto-experimental) ACEPTO_EXPERIMENTAL=1; shift ;;
      -h|--help) zm_uso; exit 0 ;;
      *) zm_err "opción desconocida: $1"; zm_uso >&2; exit 2 ;;
    esac
  done
  ZM_BIN="${PREFIJO}/bin/zfsmgr-cli"
  ZM_COMPARTIDO="${PREFIJO}/share/zfsmgr"
}

# Cada guion es de UN sistema. Ejecutado en otro, se niega y dice cuál es el bueno: no hay
# nada que ganar con intentar instalar binarios de macOS en Linux.
zm_exige_sistema() {
  esperado="$1"
  real="$(uname -s)"
  [ "${real}" = "${esperado}" ] && return 0
  case "${real}" in
    Linux) bueno=install-linux.sh ;;
    Darwin) bueno=install-macos.sh ;;
    FreeBSD) bueno=install-freebsd.sh ;;
    *) bueno="" ;;
  esac
  if [ -n "${bueno}" ]; then
    zm_muere "este guion es para ${esperado} y esto es ${real}: use «sh ${bueno}»"
  fi
  zm_muere "este guion es para ${esperado} y esto es ${real}, que no está soportado"
}

# <sistema>-<arquitectura>: la MISMA clave que usa el cliente para buscar su agente.
zm_clave() {
  so="$1"
  maquina="$(uname -m)"
  case "${so}-${maquina}" in
    macos-arm64)                         ZM_CLAVE=macos-arm64 ;;
    macos-x86_64)                        ZM_CLAVE=macos-amd64 ;;
    linux-x86_64|linux-amd64)            ZM_CLAVE=linux-x86_64 ;;
    freebsd-amd64|freebsd-x86_64)        ZM_CLAVE=freebsd-x86_64 ;;
    *) zm_muere "arquitectura no soportada: $(uname -s) ${maquina}" ;;
  esac
  zm_log "$(uname -s) ${maquina} -> ${ZM_CLAVE}"
}

# Elevar solo si hace falta, y decirlo ANTES de pedir la contraseña. La pregunta no es
# «¿puedo escribir en <prefijo>/bin?» —que puede no existir aún— sino «¿puedo crear ese
# árbol?», que se contesta subiendo hasta el primer directorio que sí exista. Con la
# pregunta mal hecha, un `--prefijo $HOME/.local` perfectamente escribible pedía sudo.
zm_escribible() {
  d="$1"
  while [ ! -d "${d}" ] && [ "${d}" != "/" ] && [ -n "${d}" ]; do
    d="$(dirname "${d}")"
  done
  [ -w "${d}" ]
}

zm_eleva_para() {
  ruta="$1"
  if [ "$(id -u)" -eq 0 ] || zm_escribible "${ruta}"; then
    ZM_ELEVA=""
  elif command -v sudo >/dev/null 2>&1; then
    ZM_ELEVA="sudo"
    zm_log "${ruta} es de root: se usará sudo y pedirá su contraseña."
  else
    zm_muere "no se puede escribir en ${ruta} y no hay sudo; use --prefijo \$HOME/.local"
  fi
}

# El daemon es SIEMPRE de root, se instale el CLI donde se instale.
zm_eleva_root() {
  if [ "$(id -u)" -eq 0 ]; then
    ZM_ELEVA_ROOT=""
  elif command -v sudo >/dev/null 2>&1; then
    ZM_ELEVA_ROOT="sudo"
  else
    ZM_ELEVA_ROOT="__no__"
  fi
}

zm_corre() {
  if [ "${SIMULAR}" -eq 1 ]; then
    echo "  [simulado] ${ZM_ELEVA} $*"
  else
    ${ZM_ELEVA} "$@"
  fi
}

# Las sumas ANTES de copiar. Un ZIP que se bajó a medias se detiene aquí y no a mitad de la
# instalación, con la mitad de los ficheros puestos y la otra mitad no.
zm_sumas() {
  [ -f "${ZM_AQUI}/MANIFIESTO.sha256" ] || zm_muere "falta MANIFIESTO.sha256: el paquete está incompleto"
  if command -v sha256sum >/dev/null 2>&1; then
    ( cd "${ZM_AQUI}" && sha256sum -c --quiet MANIFIESTO.sha256 ) \
      || zm_muere "el paquete está dañado: las sumas no cuadran"
  elif command -v shasum >/dev/null 2>&1; then
    ( cd "${ZM_AQUI}" && shasum -a 256 -c MANIFIESTO.sha256 >/dev/null ) \
      || zm_muere "el paquete está dañado: las sumas no cuadran"
  elif command -v sha256 >/dev/null 2>&1; then
    # FreeBSD: `sha256 -c` compara UNA suma, así que se recorre el manifiesto.
    while read -r suma fichero; do
      [ "$(cd "${ZM_AQUI}" && sha256 -q "${fichero}")" = "${suma}" ] \
        || zm_muere "el paquete está dañado: no cuadra ${fichero}"
    done < "${ZM_AQUI}/MANIFIESTO.sha256"
  else
    zm_log "aviso: sin sha256sum, shasum ni sha256: no se comprueba la integridad"
    return 0
  fi
  zm_log "sumas comprobadas"
}

# --- OpenZFS: si lo hay, tiene que valer. Si no lo hay, también se instala.
#
# Esta máquina puede no tener ZFS y seguir siendo útil: el CLI y la GUI son CLIENTES, y su
# trabajo es hablar con los daemons de otras máquinas. Donde sí hace falta es en la que va a
# llevar el DAEMON, y por eso el daemon solo se ofrece cuando OpenZFS vale.
#
# Las rutas se nombran a mano: el PATH de un guion no es el del terminal, y en macOS OpenZFS
# vive en /usr/local/zfs/bin. Se junta la salida de error con la normal, porque lo que
# distingue «no está» de «está pero el módulo no carga» viene por la segunda.
zm_openzfs() {
  ZM_ZFS=no
  crudo=""
  hallado=0
  for z in zfs /usr/local/zfs/bin/zfs /usr/local/bin/zfs /sbin/zfs /usr/sbin/zfs; do
    if command -v "${z}" >/dev/null 2>&1; then
      crudo="$("${z}" version 2>&1 || true)"
      hallado=1
      break
    fi
  done
  version="$(printf '%s' "${crudo}" | sed -n 's/.*zfs-\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -1)"
  if [ "${hallado}" -eq 0 ]; then
    zm_log "sin OpenZFS en esta máquina: se instala como CLIENTE."
    zm_log "  podrá manejar otras máquinas; la conexión Local no tendrá nada que enseñar."
    return 0
  fi
  if [ -z "${version}" ]; then
    # Bloquear por algo que no se ha podido medir es inventarse un veredicto.
    zm_err "aviso: hay OpenZFS pero no se pudo leer su versión. Dijo: ${crudo}"
    zm_err "  se instala igual; el daemon local no se ofrece hasta que se arregle."
    ZM_ZFS=ilegible
    return 0
  fi
  mayor="${version%%.*}"
  menor="${version#*.}"
  if [ "${mayor}" -lt "${ZM_ZFS_MIN_MAYOR}" ] \
     || { [ "${mayor}" -eq "${ZM_ZFS_MIN_MAYOR}" ] && [ "${menor}" -lt "${ZM_ZFS_MIN_MENOR}" ]; }; then
    zm_err ""
    zm_err "ZFSMgr no se instala: esta máquina tiene OpenZFS ${version} y hace falta"
    zm_err "${ZM_ZFS_MIN_MAYOR}.${ZM_ZFS_MIN_MENOR} o posterior. Actualice OpenZFS y vuelva a ejecutar esto."
    exit 1
  fi
  zm_log "OpenZFS ${version}: vale"
  ZM_ZFS=ok
}

# ¿Trae este paquete la GUI de esta máquina? El de solo CLI no trae `gui/`.
zm_hay_gui() {
  [ -d "${ZM_AQUI}/gui/${ZM_CLAVE}" ]
}

# El CLI suelto: binario en <prefijo>/bin y, a su lado, <prefijo>/share/zfsmgr/{i18n,agents}.
# Son las rutas donde el propio programa los busca a partir de su ejecutable; moverlos por
# separado lo deja hablando solo castellano y sin poder instalar daemons.
zm_instala_cli_suelto() {
  origen="${ZM_AQUI}/cli/zfsmgr-cli-${ZM_CLAVE}"
  [ -f "${origen}" ] || zm_muere "el paquete no trae el CLI de ${ZM_CLAVE}"
  zm_corre mkdir -p "${PREFIJO}/bin" "${ZM_COMPARTIDO}/i18n" "${ZM_COMPARTIDO}/agents"
  # Si había un ENLACE —el que deja la instalación con GUI en macOS—, se quita antes:
  # `install` sobre un enlace escribiría DENTRO del .app.
  [ -L "${ZM_BIN}" ] && zm_corre rm -f "${ZM_BIN}"
  zm_corre install -m 0755 "${origen}" "${ZM_BIN}"
  zm_instala_compartido "${ZM_COMPARTIDO}"
}

# Catálogos y agentes. TODOS los agentes, no solo el de esta máquina: son los que permiten
# instalar el daemon en una máquina de otro sistema desde ésta.
zm_instala_compartido() {
  destino="$1"
  zm_corre mkdir -p "${destino}/i18n" "${destino}/agents"
  for j in "${ZM_AQUI}"/i18n/*.json; do
    zm_corre install -m 0644 "${j}" "${destino}/i18n/"
  done
  for d in "${ZM_AQUI}"/agents/*/; do
    k="$(basename "${d}")"
    zm_corre mkdir -p "${destino}/agents/${k}"
    for a in "${d}"*; do
      zm_corre install -m 0755 "${a}" "${destino}/agents/${k}/"
    done
  done
  # Y los CLI de todas las plataformas: los que `install-daemon` manda a otra máquina para
  # ponerle al día su zfsmgr-cli. En <compartido>/cli/<plataforma>/zfsmgr-cli[.exe].
  for c in "${ZM_AQUI}"/cli/zfsmgr-cli-*; do
    [ -f "${c}" ] || continue
    n="$(basename "${c}")"
    k="${n#zfsmgr-cli-}"
    ext=""
    case "${k}" in *.exe) k="${k%.exe}"; ext=".exe" ;; esac
    zm_corre mkdir -p "${destino}/cli/${k}"
    zm_corre install -m 0755 "${c}" "${destino}/cli/${k}/zfsmgr-cli${ext}"
  done
}

zm_desinstala_cli_suelto() {
  zm_corre rm -f "${ZM_BIN}"
  zm_corre rm -rf "${ZM_COMPARTIDO}"
}

# Y se COMPRUEBA que arranca, que es lo único que demuestra que la instalación sirve.
zm_prueba_cli() {
  [ "${SIMULAR}" -eq 1 ] && return 0
  if ! salida="$("${ZM_BIN}" version 2>&1)"; then
    zm_err "instalado en ${ZM_BIN}, pero NO arranca:"
    zm_err "${salida}"
    exit 1
  fi
  zm_log "${salida}  ->  ${ZM_BIN}"
  # El PATH de AQUÍ no es el del usuario: bajo sudo es el `secure_path`, y por SSH sin
  # terminal es el mínimo. Avisaba en mbp y mmela con /usr/local/bin en el PATH de sus
  # sesiones. Se mira también /etc/paths y /etc/paths.d, que es de donde lo saca macOS para
  # las sesiones de verdad.
  zm_en_path=0
  case ":${PATH}:" in *":${PREFIJO}/bin:"*) zm_en_path=1 ;; esac
  if [ "${zm_en_path}" = 0 ] && cat /etc/paths /etc/paths.d/* 2>/dev/null | grep -qx "${PREFIJO}/bin"; then
    zm_en_path=1
  fi
  case "${zm_en_path}" in
    1) ;;
    *) zm_log "aviso: ${PREFIJO}/bin no está en su PATH." ;;
  esac
}

# La GUI arranca de verdad: `--version` contesta DESPUÉS de cargar Qt y su plugin de
# plataforma, que es lo que falla cuando falta una biblioteca. Sin pantalla, en offscreen.
zm_prueba_gui() {
  gui="$1"
  [ "${SIMULAR}" -eq 1 ] && return 0
  if ! salida="$(QT_QPA_PLATFORM=offscreen "${gui}" --version 2>&1)"; then
    zm_err "la interfaz se instaló en ${gui}, pero NO arranca:"
    zm_err "${salida}"
    exit 1
  fi
  zm_log "${salida}  ->  ${gui}"
}

# --- Trabajos en curso del daemon que ya hay, ANTES de tocar nada.
#
# Reinstalar el daemon reinicia el servicio, y lo que estuviera copiando muere a medias: dos
# copias de 16 y 50 GB quedaron por la mitad en fc16 el 2026-10-10 por una reinstalación. Se
# pregunta con el CLI DEL PAQUETE —el instalado puede ser anterior a saber hacerlo— y antes de
# instalar nada, para no dejar un CLI nuevo con un daemon viejo si hubiera que parar.
#
# Con --con-daemon y trabajos en curso, se para sin tocar nada. Si solo se iba a preguntar,
# se instala el CLI y el daemon se deja como está. --forzar lo reinstala igual.
zm_comprueba_trabajos() {
  [ "${DAEMON}" = no ] && return 0
  [ -f /usr/local/libexec/zfsmgr-agent ] || return 0
  [ "${SIMULAR}" -eq 1 ] && return 0
  [ "${FORZAR}" -eq 1 ] && return 0
  zm_eleva_root
  [ "${ZM_ELEVA_ROOT}" = "__no__" ] && return 0
  nuevo="${ZM_AQUI}/cli/zfsmgr-cli-${ZM_CLAVE}"
  [ -x "${nuevo}" ] || return 0
  # Con `set -e` un código distinto de 0 terminaría el guion aquí mismo: se recoge aparte.
  rc=0
  ${ZM_ELEVA_ROOT} "${nuevo}" install-daemon-local --comprobar || rc=$?
  case ${rc} in
    0) return 0 ;;
    3)
      if [ "${DAEMON}" = si ]; then
        zm_muere "no se ha instalado nada. Espere a que terminen o repita con --forzar."
      fi
      zm_err "el daemon NO se actualiza mientras tenga trabajos en curso (--forzar lo haría)."
      DAEMON=no
      ;;
    *) return 0 ;;   # no se pudo preguntar: no hay trabajos que proteger
  esac
}

# --- El daemon local.
#
# Lo instala el propio CLI con `install-daemon-local`, que ejecuta EL MISMO guion que
# `install-daemon` manda por SSH. Así la lógica de systemd, launchd y rc.d vive en un solo
# sitio. `$1` es el CLI ya instalado, que encuentra el agente junto a sí mismo.
zm_daemon() {
  cli="$1"
  [ "${DAEMON}" = no ] && return 0
  if [ "${ZM_ZFS}" != ok ]; then
    [ "${DAEMON}" = si ] && zm_err "aviso: --con-daemon, pero sin un OpenZFS válido el daemon no se instala."
    return 0
  fi
  zm_eleva_root
  if [ "${ZM_ELEVA_ROOT}" = "__no__" ]; then
    zm_err "aviso: el daemon necesita root y no hay sudo: no se instala."
    return 0
  fi
  ya=0
  [ -f /usr/local/libexec/zfsmgr-agent ] && ya=1
  if [ "${DAEMON}" = preguntar ]; then
    # Sin terminal no se pregunta —nadie contestaría— y no se decide por el usuario.
    if [ ! -t 0 ]; then
      if [ "${ya}" -eq 1 ]; then
        zm_log "hay un daemon instalado y NO se ha actualizado (sin terminal para preguntar)."
      fi
      zm_log "para instalar o actualizar el daemon local: --con-daemon"
      return 0
    fi
    if [ "${ya}" -eq 1 ]; then
      printf '[zfsmgr] Hay un daemon instalado en esta máquina. ¿Actualizarlo a esta versión? [S/n] '
      read -r resp || resp=""
      case "${resp}" in [nN]*) return 0 ;; esac
    else
      printf '[zfsmgr] Esta máquina tiene OpenZFS. ¿Instalar el daemon, para manejarla desde otras? [s/N] '
      read -r resp || resp=""
      case "${resp}" in [sSyY]*) ;; *) return 0 ;; esac
    fi
  fi
  if [ "${SIMULAR}" -eq 1 ]; then
    echo "  [simulado] ${ZM_ELEVA_ROOT} ${cli} install-daemon-local"
    return 0
  fi
  zm_log "instalando el daemon local…"
  if [ "${FORZAR}" -eq 1 ]; then
    ${ZM_ELEVA_ROOT} "${cli}" install-daemon-local --force || zm_muere "el daemon NO se instaló (ver arriba)"
  else
    ${ZM_ELEVA_ROOT} "${cli}" install-daemon-local || zm_muere "el daemon NO se instaló (ver arriba)"
  fi
}

zm_desinstala_daemon() {
  cli="$1"
  [ -f /usr/local/libexec/zfsmgr-agent ] || return 0
  [ "${DAEMON}" = no ] && return 0
  if [ "${DAEMON}" = preguntar ]; then
    [ -t 0 ] || { zm_log "el daemon local se deja (sin terminal para preguntar; --con-daemon lo quita)"; return 0; }
    printf '[zfsmgr] ¿Quitar también el daemon de esta máquina? [s/N] '
    read -r resp || resp=""
    case "${resp}" in [sSyY]*) ;; *) return 0 ;; esac
  fi
  zm_eleva_root
  [ "${ZM_ELEVA_ROOT}" = "__no__" ] && { zm_err "aviso: quitar el daemon necesita root y no hay sudo"; return 0; }
  if [ "${SIMULAR}" -eq 1 ]; then
    echo "  [simulado] ${ZM_ELEVA_ROOT} ${cli} uninstall-daemon-local"
    return 0
  fi
  ${ZM_ELEVA_ROOT} "${cli}" uninstall-daemon-local || zm_err "aviso: el daemon no se pudo quitar del todo"
}
