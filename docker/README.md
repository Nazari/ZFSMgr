# Compilación multiplataforma en contenedor

Sustituye a `scripts/buildall.sh`, que lanzaba los builds por SSH en máquinas
dedicadas de Windows, FreeBSD y macOS. Esas máquinas ya no existen y el esquema
tenía dos problemas de fondo: no era reproducible (dependía de que cada máquina
estuviera "bien preparada") y el aprovisionamiento local alternativo dependía de
rutas del `$HOME` no versionadas (`~/Qt`, `~/sysroots`, `~/opt`), que es
exactamente lo que se pierde al reinstalar o cambiar de equipo.

## Uso

```bash
# Linux + Windows + FreeBSD (la primera vez construye la imagen: tarda)
docker/build.sh

# Solo algunas plataformas
docker/build.sh --platforms windows,freebsd

# Reconstruir la imagen desde cero
docker/build.sh --rebuild-image

# Entrar a inspeccionar la toolchain
docker/build.sh --shell
```

Los artefactos aparecen en `builds/` del repositorio, igual que en un build local:
el código se monta en `/src` y el contenedor solo aporta la toolchain.

## Qué contiene la imagen

- **Linux nativo**: Qt6 de Ubuntu, OpenSSL, herramientas de AppImage.
- **Windows (cross)**: MinGW-w64, Qt para MinGW vía `aqtinstall`, OpenSSL compilado
  para MinGW, y **Wine + Xvfb** para generar el instalador de Inno Setup.
- **FreeBSD (cross)**: clang, sysroot de `base.txz` y paquetes Qt del repositorio
  oficial de FreeBSD.

### Wine y el instalador de Windows

El compilador de Inno Setup es un ejecutable de Windows, así que sin Wine la fase
`windows-installer-local` abortaba y se llevaba por delante el `.pkg` de FreeBSD, que
se genera después. Antes había que compilar dentro del contenedor con
`--windows-installer 0` y hacer el instalador aparte en el host.

Hace falta `wine32:i386` aunque el prefijo sea de 64 bits: el instalador de Inno
Setup 6 es de 32 bits. Son unos 650 MB extra. La alternativa sería Inno Setup 7,
cuyo instalador sí es x64 nativo, pero eso cambiaría la versión con la que se genera
el instalador que se publica.

Verificado que el `.exe` producido dentro del contenedor es **byte a byte idéntico**
al que se generaba en el host.

La imagen **no duplica la lógica de aprovisionamiento**: ejecuta el propio
`scripts/provision-cross-targets.sh` del repo. Así una máquina aprovisionada a mano
y la imagen quedan equivalentes por construcción, y no hay dos fuentes de verdad que
se desincronicen.

Tampoco duplica la lógica de build: dentro del contenedor se ejecuta
`scripts/buildall-cross.sh`, el mismo que se usaría en local.

## macOS: por qué no está en la imagen

osxcross necesita el SDK de Xcode, y Apple no permite redistribuirlo. Meterlo en la
imagen convertiría el `Dockerfile` en algo que no se puede compartir ni publicar.

Si tiene osxcross ya aprovisionado en el host, se monta:

```bash
docker/build.sh --platforms linux,windows,freebsd,macos
# o con osxcross en otra ruta:
OSXCROSS_DIR=/ruta/a/osxcross docker/build.sh --platforms macos
```

Para aprovisionarlo la primera vez, con su propio SDK:

```bash
scripts/provision-cross-targets.sh --macos
```

## Detalles que no son obvios

**El repositorio se monta en la misma ruta absoluta que en el host**, no en `/src`.
CMake graba rutas absolutas en `CMakeCache.txt`, así que montar en otra ruta invalida
las cachés y obliga a elegir entre compilar siempre dentro o siempre fuera. Con la
ruta idéntica, los mismos directorios de `builds/` sirven para ambos.

**El contenedor se ejecuta con el UID/GID del usuario que lanza el script.** Sin eso,
`builds/` se llenaría de ficheros propiedad de root y el siguiente build nativo en el
host fallaría por permisos.

**La toolchain vive en `/opt/toolchain`, no en `/root`.** Como el contenedor no corre
como root, `/root` (modo 0700) sería ilegible.

**Qt no viene de los paquetes de Ubuntu.** Ubuntu 24.04 trae Qt 6.4.2 y el proyecto
exige `Qt6 6.5` como mínimo (`resources/CMakeLists.txt`). El Qt bueno lo instala
`aqtinstall` durante la construcción de la imagen, y las variables de entorno apuntan
ahí para que ni el build nativo ni el cross cojan por error el del sistema.

## Versión de Qt

La versión que **sí elegimos** está en `qt-version.txt`, en la raíz del repositorio, y
la leen tanto el CI (`.github/workflows/build-packages.yml`) como el aprovisionamiento
(`scripts/provision-cross-targets.sh`) y esta imagen. Antes el CI tenía fijado 6.6.3
mientras en local se usaba 6.8.3, sin nada que impidiera la divergencia.

Al cambiarla hay que reconstruir la imagen: `docker/build.sh --rebuild-image`.

Dos versiones **no** se alinean, y es correcto que no lo hagan:

- **FreeBSD**: su Qt viene del repositorio de paquetes de FreeBSD (hoy 6.11.1), tanto
  en el cross como en el job de CI, que hace `pkg install qt6-base`. Fijarla exigiría
  compilar Qt para FreeBSD por nuestra cuenta. El aprovisionamiento detecta esa
  versión e instala un Qt host de Linux que le haga juego, porque las herramientas de
  Qt (`moc`, `rcc`) deben ser de la misma versión que el Qt de destino.
- **Build nativo de Linux en el host**: usa el Qt de la distribución.

## Relación con el CI

Esto no sustituye a `.github/workflows/build-packages.yml`, que compila en runners
nativos de GitHub (Linux, macOS, Windows) y en una VM de FreeBSD, usando
`scripts/build-linux.sh`, `build-macos.sh`, `build-windows.ps1` y `build-freebsd.sh`.
Esos scripts siguen siendo necesarios: el contenedor cubre el caso de compilar todo
localmente sin depender de máquinas ajenas, no el de CI.
