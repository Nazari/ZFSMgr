# Cross-compiling desde Linux

Este repo ya soporta builds remotos (macOS/FreeBSD/Windows) mediante `scripts/buildall.sh`.

Además, se añade una base para cross-compiling local en Linux con:

- `scripts/build-cross.sh`
- `scripts/provision-cross-targets.sh`
- `toolchains/windows-mingw64.cmake`
- `toolchains/freebsd-clang.cmake`
- `toolchains/macos-osxcross.cmake`

## Estado por plataforma

- Windows: viable y validado en este Linux (configura y compila `zfsmgr_qt.exe`).
- FreeBSD: viable con sysroot FreeBSD + Qt6 para target FreeBSD.
- macOS: viable con osxcross + SDK macOS + Qt6 para target macOS.

Nota: para Qt, no basta Qt host Linux. Necesitas Qt compilado para cada target.

## Uso rápido

Provisioning de prerequisitos target:

```bash
./scripts/provision-cross-targets.sh --windows --freebsd
# opcional macOS (requiere SDK local):
# ./scripts/provision-cross-targets.sh --macos --macos-sdk /ruta/al/SDK.tar.xz
```

Qué hace `--freebsd` ahora:
- Descarga `base.txz` (sysroot base).
- Descarga `packagesite.pkg` de FreeBSD pkg repo.
- Resuelve `qt6-base` y sus dependencias y las extrae en el sysroot.
- Instala Qt host Linux compatible (misma rama 6.x.y de `qt6-base`) en `~/Qt`.

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

Esto deja artefactos en `builds/artifacts/<timestamp>/`.

Instalador de Windows (Inno) en Linux:

```bash
./scripts/buildall-cross.sh --platforms windows --windows-installer 1
```

Artefactos esperados para Windows:
- `ZFSMgr-<version>-windows.exe`
- `ZFSMgr-Setup-<version>.exe`

Dependencias host recomendadas para Inno por Wine (Ubuntu/Debian):

```bash
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install -y wine64 wine32:i386 winbind xvfb cabextract
```

## Variables de entorno

### Windows

- `QT6_WINDOWS_PREFIX`: prefijo de Qt6 para Windows target.
- `QT_HOST_PATH`: Qt host Linux para herramientas (`moc/uic/rcc`), idealmente misma versión que target.
- `QT_HOST_PATH_CMAKE_DIR`: `.../lib/cmake/Qt6` para `QT_HOST_PATH`.
- `OPENSSL_ROOT_DIR`: OpenSSL MinGW target.
- `CROSS_TRIPLE_WINDOWS` (opcional): default `x86_64-w64-mingw32`.

Autodetecciones implementadas:
- `QT6_WINDOWS_PREFIX` desde `~/Qt/*/mingw_64`.
- `QT_HOST_PATH` desde `~/Qt/<version>/gcc_64` (misma versión) o fallback `/usr`.
- `OPENSSL_ROOT_DIR` desde `~/opt/openssl-mingw64`.

Comando validado:

```bash
./scripts/build-cross.sh --target windows --build-dir builds/cross-windows --jobs 8
```

### FreeBSD

- `FREEBSD_SYSROOT`: sysroot de FreeBSD.
- `QT6_FREEBSD_PREFIX`: prefijo Qt6 target FreeBSD.
- `FREEBSD_TRIPLE` (opcional): default `x86_64-unknown-freebsd13`.
- `FREEBSD_CC` / `FREEBSD_CXX` (opcionales).

Autodetección implementada:
- `FREEBSD_SYSROOT` desde `~/sysroots/freebsd*-amd64`.
- `QT6_FREEBSD_PREFIX` desde `\$FREEBSD_SYSROOT/usr/local` si existe `Qt6Config.cmake`.

### macOS

- `OSXCROSS_TARGET`: autodetectado desde `/opt/osxcross/target/bin` (preferencia `x86_64-apple-darwin*`).
- `OSX_SYSROOT`: SDK macOS.
- `QT6_MACOS_PREFIX`: prefijo Qt6 target macOS.
- `QT_HOST_PATH`: Qt host Linux para `moc/uic/rcc` (autodetectado desde `~/Qt/*/gcc_64` y prioriza la misma versión que target).
- `OPENSSL_ROOT_DIR`: prefijo OpenSSL target macOS (autodetecta `~/opt/openssl-macos-x86_64`).
- `OSXCROSS_CC` / `OSXCROSS_CXX` (opcionales).

Autodetección implementada:
- Añade `/opt/osxcross/target/bin` al `PATH` en `--doctor`.
- `QT6_MACOS_PREFIX` desde `~/Qt/*/{macos,clang_64}`.
- `QT_HOST_PATH` y `QT_HOST_PATH_CMAKE_DIR` desde `~/Qt/*/gcc_64`.
- Fuerza herramientas host Qt (`moc/uic/rcc`) y `Qt6CoreTools_DIR`/`Qt6WidgetsTools_DIR` al kit Linux.
- `CMAKE_OSX_DEPLOYMENT_TARGET=10.15` por defecto (si no defines `MACOSX_DEPLOYMENT_TARGET`).
- OpenSSL target macOS vía `OPENSSL_ROOT_DIR` + include/lib explícitos.

## Directorios de build

Por defecto:

- `builds/cross-windows`
- `builds/cross-freebsd`
- `builds/cross-macos`

Puedes cambiarlo con `--build-dir`.
