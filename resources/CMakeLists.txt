cmake_minimum_required(VERSION 3.21)
project(ZFSMgrQt VERSION 0.92.0 LANGUAGES CXX)
include(CTest)
set(ZFSMGR_APP_VERSION_STRING "0.92.0")
set(ZFSMGR_LINUX_DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/zfsmgr.desktop")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../src/mainwindow_connections.cpp" ZFSMGR_GSA_SOURCE_SHA)
string(SUBSTRING "${ZFSMGR_GSA_SOURCE_SHA}" 0 9 ZFSMGR_GSA_VERSION_SUFFIX)
string(REGEX REPLACE "[aA]" "1" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
string(REGEX REPLACE "[bB]" "2" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
string(REGEX REPLACE "[cC]" "3" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
string(REGEX REPLACE "[dD]" "4" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
string(REGEX REPLACE "[eE]" "5" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
string(REGEX REPLACE "[fF]" "6" ZFSMGR_GSA_VERSION_SUFFIX "${ZFSMGR_GSA_VERSION_SUFFIX}")
set(ZFSMGR_AGENT_SCHEMA_MARKER [=[linux-service:/etc/systemd/system/zfsmgr-agent.service
linux-bin:/usr/local/libexec/zfsmgr-agent
linux-config:/etc/zfsmgr/agent.toml
linux-state:/var/lib/zfsmgr-agent/state.db
mac-plist:/Library/LaunchDaemons/org.zfsmgr.agent.plist
freebsd-rc:/usr/local/etc/rc.d/zfsmgr_agent
win-service:ZFSMgrAgent
tls:mtls-required
api:v3
cmd:--dump-zfs-exists
cmd:--mutate-zfs-clone
cli-only:--mutate-shell-generic
cli-only:--mutate-advanced-fromdir
cli-only:--mutate-sync-temp-tar-source
cli-only:--mutate-sync-temp-tar-dest
cmd:--repair-alt-mountpoints
cmd:--job-submit
cmd:--mutate-zfs-allow-batch
cmd:--zfs-pipe-local
cmd:--mutate-rsync-local
cmd:--wait-for-event
cmd:--dump-gsa-log
cmd:--dump-tool-availability
health:caps-declared
win:native-only
zed:event-watcher-class-filter
fallback:no-script-helpers
gui:daemon-mandatory
daemon:log-and-heartbeat
daemon:log-tail-with-absolute-offset
gsa:scheduler
zed:lightweight-poll
breakdown:dir-name-pairs
todir:progress-and-per-dataset
cmd:--mutate-zfs-create
create:reports-mountpoint
cmd:--mutate-copy-tree
cmd:--dump-copy-tree-pending
copytree:skip-unchanged-delete-dryrun
]=])
string(SHA256 ZFSMGR_AGENT_SCHEMA_SHA "${ZFSMGR_AGENT_SCHEMA_MARKER}")
string(SUBSTRING "${ZFSMGR_AGENT_SCHEMA_SHA}" 0 8 ZFSMGR_AGENT_SCHEMA_HEX)
math(EXPR ZFSMGR_AGENT_SCHEMA_RAW "0x${ZFSMGR_AGENT_SCHEMA_HEX}")
math(EXPR ZFSMGR_AGENT_VERSION_SUFFIX "${ZFSMGR_AGENT_SCHEMA_RAW} % 1000000000")
set(ZFSMGR_AGENT_VERSION_STRING "${ZFSMGR_APP_VERSION_STRING}.${ZFSMGR_AGENT_VERSION_SUFFIX}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# El agente no usa Qt, pero este CMakeLists es común con la GUI y su find_package(Qt6)
# obligaba a instalar Qt para compilar solo el daemon. Eso hacía inviable cruzarlo con
# MinGW en integración continua, que es donde hace falta: el agente de Windows se
# despliega solo en la máquina remota y tiene que ir enlazado estáticamente.
option(ZFSMGR_AGENT_ONLY "Compilar únicamente el agente, sin Qt ni GUI" OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(ZFSMGR_AGENT_ONLY)
    add_executable(zfsmgr_agent ../src/daemon_main.cpp ../src/copytree.cpp)
    target_compile_definitions(zfsmgr_agent PRIVATE
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
        ZFSMGR_AGENT_VERSION_STRING="${ZFSMGR_AGENT_VERSION_STRING}"
    )
    target_link_libraries(zfsmgr_agent PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    if(WIN32)
        target_link_libraries(zfsmgr_agent PRIVATE ws2_32 crypt32 iphlpapi ncrypt bcrypt advapi32)
        # Igual que en la compilación completa: el agente viaja SOLO, así que no puede
        # depender de libgcc, libstdc++ ni las DLL de OpenSSL. Sin esto arranca con
        # 0xC0000135 en cualquier Windows sin esas bibliotecas.
        if(MINGW)
            target_link_options(zfsmgr_agent PRIVATE -static-libgcc -static-libstdc++ -static)
        endif()
    endif()
    return()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Core Gui Concurrent Network Test)

set(ZFSMGR_CORE_SOURCES
    ../src/mainwindow.cpp
    ../src/mainwindow_helpers.cpp
    ../src/helperinstallcatalog.cpp
    ../src/helperinstallcatalog.h
    ../src/mainwindow_ui_logic.cpp
    ../src/connectioncapabilities.cpp
    ../src/mainwindow_ui.cpp
    ../src/mainwindow_dialogs.cpp
    ../src/mainwindow_connections.cpp
    ../src/mainwindow_pools.cpp
    ../src/mainwindow_pool_create.cpp
    ../src/mainwindow_selection.cpp
    ../src/mainwindow_dataset_tree.cpp
    ../src/mainwindow_permissions.cpp
    ../src/mainwindow_filebrowser.cpp
    ../src/mainwindow_dataset_props.cpp
    ../src/mainwindow_pending_store.cpp
    ../src/mainwindow_dataset_actions.cpp
    ../src/mainwindow_dataset_manage.cpp
    ../src/mainwindow_transfer.cpp
    ../src/mainwindow_state_ui.cpp
    ../src/mainwindow_advanced.cpp
    ../src/mainwindow_advanced_dir.cpp
    ../src/mainwindow_refresh.cpp
    ../src/mainwindow_runtime_ui.cpp
    ../src/mainwindow_remote.cpp
    ../src/mainwindow_table_utils.cpp
    ../src/mainwindow_settings.cpp
    ../src/mainwindow_logging.cpp
    ../src/mainwindow_help.cpp
    ../src/mainwindow.h
    ../src/connectiondatasettreepane.cpp
    ../src/connectiondatasettreepane.h
    ../src/connectiondatasettreecontroller.cpp
    ../src/connectiondatasettreecontroller.h
    ../src/connectiondatasettreecoordinator.cpp
    ../src/connectiondatasettreecoordinator.h
    ../src/connectiondatasettreewidget.cpp
    ../src/connectiondatasettreewidget.h
    ../src/mainwindow_connectiondatasettreedelegate.cpp
    ../src/mainwindow_connectiondatasettreedelegate.h
    ../src/connectionstore.cpp
    ../src/connectionstore.h
    ../src/connectiondialog.cpp
    ../src/connectiondialog.h
    ../src/masterpassworddialog.cpp
    ../src/masterpassworddialog.h
    ../src/i18nmanager.cpp
    ../src/i18nmanager.h
    ../src/secretcipher.cpp
    ../src/secretcipher.h
    ../src/agentversion.cpp
    ../src/agentversion.h
    ../src/daemonpayload.cpp
    ../src/daemonpayload.h
    resources.qrc
)

# Capa base: lógica del cliente SIN Qt.
#
# La regla que le da sentido es la de enlazado: este objetivo NO enlaza contra Qt, así
# que si alguien mete aquí un QString el build falla en el sitio, no meses después
# cuando ya haya media biblioteca contaminada. Esa es toda la garantía; el resto son
# buenas intenciones.
#
# Lo usan tanto la GUI como el agente. Ver docs/diseno_tecnico_capa_base_sin_qt.md.
add_library(zfsmgr_base STATIC
    ../src/base/strutil.cpp
    ../src/base/strutil.h
    ../src/base/daemonpayload.cpp
    ../src/base/secretcipher.cpp
    ../src/base/secretcipher.h
    ../src/base/json.cpp
    ../src/base/json.h
    ../src/base/helpers.cpp
    ../src/base/helpers.h
    ../src/base/daemonpayload.h
)
target_include_directories(zfsmgr_base PUBLIC ../src/base)
# OpenSSL sí, Qt no. La regla es la ausencia de Qt, no la de dependencias: el cifrado de
# la configuración usa EVP/HMAC/RAND y el agente ya enlaza lo mismo.
target_link_libraries(zfsmgr_base PUBLIC OpenSSL::Crypto)
set_target_properties(zfsmgr_base PROPERTIES POSITION_INDEPENDENT_CODE ON)

qt_add_library(zfsmgr_core STATIC ${ZFSMGR_CORE_SOURCES})

# OpenSSL::SSL además de Crypto aunque el código no la use directamente: hace que
# libssl quede enlazada y, por tanto, cargada al arrancar y copiada al bundle por el
# despliegue de macOS. Sin ella, el plugin TLS de OpenSSL de Qt no puede cargarse y
# en macOS solo queda SecureTransport, que pide la clave del cliente en el llavero e
# ignora la CA que se le pasa.
target_link_libraries(zfsmgr_core PUBLIC Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Network OpenSSL::SSL OpenSSL::Crypto zfsmgr_base)

if(WIN32)
    # OpenSSL static builds on MinGW may require explicit Win32 system libs.
    target_link_libraries(zfsmgr_core PUBLIC ws2_32 crypt32 iphlpapi ncrypt bcrypt)
endif()

target_include_directories(zfsmgr_core PUBLIC ../src)

target_compile_definitions(zfsmgr_core PUBLIC
    QT_NO_KEYWORDS
    ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    ZFSMGR_GSA_VERSION_SUFFIX="${ZFSMGR_GSA_VERSION_SUFFIX}"
    ZFSMGR_AGENT_VERSION_STRING="${ZFSMGR_AGENT_VERSION_STRING}"
)

qt_add_executable(zfsmgr_qt
    ../src/main.cpp
)

target_link_libraries(zfsmgr_qt PRIVATE zfsmgr_core)

add_executable(zfsmgr_agent
    ../src/daemon_main.cpp
    ../src/copytree.cpp
)
target_compile_definitions(zfsmgr_agent PRIVATE
    ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    ZFSMGR_AGENT_VERSION_STRING="${ZFSMGR_AGENT_VERSION_STRING}"
)
target_link_libraries(zfsmgr_agent PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(zfsmgr_agent PRIVATE ws2_32 crypt32 iphlpapi ncrypt bcrypt advapi32)
    # Enlazado estático del runtime en Windows: sin esto el .exe necesita
    # libgcc_s_seh-1.dll, libstdc++-6.dll y libwinpthread-1.dll a su lado, y al
    # desplegarlo suelto en un Windows real muere con 0xC0000135 (DLL no encontrada)
    # sin decir cuál. El daemon se despliega SOLO, así que tiene que bastarse.
    if(MINGW)
        target_link_options(zfsmgr_agent PRIVATE -static-libgcc -static-libstdc++ -static)
    endif()
endif()
if(NOT WIN32)
    # Optional libzfs_core + libnvpair for efficient native ZFS operations.
    # Subprocess fallback is used automatically when not available.
    # Override with -DLIBZFS_CORE_INCLUDE_DIR=<path> if headers are non-standard.
    find_library(LIBZFS_CORE_LIB NAMES zfs_core libzfs_core.so.3 libzfs_core.so)
    find_library(LIBNVPAIR_LIB   NAMES nvpair   libnvpair.so.3   libnvpair.so)
    if(NOT DEFINED LIBZFS_CORE_INCLUDE_DIR)
        find_path(LIBZFS_CORE_INCLUDE_DIR libzfs_core.h
            PATHS
                /usr/include
                /usr/local/include
                /usr/include/libzfs
                /usr/local/include/libzfs
                /opt/homebrew/include
                /opt/homebrew/include/libzfs
        )
    endif()
    # libnvpair.h must be reachable from the same include dir as libzfs_core.h.
    # Check with file(EXISTS) so we stay strictly within the resolved sysroot
    # path and don't accidentally pick up host-system headers during cross builds.
    # libzfs_core.h incluye <sys/fs/zfs.h>, que no siempre acompaña a la cabecera: el
    # sysroot base de FreeBSD trae libzfs_core.h y libnvpair.h pero no sys/fs/zfs.h, y
    # entonces la detección daba por buena una cabecera que no compila. Comprobarla
    # aquí hace que en ese caso se use el fallback por subproceso, que es el
    # comportamiento previsto, en lugar de romper el build.
    if(LIBZFS_CORE_INCLUDE_DIR
       AND EXISTS "${LIBZFS_CORE_INCLUDE_DIR}/libnvpair.h"
       AND EXISTS "${LIBZFS_CORE_INCLUDE_DIR}/sys/fs/zfs.h")
        set(_LIBNVPAIR_HEADER_OK TRUE)
    else()
        set(_LIBNVPAIR_HEADER_OK FALSE)
    endif()
    if(LIBZFS_CORE_LIB AND LIBNVPAIR_LIB AND LIBZFS_CORE_INCLUDE_DIR AND _LIBNVPAIR_HEADER_OK)
        message(STATUS "zfsmgr_agent: libzfs_core=${LIBZFS_CORE_LIB} headers=${LIBZFS_CORE_INCLUDE_DIR}")
        target_compile_definitions(zfsmgr_agent PRIVATE HAVE_LIBZFS_CORE=1)
        target_include_directories(zfsmgr_agent PRIVATE ${LIBZFS_CORE_INCLUDE_DIR})
        target_link_libraries(zfsmgr_agent PRIVATE ${LIBZFS_CORE_LIB} ${LIBNVPAIR_LIB})
    else()
        message(STATUS "zfsmgr_agent: libzfs_core not found — subprocess fallback active")
    endif()
endif()

if(WIN32)
    target_sources(zfsmgr_qt PRIVATE appicon.rc)
    set_target_properties(zfsmgr_qt PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        # appicon.rc already embeds app.manifest; disable linker auto-manifest
        # to avoid CVT1100 duplicate MANIFEST in GitHub Actions (MSVC).
        target_link_options(zfsmgr_qt PRIVATE "/MANIFEST:NO")
    endif()
endif()

if(APPLE)
    # SIN versión en el nombre: el bundle debe llamarse siempre ZFSMgr.app para que
    # al copiarlo a /Applications reemplace al anterior en vez de instalarse a su
    # lado. La versión sigue estando en el Info.plist (CFBundleShortVersionString)
    # y en el nombre del .zip que se publica.
    set(ZFSMGR_BUNDLE_NAME "ZFSMgr")
    set(app_icns "${CMAKE_CURRENT_SOURCE_DIR}/../icons/ZFSMgr.icns")
    set_source_files_properties(${app_icns} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(zfsmgr_qt PRIVATE ${app_icns})
    set_target_properties(zfsmgr_qt PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${ZFSMGR_BUNDLE_NAME}"
        MACOSX_BUNDLE_BUNDLE_NAME "${ZFSMGR_BUNDLE_NAME}"
        MACOSX_BUNDLE_ICON_FILE "ZFSMgr.icns"
        # Sin esto, CMake genera com.yourcompany.zfsmgr_qt, su marcador de posición
        # por defecto, que es lo que aparecía en los informes de fallo del sistema.
        # Se usa el prefijo org.zfsmgr, que ya emplea el daemon en su plist de
        # launchd (org.zfsmgr.agent), en vez de introducir otro distinto.
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.zfsmgr.ZFSMgr"
        # CMake pone por defecto solo mayor.menor en la versión corta, así que la
        # 0.90.2 y la 0.90.3 se veían ambas como "0.90" en Finder y en los informes
        # de fallo. Ambas cadenas llevan la versión completa.
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${ZFSMGR_APP_VERSION_STRING}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ZFSMGR_APP_VERSION_STRING}"
    )
endif()

install(TARGETS zfsmgr_qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

install(TARGETS zfsmgr_agent
    RUNTIME DESTINATION bin
)

if(DEFINED ZFSMGR_AGENT_BUNDLE_DIR AND EXISTS "${ZFSMGR_AGENT_BUNDLE_DIR}")
    install(DIRECTORY "${ZFSMGR_AGENT_BUNDLE_DIR}/"
        DESTINATION share/zfsmgr/agents
    )
endif()

if(UNIX AND NOT APPLE)
    file(WRITE "${ZFSMGR_LINUX_DESKTOP_FILE}" [=[
[Desktop Entry]
Type=Application
Name=ZFSMgr
Comment=Cross-platform OpenZFS GUI manager
Exec=zfsmgr_qt
Icon=ZFSMgr
Categories=System;Utility;
Terminal=false
]=])

    install(FILES "${ZFSMGR_LINUX_DESKTOP_FILE}"
        DESTINATION share/applications
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../icons/ZFSMgr-512.png"
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME ZFSMgr.png
    )

    set(CPACK_PACKAGE_NAME "zfsmgr")
    set(CPACK_PACKAGE_VENDOR "Nazari")
    set(CPACK_PACKAGE_CONTACT "Nazari")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cross-platform OpenZFS GUI manager")
    set(CPACK_PACKAGE_VERSION "${ZFSMGR_APP_VERSION_STRING}")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Nazari")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    if(DEFINED ZFSMGR_AGENT_BUNDLE_DIR AND EXISTS "${ZFSMGR_AGENT_BUNDLE_DIR}")
        # Cuando incluimos agentes multi-OS, dpkg-shlibdeps intenta resolver ELFs
        # de FreeBSD dentro del .deb y falla (no existen esas libs en Linux).
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    else()
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    endif()
    include(CPack)
endif()

if(BUILD_TESTING)
    add_executable(zfsmgr_helpers_test
        ../tests/helpers_test.cpp
        ../src/mainwindow_helpers.cpp
        # agentCommand() toma la ruta del binario de daemonpayload en vez de repetirla:
        # quien construye la invocación y quien la reconoce para convertirla en RPC deben
        # hablar de la misma ruta, o el comando se va por SSH en crudo sin avisar.
        ../src/daemonpayload.cpp
    )
    target_include_directories(zfsmgr_helpers_test PRIVATE ../src)
    target_link_libraries(zfsmgr_helpers_test PRIVATE Qt6::Core Qt6::Widgets zfsmgr_base)
    target_compile_definitions(zfsmgr_helpers_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
    add_test(NAME zfsmgr_helpers_test COMMAND zfsmgr_helpers_test)

    # SIN Qt a propósito: ni Qt6::Core. Es la comprobación de que la capa base está
    # realmente limpia; si alguien mete un QString ahí, esto deja de enlazar y el fallo
    # sale en el commit que lo introduce, no meses después.
    add_executable(zfsmgr_base_test ../tests/base_test.cpp)
    target_link_libraries(zfsmgr_base_test PRIVATE zfsmgr_base OpenSSL::Crypto)
    add_test(NAME zfsmgr_base_test COMMAND zfsmgr_base_test)

    # La copia de árboles se prueba aparte y sin Qt: es donde caen los casos límite
    # —enlaces duros, dispersos, exclusiones— y donde un fallo BORRA datos, porque de
    # ella cuelgan Desglosar, Ensamblar y Hacia Dir.
    add_executable(zfsmgr_copytree_test
        ../tests/copytree_test.cpp
        ../src/copytree.cpp
    )
    target_include_directories(zfsmgr_copytree_test PRIVATE ../src)
    target_link_libraries(zfsmgr_copytree_test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(zfsmgr_copytree_test PRIVATE QT_NO_KEYWORDS)
    add_test(NAME zfsmgr_copytree_test COMMAND zfsmgr_copytree_test)

    qt_add_executable(zfsmgr_capabilities_test
        ../tests/capabilities_test.cpp
        ../src/connectioncapabilities.cpp
    )
    target_include_directories(zfsmgr_capabilities_test PRIVATE ../src)
    target_link_libraries(zfsmgr_capabilities_test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(zfsmgr_capabilities_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
    add_test(NAME zfsmgr_capabilities_test COMMAND zfsmgr_capabilities_test)

    qt_add_executable(zfsmgr_ui_logic_test
        ../tests/ui_logic_test.cpp
        ../src/mainwindow_ui_logic.cpp
    )
    target_include_directories(zfsmgr_ui_logic_test PRIVATE ../src)
    target_link_libraries(zfsmgr_ui_logic_test PRIVATE Qt6::Core Qt6::Test)
    target_compile_definitions(zfsmgr_ui_logic_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
    add_test(NAME zfsmgr_ui_logic_test COMMAND zfsmgr_ui_logic_test)

    qt_add_executable(zfsmgr_gui_mainwindow_test
        ../tests/gui_mainwindow_test.cpp
    )
    target_include_directories(zfsmgr_gui_mainwindow_test PRIVATE ../src)
    target_link_libraries(zfsmgr_gui_mainwindow_test PRIVATE zfsmgr_core Qt6::Test)
    target_compile_definitions(zfsmgr_gui_mainwindow_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
    add_test(NAME zfsmgr_gui_mainwindow_test COMMAND zfsmgr_gui_mainwindow_test)
    set_tests_properties(zfsmgr_gui_mainwindow_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ZFSMGR_TEST_MODE=1")

    qt_add_executable(zfsmgr_ui_doc_capture
        ../tests/ui_doc_capture.cpp
    )
    target_include_directories(zfsmgr_ui_doc_capture PRIVATE ../src)
    target_link_libraries(zfsmgr_ui_doc_capture PRIVATE zfsmgr_core)
    target_compile_definitions(zfsmgr_ui_doc_capture PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
endif()
