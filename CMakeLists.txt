cmake_minimum_required(VERSION 3.20)
project(storm_connections_netcode C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook static library (third-party C dependency)
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/minhook")
add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC
    ${MINHOOK_DIR}/include
)

add_library(d3dcompiler_47 SHARED
    src/dllmain.cpp
    src/proxy_d3dcompiler.cpp
    src/crash_fixes.cpp
    src/lobby_wait_fix.cpp
    src/system_tuning.cpp
    src/rtt_negotiation.cpp
    src/packet_redundancy.cpp
    src/netcode_hooks.cpp
    d3dcompiler_47.def
)

target_include_directories(d3dcompiler_47 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(d3dcompiler_47 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /Zc:__cplusplus>
)

target_link_libraries(d3dcompiler_47 PRIVATE
    minhook
    ws2_32
    user32
)

set_target_properties(d3dcompiler_47 PROPERTIES
    OUTPUT_NAME "d3dcompiler_47"
    PREFIX ""
)
