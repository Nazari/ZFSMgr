# El servidor web está abandonado (2026-08-23, a partir de 0.99.2)

**No se compila ni se distribuye.** El código se queda aquí a propósito; para volver a
levantarlo: `cmake -DZFSMGR_BUILD_WEB=ON`.

## Por qué

La razón de existir de este servidor era WebDAV: que un explorador de archivos —Finder,
Explorer, Dolphin— montara los datasets sin escribir un plugin por plataforma. Eso es lo
que no se cumple, y comprobado contra una máquina real (mmela, macOS 26.5) son tres cosas
distintas, ninguna de ellas un descuido:

1. **No hay autenticación HTTP.** `/dav/` exige la cookie `zfsmgr_sesion`; sin ella
   contesta `403 sin sesion`. En el servidor no hay Basic ni Digest. Finder monta con
   usuario y contraseña, nunca con cookies de navegador.
2. **Pegar la URL con `?s=<sesion>` tampoco vale.** La primera petición pasa —`OPTIONS`
   200, `PROPFIND` 207— pero los `<D:href>` de la respuesta salen sin la sesión, así que
   la siguiente petición, que el explorador construye a partir de ellos, vuelve a 403.
3. **El certificado es autofirmado y se rehace.** `CN=zfsmgr-web`, generado por el propio
   servidor en `~/.config/ZFSMgr/web/`, así que ningún cliente puede confiar en él de
   forma estable.

Arreglarlo no es una corrección: es elegir un mecanismo de credenciales, hacer persistente
el certificado y documentar cómo confiar en él. Se decidió no seguir por ahí.

Dentro del navegador, con su sesión, todo esto funciona —incluido `/dav/`—. Lo que no hay
es forma de llegar desde fuera.

## Por qué no se borra

Dos cosas que merece la pena no perder:

- **Es la prueba de que la capa base sirve para un cliente entero sin Qt.** Enlaza solo
  `zfsmgr_commands`, y si algún día dejara de compilar por un símbolo de Qt sería porque
  se ha metido interfaz donde no toca.
- **La implementación de WebDAV** (`dav.h`/`dav.cpp`): OPTIONS, PROPFIND, GET y HEAD, con
  su XML de multiestado y su escapado.

Ver `docs/diseno_tecnico_servidor_web.md`, cuya tabla de exploradores describe lo que se
pretendía y no lo que hay.
