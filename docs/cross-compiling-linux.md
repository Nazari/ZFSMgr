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
./scripts/build-cross.sh --target windows --build-dir build-cross-windows --jobs 8
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

- `OSXCROSS_TARGET`: default `x86_64-apple-darwin23`.
- `OSX_SYSROOT`: SDK macOS.
- `QT6_MACOS_PREFIX`: prefijo Qt6 target macOS.
- `OSXCROSS_CC` / `OSXCROSS_CXX` (opcionales).

## Directorios de build

Por defecto:

- `build-cross-windows`
- `build-cross-freebsd`
- `build-cross-macos`

Puedes cambiarlo con `--build-dir`.
