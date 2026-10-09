#!/usr/bin/env bash
# Los dos artefactos de una versión: `zfsmgr-<ver>.zip` (CLI + interfaz) y
# `zfsmgr-cli-<ver>.zip` (solo CLI). La misma estructura y los mismos guiones; el segundo
# es el primero sin `gui/`. Dentro, un instalador por sistema operativo.
#
# **Se niega a empaquetar si algo no es de esta compilación.** Ya pasó dos veces: un
# agente de agosto empaquetado con el número de la versión nueva —una vuelta atrás que
# nadie vio—, y un Setup de Windows y un AppDir del 26 de agosto que se llamaban 0.99.3.
# El nombre de un fichero no dice de cuándo es; la HUELLA que lleva dentro, sí. Así que se
# lee la huella de cada binario —clientes, agentes, y lo que va dentro del .app, del AppDir
# y del Setup— y todas tienen que ser la misma.
#
# Windows y FreeBSD: el CLI de Windows va solo (enlazado estático) para el zip de solo CLI;
# FreeBSD entra como EXPERIMENTAL y solo con CLI: se ha probado en una sola máquina (15.1) y
# su interfaz nunca se ha compilado.
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLANTILLAS="${RAIZ}/scripts/plantillas"
SALIDA="${RAIZ}/dist"
PODAR=1
SOLO_CLI=0

uso() {
  cat <<'USO'
Uso: empaqueta.sh [--salida <dir>] [--solo-cli] [--sin-podar]

  --salida <dir>   Dónde dejar los .zip (por omisión, dist/)
  --solo-cli       Genera solo zfsmgr-cli-<ver>.zip (no exige las interfaces)
  --sin-podar      No quitar los símbolos de los binarios sueltos; sirve para depurar.
USO
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --salida) shift; SALIDA="${1:?falta el directorio}"; shift ;;
    --solo-cli) SOLO_CLI=1; shift ;;
    --sin-podar) PODAR=0; shift ;;
    -h|--help) uso; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; uso >&2; exit 2 ;;
  esac
done

FALLOS=()
falla() { FALLOS+=("$*"); echo "  ✗ $*" >&2; }
bien() { echo "  ✓ $*"; }

# --- La versión y la huella de ESTA compilación.
#
# La huella se lee de la cabecera que genera CMake al compilar el árbol de Linux, que es lo
# que los clientes llevan como «agente que espero». Pero si alguien cambió el código DESPUÉS
# de compilar, esa cabecera es vieja y todos los binarios podrían coincidir entre sí y estar
# mal a la vez. Por eso antes se mira que ningún fuente de los que entran en la huella sea
# más nuevo que ella.
# La versión, de donde la fija el proyecto. (No está en la caché de CMake: el script anterior
# la buscaba allí, no la encontraba nunca y caía siempre al respaldo de ejecutar el CLI.)
VERSION="$(sed -n 's/^set(ZFSMGR_APP_VERSION_STRING "\(.*\)")/\1/p' "${RAIZ}/resources/CMakeLists.txt" | head -1)"
[[ -n "${VERSION}" ]] || { echo "no se pudo leer la versión de resources/CMakeLists.txt" >&2; exit 1; }

CABECERA="${RAIZ}/builds/linux/generated/zfsmgr/agentversion_generated.h"
[[ -f "${CABECERA}" ]] || { echo "falta ${CABECERA}: compila primero builds/linux" >&2; exit 1; }
HUELLA="$(sed -n 's/.*ZFSMGR_AGENT_VERSION_STRING "\(.*\)".*/\1/p' "${CABECERA}")"
[[ "${HUELLA}" == "${VERSION}."* ]] || { echo "huella «${HUELLA}» no casa con la versión ${VERSION}" >&2; exit 1; }

echo "ZFSMgr ${VERSION}, huella ${HUELLA}"
echo
echo "== El árbol de compilación frente al código"
mas_nuevo="$(find "${RAIZ}/src/base" "${RAIZ}/src/commands" "${RAIZ}/src/daemon" -type f \
               -newer "${CABECERA}" 2>/dev/null | head -3)"
