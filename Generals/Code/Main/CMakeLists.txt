# GeneralsX @feature android-port 08/02/2026
# Android apps are shared libraries, not executables. Zero Hour already builds
# this way; Generals now does too, so both engines can ship in one APK and the
# launcher can choose between them at load time.
if(ANDROID)
    add_library(g_generals SHARED)
else()
    add_executable(g_generals WIN32)
endif()

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if(ANDROID)
    # Zero Hour owns "libmain.so", because SDL's nativeRunMain defaults to it.
    # Generals ships alongside as a second engine; GameActivity picks which one
    # to load via getMainSharedObject().
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "main_generals")
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "generalsv${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME GeneralsX)
endif()

# GeneralsX @build BenderAI 10/03/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(g_generals PRIVATE
        binkstub
        comctl32
        d3d8
        dinput8
        dxguid
        imm32
        milesstub
        vfw32
        winmm
    )
endif()

# Cross-platform core libraries
target_link_libraries(g_generals PRIVATE
    d3dx8
    g_gameengine
    g_gameenginedevice
    gi_always
)

# GeneralsX @feature android-port 08/02/2026 Android system libraries, matching
# Zero Hour: liblog for __android_log_print, libandroid for the AAssetManager
# APIs the shared bootstrap uses to extract fonts out of the APK.
if(ANDROID)
    target_link_libraries(g_generals PRIVATE log android)
endif()

# GeneralsX @bugfix android-port 08/02/2026 Hide the engine's global operator
# new/delete from other shared libraries, exactly as Zero Hour does. Without
# this, DXVK allocates through the engine's TheDynamicMemoryAllocator pool,
# which zeroes every allocation and garbage-fills on free -- that corrupts
# DXVK's free-lists and crashes in DxvkResourceAllocationPool::alloc().
if(ANDROID)
    set(_gx_alloc_map_g "${CMAKE_CURRENT_BINARY_DIR}/gx_hide_alloc_generals.map")
    file(WRITE "${_gx_alloc_map_g}" [=[
{
  global:
    *;
  local:
    _Znw*;
    _Zna*;
    _Zdl*;
    _Zda*;
};
]=])
    target_link_options(g_generals PRIVATE
        "-Wl,--version-script,${_gx_alloc_map_g}"
    )
endif()

# Linux SDL3 windowing/input
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(g_generals PRIVATE sdl3lib)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
"
)

# Based on original binary values for these variables.
if (IS_VS6_BUILD)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 7
#define VERSION_BUILDNUM 601
"
)
else()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 8
#define VERSION_BUILDNUM 601
"
)
endif()

if(MSVC)
    target_link_options(g_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(g_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/Code/Libraries
)

target_sources(g_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point
    $<$<NOT:$<BOOL:${WIN32}>>:SDL3Main.cpp>
    # Linux stubs
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
    # GeneralsX @refactor android-port 08/02/2026 Shared Android startup, so both
    # engines get an identical bootstrap instead of two copies that drift apart.
    $<$<NOT:$<BOOL:${WIN32}>>:${CMAKE_SOURCE_DIR}/Core/Main/SageAndroidBootstrap.cpp>
)

# The shared Android bootstrap header lives beside its source.
target_include_directories(g_generals PRIVATE ${CMAKE_SOURCE_DIR}/Core/Main)

# RC files are optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
# TYPELIB: Broken (0 bytes) and causes windres memory exhaustion errors
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(g_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(g_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalsv${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(g_generals)
endif()
