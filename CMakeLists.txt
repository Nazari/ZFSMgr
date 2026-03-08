cmake_minimum_required(VERSION 3.21)
project(ZFSMgrQt VERSION 0.9.1 LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Core Gui Concurrent)
find_package(OpenSSL REQUIRED)

qt_add_executable(zfsmgr_qt
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow_helpers.cpp
    src/mainwindow_ui.cpp
    src/mainwindow_dialogs.cpp
    src/mainwindow_connections.cpp
    src/mainwindow_pools.cpp
    src/mainwindow_pool_create.cpp
    src/mainwindow_selection.cpp
    src/mainwindow_dataset_tree.cpp
    src/mainwindow_dataset_props.cpp
    src/mainwindow_dataset_actions.cpp
    src/mainwindow_dataset_manage.cpp
    src/mainwindow_transfer.cpp
    src/mainwindow_state_ui.cpp
    src/mainwindow_advanced.cpp
    src/mainwindow_advanced_dir.cpp
    src/mainwindow_refresh.cpp
    src/mainwindow_runtime_ui.cpp
    src/mainwindow_remote.cpp
    src/mainwindow_table_utils.cpp
    src/mainwindow_settings.cpp
    src/mainwindow_logging.cpp
    src/mainwindow_help.cpp
    src/mainwindow.h
    src/connectionstore.cpp
    src/connectionstore.h
    src/connectiondialog.cpp
    src/connectiondialog.h
    src/masterpassworddialog.cpp
    src/masterpassworddialog.h
    src/i18nmanager.cpp
    src/i18nmanager.h
    src/secretcipher.cpp
    src/secretcipher.h
    resources.qrc
)

target_link_libraries(zfsmgr_qt PRIVATE Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Concurrent OpenSSL::Crypto)

target_compile_definitions(zfsmgr_qt PRIVATE
    QT_NO_KEYWORDS
    ZFSMGR_APP_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_sources(zfsmgr_qt PRIVATE appicon.rc)
    set_target_properties(zfsmgr_qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    set(ZFSMGR_BUNDLE_NAME "ZFSMgr-${PROJECT_VERSION}")
    set(app_icns "${CMAKE_CURRENT_SOURCE_DIR}/icons/ZFSMgr.icns")
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

if(BUILD_TESTING)
    add_executable(zfsmgr_helpers_test
        tests/helpers_test.cpp
        src/mainwindow_helpers.cpp
    )
    target_include_directories(zfsmgr_helpers_test PRIVATE src)
    target_link_libraries(zfsmgr_helpers_test PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_definitions(zfsmgr_helpers_test PRIVATE
        QT_NO_KEYWORDS
        ZFSMGR_APP_VERSION="${PROJECT_VERSION}"
    )
    add_test(NAME zfsmgr_helpers_test COMMAND zfsmgr_helpers_test)
endif()
