cmake_minimum_required(VERSION 3.20)
project(zfsmgr_daemon LANGUAGES CXX)

find_package(Qt6 6.5 COMPONENTS Core REQUIRED)
find_package(libssh REQUIRED)

set(DAEMON_SOURCES
    libssh_daemon.cpp
)

qt_add_executable(zfsmgr_daemon ${DAEMON_SOURCES})
target_link_libraries(zfsmgr_daemon PRIVATE Qt6::Core ssh)
target_compile_features(zfsmgr_daemon PRIVATE cxx_std_17)
