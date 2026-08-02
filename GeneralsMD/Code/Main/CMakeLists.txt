# GeneralsX @feature android-port 06/07/2026
# Android apps are shared libraries, not executables: SDL3's SDLActivity loads
# libmain.so via JNI and invokes SDL_main(). On every other platform the engine
# is a normal executable (WIN32 entry on Windows, console main elsewhere).
if(ANDROID)
    add_library(z_generals SHARED)
else()
    add_executable(z_generals WIN32)
endif()

# Use a binary name that doesn't conflict with original game.
# GeneralsX @build BenderAI 15/02/2026 Linux branding update
if(ANDROID)
    # SDL's nativeRunMain dlopens "libmain.so" and dlsyms SDL_main, so the engine
    # .so MUST be named "main" on Android regardless of branding.
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "main")
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    # GeneralsX @build BenderAI 15/02/2026 Linux branding update
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME GeneralsXZH)
endif()

# GeneralsX @build BenderAI 13/02/2026 Windows-specific libraries
if(WIN32)
    target_link_libraries(z_generals PRIVATE
        binkstub
        comctl32
        core_debug
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
target_link_libraries(z_generals PRIVATE
    core_profile_legacy
    d3dx8
    z_gameengine
    z_gameenginedevice
    zi_always
)

# GeneralsX @feature android-port 06/07/2026 Android log for __android_log_print
# and android for AAssetManager (font extraction from APK assets).
if(ANDROID)
    target_link_libraries(z_generals PRIVATE log android)
endif()

# Linux SDL3 windowing/input (Phase 1)
# GeneralsX @build BenderAI 11/02/2026
if(SAGE_USE_SDL3)
    target_link_libraries(z_generals PRIVATE sdl3lib)
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
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_sources(z_generals PRIVATE
    $<$<BOOL:${WIN32}>:WinMain.cpp>
    $<$<BOOL:${WIN32}>:WinMain.h>
    # Linux SDL3 entry point (Phase 1.5 - TheSuperHackers @build 10/02/2026)
    $<$<NOT:$<BOOL:${WIN32}>>:SDL3Main.cpp>
    # Linux stubs for P2-P3 platform functions (P2-P3 - TheSuperHackers @build 13/02/2026 BenderAI Session 37)
    $<$<NOT:$<BOOL:${WIN32}>>:LinuxStubs.cpp>
    # GeneralsX @refactor android-port 08/02/2026 Shared Android startup, so both
    # engines get an identical bootstrap instead of two copies that drift apart.
    $<$<NOT:$<BOOL:${WIN32}>>:${CMAKE_SOURCE_DIR}/Core/Main/SageAndroidBootstrap.cpp>
)

# The shared Android bootstrap header lives beside its source.
target_include_directories(z_generals PRIVATE ${CMAKE_SOURCE_DIR}/Core/Main)

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(z_generals)
endif()

# GeneralsX @bugfix android-port 14/07/2026 On Android, use a version script to
# remove operator new/delete from libmain.so's dynamic symbol table. This forces
# DXVK, SDL3, OpenAL and other loaded .so libraries to use libc++'s default
# operator new (plain malloc) instead of the engine's TheDynamicMemoryAllocator
# pool, which zeroes every allocation and garbage-fills on free — both of which
# corrupt DXVK's internal free-lists and cause the
# DxvkResourceAllocationPool::alloc() crash (fault addr 0x2000000001).
# The engine code still uses its own pool internally (static linking resolves
# the operators locally); only external .so libraries are affected.
if(ANDROID)
    set(_gx_alloc_map "${CMAKE_CURRENT_BINARY_DIR}/gx_hide_alloc.map")
    file(WRITE "${_gx_alloc_map}" [=[
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
    target_link_options(z_generals PRIVATE
        "-Wl,--version-script,${_gx_alloc_map}"
    )
endif()
