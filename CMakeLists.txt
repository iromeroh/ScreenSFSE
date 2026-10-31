cmake_minimum_required(VERSION 3.21)

project(ScreenSFSEPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMMONLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/commonlibsf" CACHE PATH "CommonLibSF source directory")
if(NOT EXISTS "${COMMONLIB_ROOT}/include/RE/Starfield.h")
    message(FATAL_ERROR "CommonLibSF is missing. Follow the dependency instructions in BUILDING.md.")
endif()

set(COMMONLIB_SHARED_SOURCES
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REL/IDDB.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REL/Offset2ID.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REL/Utility.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REL/Version.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REX/FModule.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REX/REX.cpp
    ${COMMONLIB_ROOT}/lib/commonlib-shared/src/REX/W32.cpp
)

set(COMMONLIB_RE_SOURCES
    ${COMMONLIB_ROOT}/src/RE/A/Array.cpp
    ${COMMONLIB_ROOT}/src/RE/B/BSLock.cpp
    ${COMMONLIB_ROOT}/src/RE/O/Object.cpp
    ${COMMONLIB_ROOT}/src/RE/T/TypeInfo.cpp
    ${COMMONLIB_ROOT}/src/RE/V/Variable.cpp
)

set(PLUGIN_SOURCES
    src/commonlib_rex_log.cpp
    src/logger.cpp
    src/papyrus_bridge.cpp
    src/plugin.cpp
    src/screen_manager.cpp
    src/test_pattern.cpp
    src/osd.cpp
    src/video_surface.cpp
    src/video_fetcher.cpp
    src/media_player.cpp
    src/live_player.cpp
    src/screen_controller.cpp
    src/screen_location.cpp
    src/save_sync.cpp
    src/subtitles.cpp
)

# d3d12_vtbl.cpp defines CINTERFACE before any Windows header, so it is the one source compiled without the
# forced SFSE PCH include (see the per-source /FI below).
add_library(ScreenSFSEPlugin SHARED
    ${PLUGIN_SOURCES}
    src/d3d12_vtbl.cpp
    ${COMMONLIB_SHARED_SOURCES}
    ${COMMONLIB_RE_SOURCES}
)

target_include_directories(ScreenSFSEPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/sfse"
    "${COMMONLIB_ROOT}/include"
    "${COMMONLIB_ROOT}/lib/commonlib-shared/include"
)

target_compile_definitions(ScreenSFSEPlugin PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(ScreenSFSEPlugin PRIVATE
    advapi32
    bcrypt
    d3d11
    d3d12
    d3dcompiler
    dbghelp
    dxgi
    gdi32
    mf
    mfplat
    mfplay
    mfreadwrite
    mfuuid
    ole32
    shell32
    strmiids
    version
    wininet
    winmm
    ws2_32
    xaudio2
)

if(MSVC)
    target_compile_options(ScreenSFSEPlugin PRIVATE
        /bigobj
        /permissive-
    )
    set_source_files_properties(${PLUGIN_SOURCES} ${COMMONLIB_SHARED_SOURCES} ${COMMONLIB_RE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/FISFSE/Impl/PCH.h")
    target_link_options(ScreenSFSEPlugin PRIVATE
        "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/exports.def"
    )
endif()

set_target_properties(ScreenSFSEPlugin PROPERTIES
    OUTPUT_NAME "ScreenSFSE"
)