if [[ -n "${mas_nuevo}" ]]; then
  falla "hay código más nuevo que la huella compilada (recompila builds/linux):"
  printf '      %s\n' ${mas_nuevo} >&2
else
  bien "la huella compilada es la del código actual"
fi

# Las huellas que aparecen dentro de un binario. Los clientes llevan la que esperan; los
# agentes, la suya. Tiene que salir exactamente UNA, y ser la de esta compilación.
VER_RE="${VERSION//./\\.}\\.[0-9]{6,}"
# `|| true`: sin coincidencias, grep devuelve 1 y, con pipefail, eso mataba el guion entero
# en vez de informar «ninguna».
huellas_de() { { grep -aoE "${VER_RE}" "$1" 2>/dev/null || true; } | sort -u | tr '\n' ' ' | sed 's/ $//'; }
exige_huella() {
  local que="$1" fichero="$2"
  if [[ ! -f "${fichero}" ]]; then
    falla "${que}: no existe ${fichero#"${RAIZ}/"}"
    return
  fi
  local h
  h="$(huellas_de "${fichero}")"
  if [[ "${h}" == "${HUELLA}" ]]; then
    bien "${que}"
  else
    falla "${que}: lleva «${h:-ninguna}», y esta compilación es ${HUELLA}"
  fi
}

# Los catálogos que una interfaz lleva DENTRO se copiaron al compilarla, y una traducción
# añadida después no está en ellos: el .app hablaría en castellano con la interfaz en
# inglés. Tienen que ser idénticos a los de i18n/.
exige_catalogos() {
  local que="$1" dir="$2" j
  for j in "${RAIZ}"/i18n/es.json "${RAIZ}"/i18n/en.json; do
    if ! cmp -s "${j}" "${dir}/$(basename "${j}")"; then
      falla "${que}: su $(basename "${j}") no es el de i18n/ (recompila esa interfaz)"
      return
    fi
  done
  bien "${que}: catálogos al día"
}

# --- Qué binario va a cada sitio. La clave es «<sistema>-<arquitectura>», la MISMA que usa
# `rutaDelAgente()`; si aquí se escribiera otra, el paquete saldría bien formado y el
# `install-daemon` no encontraría nada.
CLAVES=(linux-x86_64 macos-amd64 macos-arm64 freebsd-x86_64 windows-x86_64)
declare -A CLI=(
  [linux-x86_64]="builds/linux/zfsmgr-cli"
  [macos-amd64]="builds/cross-macos-amd64/zfsmgr-cli"
  [macos-arm64]="builds/cross-macos-arm64/zfsmgr-cli"
  [freebsd-x86_64]="builds/cross-freebsd/zfsmgr-cli"
  [windows-x86_64]="builds/cross-windows/zfsmgr-cli.exe"
)
ext_de() { [[ "$1" == windows-* ]] && echo ".exe" || true; }

echo
echo "== Clientes y agentes"
for k in "${CLAVES[@]}"; do
  exige_huella "CLI ${k}" "${RAIZ}/${CLI[$k]}"
  exige_huella "agente ${k}" "${RAIZ}/builds/agents/${k}/zfsmgr_agent$(ext_de "$k")"
done
# El CLI de Windows del zip de solo CLI viaja SUELTO: no puede depender del runtime de MinGW.
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1 && [[ -f "${RAIZ}/${CLI[windows-x86_64]}" ]]; then
  if x86_64-w64-mingw32-objdump -p "${RAIZ}/${CLI[windows-x86_64]}" | grep -qiE 'DLL Name: (libgcc|libstdc|libwinpthread)'; then
    falla "CLI windows: depende del runtime de MinGW y no arrancaría suelto"
  else
    bien "CLI windows: sin DLL de MinGW"
  fi
fi

# --- Las interfaces, mirando DENTRO de cada una.
APPDIR="${RAIZ}/AppDir"
declare -A APP=(
  [macos-amd64]="${RAIZ}/builds/cross-macos-amd64/ZFSMgr.app"
  [macos-arm64]="${RAIZ}/builds/cross-macos-arm64/ZFSMgr.app"
)
SETUP="${RAIZ}/builds/windows-installer/ZFSMgr-Setup-${VERSION}.exe"
INNO_TMP=""
limpia() { [[ -n "${INNO_TMP}" ]] && rm -rf "${INNO_TMP}"; [[ -n "${TRABAJO:-}" ]] && rm -rf "${TRABAJO}"; return 0; }
trap limpia EXIT

