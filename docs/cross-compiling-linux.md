# Cross-compiling desde Linux

Todas las plataformas se compilan **desde este mismo equipo**, con las toolchains
cruzadas que deja `scripts/provision-cross-targets.sh`. No hace falta ninguna máquina
ajena ni ningún contenedor.

Ha habido dos intentos anteriores, los dos retirados, y conviene saber por qué:

- `scripts/buildall.sh` lanzaba los builds por SSH en máquinas dedicadas de
  macOS/FreeBSD/Windows. Esas máquinas ya no existen.
- `docker/build.sh` metía las toolchains en una imagen. Se retiró en agosto de 2026
  porque **no aportaba ninguna capacidad**: la imagen se aprovisionaba ejecutando este
  mismo `provision-cross-targets.sh`, así que las dos formas salían del mismo sitio, y
  encima la imagen no era autosuficiente —macOS necesitaba `/opt/osxcross` montado del
  anfitrión, y nunca podrá llevarlo dentro porque el SDK de Apple no se redistribuye—.
  Costaba unos 48 GB entre imagen y su propio `builds/` para hacer lo mismo.

Lo que se publica sale de aquí: `scripts/release-github.sh` llama a `buildall-cross.sh`.
La integración continua solo compila y prueba el CLI, el agente y la interfaz Java en Linux.

Desde la 2.0 no hay Qt en ninguna parte: cada objetivo compila `zfsmgr-cli` y
`zfsmgr_agent`, y la interfaz es UN `.jar` (gui-java/) que se compila con Maven.

La base del cross-compiling local:

- `scripts/build-cross.sh`
- `scripts/provision-cross-targets.sh`
- `toolchains/windows-mingw64.cmake`
- `toolchains/freebsd-clang.cmake`
- `toolchains/macos-osxcross.cmake`

## Dónde viven las cadenas de herramientas: `<entorno>`

Los OpenSSL cruzados y el sysroot de FreeBSD **no son del proyecto** pero hacen falta
para compilarlo. Viven en `entorno/` dentro del árbol —ignorado por git— y no en el
directorio personal, que es donde estaban: `~/opt` y `~/sysroots`,
entradas en la raíz del HOME de alguien que no las puso ahí a propósito.

`scripts/entorno.sh` lo resuelve, y lo incluyen todos los guiones de compilación:

1. `ZFSMGR_ENTORNO` del entorno, si viene puesta. Es la salida para tenerlas en otro disco.
2. `<proyecto>/entorno`, si existe. El sitio de hoy.
3. `$HOME`, si no. El sitio VIEJO, que se conserva a propósito: un clon que no haya migrado
   sigue compilando igual y nadie está obligado a mover nada.

Donde este documento dice `<entorno>`, léase lo que salga de esos tres pasos.


## Estado por plataforma

- Windows: MinGW y OpenSSL estático; el CLI y el agente van solos, sin DLL.
- FreeBSD: sysroot base de FreeBSD; OpenSSL estático del propio sysroot.
- macOS: osxcross + SDK de macOS; dos arquitecturas (amd64 y arm64). El agente se firma
  con `rcodesign` y el certificado estable de `~/.config/zfsmgr-firma`.

## Uso rápido

Provisioning de prerequisitos target:

```bash
./scripts/provision-cross-targets.sh --windows --freebsd
# opcional macOS (requiere SDK local):
# ./scripts/provision-cross-targets.sh --macos --macos-sdk /ruta/al/SDK.tar.xz
```

Qué hace `--freebsd`: descarga `base.txz` y lo extrae como sysroot.

Validación de entorno:

```bash
./scripts/build-cross.sh --target windows --doctor
./scripts/build-cross.sh --target freebsd --doctor
./scripts/build-cross.sh --target macos --doctor
```

Build:

```bash
./scripts/build-cross.sh --target windows
./scripts/build-cross.sh --target freebsd
./scripts/build-cross.sh --target macos
```

Build local multiplaforma (fc16):

```bash
./scripts/buildall-cross.sh --platforms linux,windows,freebsd,macos --macos-arches amd64,arm64
```

Esto deja en `builds/artifacts/<timestamp>/` los dos zip de la versión (`scripts/empaqueta.sh`),
y los agentes en `builds/agents/<plataforma>/`.

## Variables de entorno

### Windows

- `OPENSSL_ROOT_DIR`: OpenSSL MinGW target.
- `CROSS_TRIPLE_WINDOWS` (opcional): default `x86_64-w64-mingw32`.

Autodetecciones implementadas:
- `OPENSSL_ROOT_DIR` desde `<entorno>/opt/openssl-mingw64`.

Comando validado:

```bash
./scripts/build-cross.sh --target windows --build-dir builds/cross-windows --jobs 8
```

### FreeBSD

- `FREEBSD_SYSROOT`: sysroot de FreeBSD.
- `FREEBSD_TRIPLE` (opcional): default `x86_64-unknown-freebsd13`.
- `FREEBSD_CC` / `FREEBSD_CXX` (opcionales).

Autodetección implementada:
- `FREEBSD_SYSROOT` desde `<entorno>/sysroots/freebsd*-amd64`.

### macOS

- `OSXCROSS_TARGET`: autodetectado desde `/opt/osxcross/target/bin` (preferencia `x86_64-apple-darwin*`).
- `OSX_SYSROOT`: SDK macOS.
- `OPENSSL_ROOT_DIR`: prefijo OpenSSL target macOS (autodetecta `<entorno>/opt/openssl-macos-x86_64`).
- `OSXCROSS_CC` / `OSXCROSS_CXX` (opcionales).

Autodetección implementada:
- Añade `/opt/osxcross/target/bin` al `PATH` en `--doctor`.
- `CMAKE_OSX_DEPLOYMENT_TARGET=10.15` por defecto (si no defines `MACOSX_DEPLOYMENT_TARGET`).
- OpenSSL target macOS vía `OPENSSL_ROOT_DIR` + include/lib explícitos.

## Directorios de build

Por defecto:

- `builds/cross-windows`
- `builds/cross-freebsd`
- `builds/cross-macos-<arch>` (desde `buildall-cross.sh`)

Puedes cambiarlo con `--build-dir`.
