# Imagen de toolchain para compilar ZFSMgr para Linux, Windows y FreeBSD desde una
# sola máquina Linux, sin depender de máquinas remotas ni de rutas del home.
#
# Deliberadamente NO contiene la lógica de build: reutiliza los scripts del repo
# (provision-cross-targets.sh, build-cross.sh, buildall-cross.sh) para que no haya
# dos fuentes de verdad que se desincronicen.
#
# macOS queda fuera a propósito: osxcross necesita el SDK de Xcode, que Apple no
# permite redistribuir. Ver docker/README.md para cómo añadirlo montándolo.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

# El Qt de Ubuntu 24.04 es 6.4.2 y el proyecto exige >= 6.5, así que NO se instalan
# los paquetes qt6-*-dev: el Qt bueno lo trae aqtinstall más abajo. Aquí solo van las
# dependencias de sistema que Qt necesita para enlazar y para empaquetar el AppImage.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl ca-certificates \
        python3 python3-venv python3-pip perl make file xz-utils zstd \
        pkg-config desktop-file-utils fuse3 libfuse2t64 \
        libssl-dev zlib1g-dev libgl1-mesa-dev libglx-dev \
        libxkbcommon-dev libxkbcommon-x11-dev libfontconfig1-dev libfreetype-dev \
        libx11-xcb-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev \
        libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render-util0-dev \
        libxcb-shape0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-xinerama0-dev \
        libxcb-xkb-dev libxcb-util-dev \
        mingw-w64 mingw-w64-tools \
        clang lld \
    && rm -rf /var/lib/apt/lists/*

# La toolchain se aprovisiona bajo /opt/toolchain, no bajo /root: el contenedor se
# ejecuta con el UID del usuario del host para que los artefactos no salgan como root,
# y /root es 0700, así que sería ilegible.
ENV HOME=/opt/toolchain
RUN mkdir -p /opt/toolchain

WORKDIR /opt/provision

# El aprovisionamiento se hace con el propio script del repo, así que la imagen y una
# máquina aprovisionada a mano quedan siempre equivalentes por construcción.
COPY scripts/provision-cross-targets.sh /opt/provision/provision-cross-targets.sh
# El script lee la versión de Qt de ../qt-version.txt respecto a su propia ubicación,
# así que se copia al nivel correspondiente: sin esto la imagen usaría el valor de
# reserva y quedaría desalineada al cambiar la versión.
COPY qt-version.txt /opt/qt-version.txt
RUN chmod +x /opt/provision/provision-cross-targets.sh

# Misma versión que qt-version.txt. docker/build.sh la pasa desde ese fichero, para
# que las rutas de abajo no puedan quedarse desalineadas con lo que se instaló.
ARG QT_VERSION=6.8.3

# Windows: Qt para MinGW vía aqtinstall + OpenSSL compilado para MinGW.
# Instala además el Qt host de Linux (gcc_64), que es el que usan las herramientas
# de Qt durante el cross y el que sirve para el build nativo de Linux.
RUN /opt/provision/provision-cross-targets.sh --windows

# FreeBSD: sysroot base.txz + paquetes Qt del repositorio oficial de FreeBSD.
RUN /opt/provision/provision-cross-targets.sh --freebsd

# macOS: SOLO el Qt. osxcross no va en la imagen porque el SDK de Xcode no es
# redistribuible —se monta desde el anfitrión—, pero el Qt para macOS son binarios que
# aqtinstall descarga y no necesita SDK ninguno. Sin este árbol el cruce de macOS compilaba
# el agente y se saltaba la interfaz con un aviso, así que el .app no salía nunca de aquí.
#
# Las herramientas de Qt para macOS (moc, rcc, uic) son Mach-O y NO corren en Linux: las
# pone el Qt host de Linux de la MISMA versión, que ya instaló el paso de Windows.
RUN /opt/provision/provision-cross-targets.sh --macos-image

# Wine para el instalador de Windows. El compilador de Inno Setup es un ejecutable
# de Windows, así que sin esto build-windows-inno-linux.sh aborta con "wine no está
# instalado" y buildall-cross.sh muere en la fase windows-installer-local ANTES de
# generar el .pkg de FreeBSD, que va después. Al no tenerlo había que compilar dentro
# del contenedor con --windows-installer 0 y generar el instalador aparte en el host.
#
# Se instala "wine", no "wine64": wine64 deja los binarios fuera del PATH (quedan
# bajo /usr/lib/wine) y el script busca "wine" y "winepath".
#
# Y hace falta wine32:i386 aunque el prefijo sea WINEARCH=win64: el instalador de
# Inno Setup 6 es un ejecutable de 32 bits, así que sin la capa WoW64 wine muere con
# "failed to load C:\windows\syswow64\ntdll.dll". Son unos 650 MB extra; la
# alternativa sería saltar a Inno Setup 7, cuyo instalador sí es x64 nativo, pero eso
# cambia la versión con la que se genera el instalador que se publica.
#
# xvfb porque el instalador de Inno necesita un display aunque se ejecute en silencio.
#
# Va al final a propósito: así no invalida las capas caras de Qt y del sysroot de
# FreeBSD, que tardan mucho en reconstruirse.
# `zip` va aquí porque el empaquetado del .app de macOS lo usa: sin él el cruce compila
# los dos bundles enteros y muere en la última línea con «zip: command not found».
RUN dpkg --add-architecture i386 \
    && apt-get update && apt-get install -y --no-install-recommends \
        wine wine32:i386 winbind xvfb zip \
    && rm -rf /var/lib/apt/lists/*

# Legible y ejecutable por cualquier UID: el contenedor no corre como root.
RUN chmod -R a+rX /opt/toolchain

# Variables que los doctores de build-cross.sh esperan encontrar, para no tener que
# exportarlas a mano dentro del contenedor.
# NO se fija QT_HOST_PATH: cada objetivo necesita un Qt host de su misma versión
# (Windows usa 6.8.3, y el Qt de FreeBSD del sysroot es 6.11.1, que el propio
# aprovisionamiento instala aparte). build-cross.sh ya resuelve el adecuado por
# objetivo buscando en ~/Qt, y docker/build.sh enlaza ~/Qt a esta toolchain.
ENV PATH=/opt/toolchain/.local/bin:${PATH} \
    QT_ROOT=/opt/toolchain/Qt \
    QT6_WINDOWS_PREFIX=/opt/toolchain/Qt/${QT_VERSION}/mingw_64 \
    QT6_MACOS_PREFIX=/opt/toolchain/Qt/${QT_VERSION}/macos \
    OPENSSL_ROOT_DIR=/opt/toolchain/opt/openssl-mingw64 \
    FREEBSD_SYSROOT=/opt/toolchain/sysroots/freebsd14.3-amd64 \
    QT6_FREEBSD_PREFIX=/opt/toolchain/sysroots/freebsd14.3-amd64/usr/local

# El código se monta en tiempo de ejecución; la imagen solo aporta la toolchain.
ENTRYPOINT ["/bin/bash"]
