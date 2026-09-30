# GeneralsZH Android v1.1

Maintenance release integrating stability fixes, rendering fallback guards, and automated build packaging.

## What's Changed

### Rendering & Engine Fixes
- **Terrain Shader Null-Pointer Guards**: Added safety guards in `W3DShaderManager.cpp` across `TerrainShader2Stage`, `TerrainShader8Stage`, `TerrainShaderPixelShader`, `FlatTerrainShaderPixelShader`, and `CloudTextureShader` to prevent `SIGSEGV` crashes when retail `ShadersZH.big` stub shaders fail to load on Android.
- **Graceful Flat Terrain Fallback**: Made `FlatTerrainShaderPixelShader::init()` fall back gracefully to the 2-stage shader path when pixel shaders fail, rather than aborting terrain initialization.
- **Height Map First-Frame Guard**: Guarded `m_stageTwoTexture->restore()` in `FlatHeightMap.cpp` and `HeightMap.cpp` to prevent null-dereference crashes on first frame rendering.
- **Base Generals Video Player Stub**: Added non-Windows `BinkVideoPlayerStub` implementation for `g_generals` to fix linker errors when building the base game without Windows Bink SDK.

### Driver & Runtime Compatibility
- **DXVK Portability Subset Guard**: Guarded MoltenVK-specific `VK_KHR_portability_subset` calls with `#ifdef __APPLE__` in `Patches/dxvk-android.patch`, allowing DXVK to compile cleanly with Android NDK Vulkan headers.
- **DXVK Version Validation**: Updated Gradle `checkStagedDxvk` to support and accept both DXVK 1.9.2 and DXVK 2.6 runtimes.
- **CMake Dependency Resolution**: Ensured DXVK ExternalProject explicitly depends on `SDL3-shared` so `pkg-config` can locate SDL3 during cross-compilation.

### Launcher & Data Safety (from v0.13/v1.0)
- **Transactional Imports**: Interrupted or short game-file imports no longer overwrite or corrupt existing archives.
- **Transactional Mod Downloads**: Cancelled or failed downloads leave the previous file intact.
- **Lifecycle & Locale Resilience**: Activity recreation is handled safely with UI controls disabled during background operations; archive extension matching is resilient under all locales.

### Build & Packaging Automation
- **Automated CI Workflow**: Added a reliable two-pass packaging workflow in `.github/workflows/build-android.yml` to compile DXVK via Meson, stage all native `.so` libraries into `jniLibs`, and produce release APKs.
