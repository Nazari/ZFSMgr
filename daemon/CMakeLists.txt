cmake_minimum_required(VERSION 3.20)
project(zfsmgr_daemon LANGUAGES CXX)

set(DAEMON_SOURCES
    main.cpp
)

add_executable(zfsmgr_daemon ${DAEMON_SOURCES})
target_compile_features(zfsmgr_daemon PRIVATE cxx_std_17)
targeT_include_directories(zfsmgr_daemon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
