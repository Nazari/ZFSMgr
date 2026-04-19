cmake_minimum_required(VERSION 3.21)
project(ZFSMgrQt VERSION 0.64.0 LANGUAGES CXX)
include(CTest)
set(ZFSMGR_APP_VERSION_STRING "0.64.0")
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
cmd:--mutate-shell-generic
cmd:--wait-for-event
cmd:--dump-gsa-log
zed:event-watcher
gsa:scheduler
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

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Core Gui Concurrent Network Test)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(ZFSMGR_CORE_SOURCES
    ../src/mainwindow.cpp
    ../src/mainwindow_helpers.cpp
    ../src/helperinstallcatalog.cpp
    ../src/helperinstallcatalog.h
    ../src/mainwindow_ui_logic.cpp
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

qt_add_library(zfsmgr_core STATIC ${ZFSMGR_CORE_SOURCES})

target_link_libraries(zfsmgr_core PUBLIC Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Network OpenSSL::Crypto)

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
)
target_compile_definitions(zfsmgr_agent PRIVATE
    ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    ZFSMGR_AGENT_VERSION_STRING="${ZFSMGR_AGENT_VERSION_STRING}"
)
if(NOT WIN32)
    target_link_libraries(zfsmgr_agent PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(zfsmgr_agent PRIVATE Threads::Threads)

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
    if(LIBZFS_CORE_INCLUDE_DIR AND EXISTS "${LIBZFS_CORE_INCLUDE_DIR}/libnvpair.h")
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
    set(ZFSMGR_BUNDLE_NAME "ZFSMgr-${ZFSMGR_APP_VERSION_STRING}")
    set(app_icns "${CMAKE_CURRENT_SOURCE_DIR}/../icons/ZFSMgr.icns")
    set_source_files_properties(${app_icns} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(zfsmgr_qt PRIVATE ${app_icns})
    set_target_properties(zfsmgr_qt PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${ZFSMGR_BUNDLE_NAME}"
        MACOSX_BUNDLE_BUNDLE_NAME "${ZFSMGR_BUNDLE_NAME}"
        MACOSX_BUNDLE_ICON_FILE "ZFSMgr.icns"
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
    )
    target_include_directories(zfsmgr_helpers_test PRIVATE ../src)
    target_link_libraries(zfsmgr_helpers_test PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_definitions(zfsmgr_helpers_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${ZFSMGR_APP_VERSION_STRING}"
    )
    add_test(NAME zfsmgr_helpers_test COMMAND zfsmgr_helpers_test)

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
