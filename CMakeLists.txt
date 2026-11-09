cmake_minimum_required(VERSION 3.20)
project(storm_connections_netcode C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook source files
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/minhook")
set(MINHOOK_SOURCES
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MINHOOK_DIR}/include
)

add_library(d3dcompiler_47 SHARED
    src/dllmain.cpp
    src/proxy_d3dcompiler.cpp
    src/netcode_hooks.cpp
    d3dcompiler_47.def
    ${MINHOOK_SOURCES}
)

target_link_libraries(d3dcompiler_47 PRIVATE
    ws2_32
)

set_target_properties(d3dcompiler_47 PROPERTIES
    OUTPUT_NAME "d3dcompiler_47"
    PREFIX ""
)
