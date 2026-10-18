cmake_minimum_required(VERSION 3.21)
project(joc_mpv CXX)

# joc_mpv is an mpv C plugin (cplugins feature) that plays E-AC-3 JOC with the
# JustOneCacophony renderer.  The renderer itself is joc_core.dll, which this
# project can build from a JustOneCacophonyCXX checkout so the kernel stays the
# byte-identical artifact both frontends share.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(JOC_CORE_SOURCE "" CACHE PATH "JustOneCacophonyCXX checkout to build joc_core.dll from")

add_library(joc_mpv SHARED
    src/cplugin_main.cpp
    src/joc_decode.cpp
    src/container_scan.cpp
    src/eac3_scan.cpp
    src/log.cpp
    src/plugin/audio_source.cpp
    src/plugin/config.cpp
    src/plugin/http_server.cpp
    src/plugin/wav.cpp
)
target_include_directories(joc_mpv PRIVATE
    src
    third_party
    third_party/joc/include
)
target_compile_definitions(joc_mpv PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)
target_compile_options(joc_mpv PRIVATE /W4 /permissive- /utf-8 /EHsc /fp:precise)
target_link_libraries(joc_mpv PRIVATE ws2_32)
set_target_properties(joc_mpv PROPERTIES PREFIX "" OUTPUT_NAME "joc_mpv")

if(JOC_CORE_SOURCE)
    set(JOC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JOC_BUILD_DEVTOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${JOC_CORE_SOURCE}" "${CMAKE_BINARY_DIR}/joc_core")
    add_custom_command(TARGET joc_mpv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:joc_core> $<TARGET_FILE_DIR:joc_mpv>/joc_core.dll
        COMMENT "Copying joc_core.dll next to joc_mpv.dll")
endif()
