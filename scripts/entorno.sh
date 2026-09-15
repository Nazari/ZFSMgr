# Dónde viven las cadenas de herramientas EXTERNAS. Se incluye desde los guiones de
# compilación; no se ejecuta suelto.
#
# Son Qt, los OpenSSL cruzados y el sysroot de FreeBSD: cosas que el proyecto necesita para
# compilar pero que NO son el proyecto, así que no van al git —`entorno/` está ignorado— ni
# tienen por qué ensuciar el directorio personal, que es donde estaban: `~/Qt` (12 GB),
# `~/opt` y `~/sysroots`, tres entradas en la raíz del HOME de alguien que no las puso ahí
# a propósito.
#
# Se resuelve en tres pasos, y el orden importa:
#
#   1. `ZFSMGR_ENTORNO` del entorno, si viene puesta. Es la salida para quien las tenga en
#      un disco aparte, o para probar otra versión de Qt sin tocar nada.
#   2. `<proyecto>/entorno`, si existe. El sitio nuevo.
#   3. `$HOME`, si no. El sitio VIEJO, y por eso no se borra este paso: un clon que todavía
#      no haya movido nada sigue compilando igual, y la migración no es obligatoria.
#
# Cada guion lo incluye DESPUÉS de definir PROJECT_ROOT.
if [ -z "${ZFSMGR_ENTORNO:-}" ]; then
    if [ -d "${PROJECT_ROOT}/entorno" ]; then
        ZFSMGR_ENTORNO="${PROJECT_ROOT}/entorno"
    else
        ZFSMGR_ENTORNO="${HOME}"
    fi
fi
export ZFSMGR_ENTORNO