if [[ "${SOLO_CLI}" -eq 0 ]]; then
  echo
  echo "== Interfaz de Linux (AppDir)"
  exige_huella "AppDir: zfsmgr-gui" "${APPDIR}/usr/bin/zfsmgr-gui"
  exige_huella "AppDir: zfsmgr-cli" "${APPDIR}/usr/bin/zfsmgr-cli"
  exige_catalogos "AppDir" "${APPDIR}/usr/share/zfsmgr/i18n"
  for k in "${CLAVES[@]}"; do
    exige_huella "AppDir: agente ${k}" "${APPDIR}/usr/share/zfsmgr/agents/${k}/zfsmgr_agent$(ext_de "$k")"
  done

  for k in macos-arm64 macos-amd64; do
    echo
    echo "== Interfaz de macOS ${k}"
    a="${APP[$k]}"
    plist_ver="$(grep -A1 'CFBundleShortVersionString' "${a}/Contents/Info.plist" 2>/dev/null \
                   | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -1)"
    if [[ "${plist_ver}" == "${VERSION}" ]]; then
      bien "Info.plist: ${plist_ver}"
    else
      falla ".app ${k}: Info.plist dice «${plist_ver:-nada}», y la versión es ${VERSION}"
    fi
    exige_huella ".app ${k}: ZFSMgr" "${a}/Contents/MacOS/ZFSMgr"
    exige_huella ".app ${k}: zfsmgr-cli" "${a}/Contents/MacOS/zfsmgr-cli"
    # Sin agentes dentro, desde la interfaz de un Mac no se puede instalar un daemon en otra
    # máquina: el .app se veía bien y le faltaba media función.
    for kk in "${CLAVES[@]}"; do
      exige_huella ".app ${k}: agente ${kk}" "${a}/Contents/Resources/agents/${kk}/zfsmgr_agent$(ext_de "$kk")"
    done
    exige_catalogos ".app ${k}" "${a}/Contents/Resources/i18n"
  done

  echo
  echo "== Interfaz de Windows (Setup)"
  if [[ ! -f "${SETUP}" ]]; then
    falla "no existe ${SETUP#"${RAIZ}/"}"
  else
    # Primero, mirar DENTRO con innoextract. Si no sabe leer este Setup —la 1.9 de Ubuntu no
    # entiende Inno 6.7—, se mira el directorio del que se compiló: `payload/` queda en
    # disco. Si sus piezas llevan la huella buena Y el Setup es posterior a todas ellas, el
    # Setup se hizo con ellas: uno más viejo que su payload saldría aquí.
    inspeccionado=0
    if command -v innoextract >/dev/null 2>&1; then
      INNO_TMP="$(mktemp -d)"
      if innoextract -q -d "${INNO_TMP}" "${SETUP}" >/dev/null 2>&1; then
        inspeccionado=1
        raiz_setup="${INNO_TMP}"
        echo "  (mirado dentro del Setup con innoextract)"
      fi
    fi
    if [[ "${inspeccionado}" -eq 0 ]]; then
      raiz_setup="$(dirname "${SETUP}")/payload"
      echo "  (innoextract no lee este Setup: se mira su payload/ y que el Setup sea posterior)"
      if [[ ! -d "${raiz_setup}" ]]; then
        falla "Setup: ni innoextract lo lee ni queda su payload/ para comprobarlo"
      elif [[ -n "$(find "${raiz_setup}" -type f -newer "${SETUP}" | head -1)" ]]; then
        falla "Setup: su payload/ tiene ficheros POSTERIORES al Setup: el Setup es viejo"
      else
        bien "Setup: posterior a todo su payload/"
      fi
    fi
    if [[ -d "${raiz_setup:-}" ]]; then
      gui_exe="$(find "${raiz_setup}" -iname 'zfsmgr-gui.exe' -o -iname 'ZFSMgr.exe' | head -1)"
      cli_exe="$(find "${raiz_setup}" -iname 'zfsmgr-cli.exe' | head -1)"
      exige_huella "Setup: interfaz" "${gui_exe:-${raiz_setup}/(sin interfaz)}"
      exige_huella "Setup: zfsmgr-cli" "${cli_exe:-${raiz_setup}/(sin CLI)}"
      [[ -n "${cli_exe}" ]] && exige_catalogos "Setup" "$(dirname "${cli_exe}")/i18n"
      for k in "${CLAVES[@]}"; do
        ag="$(find "${raiz_setup}" -path "*agents/${k}/zfsmgr_agent$(ext_de "$k")" | head -1)"
        exige_huella "Setup: agente ${k}" "${ag:-${raiz_setup}/(sin agente ${k})}"
      done
    fi
  fi
