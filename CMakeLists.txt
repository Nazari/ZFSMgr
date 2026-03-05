cmake_minimum_required(VERSION 3.21)
project(ZFSMgrQt VERSION 0.1.0 LANGUAGES CXX)

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
    src/mainwindow_settings.cpp
    src/mainwindow_logging.cpp
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

target_compile_definitions(zfsmgr_qt PRIVATE QT_NO_KEYWORDS)

if(WIN32)
    target_sources(zfsmgr_qt PRIVATE appicon.rc)
    set_target_properties(zfsmgr_qt PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    set(app_icns "${CMAKE_CURRENT_SOURCE_DIR}/icons/ZFSMgr.icns")
    set_source_files_properties(${app_icns} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(zfsmgr_qt PRIVATE ${app_icns})
    set_target_properties(zfsmgr_qt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "ZFSMgr.icns"
    )
endif()

install(TARGETS zfsmgr_qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