fi

if [[ ${#FALLOS[@]} -gt 0 ]]; then
  echo >&2
  echo "NO se empaqueta: ${#FALLOS[@]} piezas no son de esta compilación o faltan." >&2
  echo "Recompila con scripts/buildall-cross.sh y vuelve a intentarlo." >&2
  exit 1
fi

# --- El mínimo de OpenZFS sale de la cabecera de C++, no de un número escrito aquí.
ZFSMIN_MAYOR="$(sed -n 's/.*kOpenZfsMinimoMayor *= *\([0-9]*\);.*/\1/p' "${RAIZ}/src/base/helpers.h" | head -1)"
ZFSMIN_MENOR="$(sed -n 's/.*kOpenZfsMinimoMenor *= *\([0-9]*\);.*/\1/p' "${RAIZ}/src/base/helpers.h" | head -1)"
[[ -n "${ZFSMIN_MAYOR}" && -n "${ZFSMIN_MENOR}" ]] \
  || { echo "no se pudo leer el mínimo de OpenZFS de src/base/helpers.h" >&2; exit 1; }

poda() {
  [[ "${PODAR}" -eq 1 ]] || return 0
  case "$1" in
    *.exe) x86_64-w64-mingw32-strip "$1" 2>/dev/null || true ;;
    *) llvm-strip "$1" 2>/dev/null || strip "$1" 2>/dev/null || true ;;
  esac
}

# Un guion de plantilla, con sus marcadores sustituidos y sin ninguno sin sustituir.
plantilla() {
  local origen="$1" destino="$2" paquete="$3"
  sed -e "s/@VERSION@/${VERSION}/g" \
      -e "s/@ZFSMIN_MAYOR@/${ZFSMIN_MAYOR}/g" -e "s/@ZFSMIN_MENOR@/${ZFSMIN_MENOR}/g" \
      -e "s/@ZFSMIN@/${ZFSMIN_MAYOR}.${ZFSMIN_MENOR}/g" -e "s/@PAQUETE@/${paquete}/g" \
      "${origen}" > "${destino}"
  if grep -q '@[A-Z_]*@' "${destino}"; then
    echo "quedaron marcadores sin sustituir en ${destino##*/}" >&2
    exit 1
  fi
}

# --- Un árbol por paquete.
TRABAJO="$(mktemp -d)"

arma() {
  local nombre="$1" con_gui="$2" paquete="$3"
  local arbol="${TRABAJO}/${nombre}"
  mkdir -p "${arbol}/cli" "${arbol}/agents" "${arbol}/i18n" "${arbol}/lib"

  for k in "${CLAVES[@]}"; do
    local e; e="$(ext_de "$k")"
    cp -f "${RAIZ}/${CLI[$k]}" "${arbol}/cli/zfsmgr-cli-${k}${e}"
    mkdir -p "${arbol}/agents/${k}"
    cp -f "${RAIZ}/builds/agents/${k}/zfsmgr_agent${e}" "${arbol}/agents/${k}/zfsmgr_agent${e}"
    chmod 0755 "${arbol}/cli/zfsmgr-cli-${k}${e}" "${arbol}/agents/${k}/zfsmgr_agent${e}"
    poda "${arbol}/cli/zfsmgr-cli-${k}${e}"
    poda "${arbol}/agents/${k}/zfsmgr_agent${e}"
  done
  cp -f "${RAIZ}"/i18n/*.json "${arbol}/i18n/"

  for g in instalar-linux.sh instalar-macos.sh instalar-freebsd.sh; do
    plantilla "${PLANTILLAS}/${g}" "${arbol}/${g}" "${paquete}"
    chmod 0755 "${arbol}/${g}"
  done
  plantilla "${PLANTILLAS}/lib/comun.sh" "${arbol}/lib/comun.sh" "${paquete}"
  plantilla "${PLANTILLAS}/LEEME.txt" "${arbol}/LEEME.txt" "${paquete}"
  # Windows. El .ps1 con BOM: sin él, Windows PowerShell 5.1 lo lee como ANSI y destroza
  # los acentos y las comillas «». El .cmd con CRLF, que es lo que cmd.exe espera.
  plantilla "${PLANTILLAS}/instalar-windows.ps1" "${TRABAJO}/ps1.tmp" "${paquete}"
  { printf '\xEF\xBB\xBF'; sed 's/$/\r/' "${TRABAJO}/ps1.tmp"; } > "${arbol}/instalar-windows.ps1"
  plantilla "${PLANTILLAS}/instalar-windows.cmd" "${TRABAJO}/cmd.tmp" "${paquete}"
  sed 's/$/\r/' "${TRABAJO}/cmd.tmp" > "${arbol}/instalar-windows.cmd"

  if [[ "${con_gui}" -eq 1 ]]; then
    # En .tar.gz DENTRO del zip: el .app (frameworks de Qt, `Versions/Current`) y el AppDir
    # (`AppRun`) llevan enlaces simbólicos, y los descompresores de Finder y de Explorer no
    # los conservan de forma fiable. `tar` sí, y es lo que usan los instaladores.
    mkdir -p "${arbol}/gui/linux-x86_64" "${arbol}/gui/macos-arm64" "${arbol}/gui/macos-amd64" \
             "${arbol}/gui/windows-x86_64"
    tar -C "${APPDIR}" -czf "${arbol}/gui/linux-x86_64/ZFSMgr-AppDir.tar.gz" .
    for k in macos-arm64 macos-amd64; do
      tar -C "$(dirname "${APP[$k]}")" -czf "${arbol}/gui/${k}/ZFSMgr.app.tar.gz" ZFSMgr.app
    done
    cp -f "${SETUP}" "${arbol}/gui/windows-x86_64/"
  fi

  # El manifiesto: los instaladores lo comprueban ANTES de copiar nada.
  ( cd "${arbol}" && find . -type f ! -name MANIFIESTO.sha256 | sed 's|^\./||' | LC_ALL=C sort \
      | xargs sha256sum > MANIFIESTO.sha256 )

  mkdir -p "${SALIDA}"
  local zip="${SALIDA}/${nombre}.zip"
  rm -f "${zip}" "${zip}.sha256"
  ( cd "${TRABAJO}" && zip -qry "${zip}" "${nombre}" )
  ( cd "${SALIDA}" && sha256sum "${nombre}.zip" > "${nombre}.zip.sha256" )
  # `du` da bloques de disco, y en ZFS con compresión miente: dijo «1.0K» de un ZIP de 22 MB.
  # El número lo formatea awk y no printf: con LC_NUMERIC en castellano, printf de bash no
  # acepta «22.83» —espera coma—, escribía «22,0 MB» y, con set -e, cortaba el guion.
  printf '  %-34s %s MB  (%s ficheros)\n' "${zip##*/}" \
    "$(LC_ALL=C awk -v b="$(stat -c%s "${zip}")" 'BEGIN{printf "%.1f", b/1048576}')" \
    "$(unzip -Z1 "${zip}" | grep -vc '/$')"
}

echo
echo "== Paquetes"
arma "zfsmgr-cli-${VERSION}" 0 "solo CLI"
[[ "${SOLO_CLI}" -eq 1 ]] || arma "zfsmgr-${VERSION}" 1 "CLI + interfaz"
