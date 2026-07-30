# Phase 6 — Android Tablet Port

**Status:** Runtime proven on Adreno and Mali. TCL 9469X now reaches the Zero
Hour menu through native Mali Vulkan using legacy DXVK at 29-30 FPS with
software-decoded DXT textures; full gameplay and lifecycle validation remain.
**Started:** 2026-07-06
**Target:** Android tablets, arm64-v8a, landscape, minSdk 24 (Android 7.0+, native Vulkan).

## Goal

Bring C&C Generals Zero Hour to Android tablets, mirroring the iOS/iPad port's
architecture (DXVK + SDL3 + OpenAL) but targeting Android's **native Vulkan**
(no MoltenVK translation layer — simpler than iOS).

## Why this is cheaper than greenfield

The iOS port did the hard, portable work already. A `SAGE_MOBILE` macro now
gates the platform-agnostic mobile code (touch gesture state machine, app-lifecycle
render-gate, high-DPI drawables, `-xres/-yres` aspect matching) so it compiles
for both iOS and Android. Of the iOS-guarded engine blocks, **11 are direct-reuse**
(widen the guard), 5 are adapted (logging, VFS paths), 0 are new engine work.

See `docs/port/ANDROID_FEASIBILITY.md` for the full feasibility analysis.

## The one hard gate: DXVK on Android

DXVK has **never been built for Android** upstream (zero `android` references in
doitsujin/dxvk; maintainer closed #1183 with "No" — but about the Wine/emulation
path, not native linking). This port uses the **native, non-Wine** path the game
already uses on Linux/iOS.

### Spike result (2026-07-06): ✅ DXVK-ON-ANDROID PROVEN ON REAL HARDWARE

**GO/NO-GO GATE: PASSED.** DXVK d3d8 device creation + render loop confirmed working
on a OnePlus tablet (Adreno 830, Android 16, arm64-v8a). The #1 risk of the entire
port — "can DXVK render via d3d8 on an Android Vulkan driver?" — is **answered: YES.**

Logcat proof from the device:
```
I dxvk-spike: dlopen(libdxvk_d3d8.so) = 0x...
I dxvk-spike: Direct3DCreate8(220) = 0x...  <-- DXVK Vulkan instance is live
I AdrenoVK:   Engine Name : DXVK / Api Version : 0x00403000 (Vulkan 1.3)
I dxvk-spike: Adapter: Adreno (TM) 830
I dxvk-spike: CreateDevice(HAL) = 0x...  hr=0x00000000  <-- D3D8 DEVICE ALIVE
I dxvk-spike: SUCCESS: DXVK d3d8 device created on Android. Clearing frames...
```

The render loop ran continuously (96% CPU, stable, no Present errors) — Clear + Present
every frame via DXVK on the Adreno 830 Vulkan driver.

**What was proven:**
1. ✅ First-ever native DXVK d3d8 + d3d9 build for Android aarch64.
2. ✅ `Direct3DCreate8` → DXVK VkInstance (Vulkan 1.3) → Adreno 830 enumerated.
3. ✅ `SDL_Vulkan_CreateSurface` creates a real `VkSurfaceKHR` on Android.
4. ✅ D3D8 `CreateDevice` returns `D3D_OK` (hr=0x00000000) — the full pipeline works.
5. ✅ Clear + Present render loop runs indefinitely with no errors.

**Bugs found and fixed during the spike:**
- DXVK `-msse*` flags gated behind x86 (`Patches/dxvk-android.patch`).
- `VK_ENABLE_BETA_EXTENSIONS` for portability-subset feature structs.
- DXVK SDL3 WSI `libSDL3.so.0` → `libSDL3.so` soname (Android ships unversioned).
- **D3D8 present-params gotcha:** windowed mode requires `FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT` (0), not IMMEDIATE — otherwise `D3DERR_DRIVERINVALIDCALL` (0x8876086c). This applies to the game too (SDL3Main.cpp sets this correctly already via the engine's own defaults).

### TCL Mali result (2026-07-30): legacy DXVK + software DXT decode

The TCL 9469X cannot run the normal Android DXVK 2.6 lane: its Mali-G57 MC2
driver exposes Vulkan 1.1.177 and no BC/DXT texture formats. A separate
hardware lane now runs through DXVK Native 1.9.2b plus a 64-bit-safe
`d3d8to9` frontend.

The Mali device gate and full game both pass:

1. D3D9 creates a device on the real Mali driver.
2. D3D8 creates a device through `d3d8to9`.
3. DXT1/3/5 capability queries return `D3DERR_NOTAVAILABLE`, causing
   `Get_Valid_Texture_Format` and `DDSFileClass::Copy_Level_To_Surface` to
   expand compressed game textures before upload.
4. The animated Zero Hour menu renders with correct textures, colors, icons,
   and dynamic text.
5. The in-game counter holds 29-30 FPS at 2200x1440, compared with roughly
   5-8 FPS through SwiftShader.

Two compatibility fixes were required beyond the Android/ARM64 WSI port:

- Do not request unsupported BC, non-solid fill, multi-viewport, shader clip,
  or shader cull features when creating the Vulkan device.
- Treat persistent `VK_SUBOPTIMAL_KHR` presentation as usable on Android so the
  swapchain is not destroyed and rebuilt every frame.

The D3D8 frontend also needs a D3DX-free full-surface `CopyRects` fallback for
the engine's A4R4G4B4 dynamic font atlases. Its pointer-derived shader handles
were replaced with a 32-bit handle map so the wrapper remains valid in the
arm64 process.

The reproducible source delta is tracked in:

- `Patches/dxvk-native-1.9.2b-android-sdl3.patch`, based on DXVK Native
  `native-1.9.2b` / `c8dc91fabd00cac11d697ccf07426e798393cd40`.
- `Patches/d3d8to9-android-arm64.patch`, based on d3d8to9
  `6cdb8a82184898f1b9371e4c8412c2d33ebb7b51`.
- `android/mali-spike`, the D3D9 and D3D8 real-device gate.
- `scripts/build/android/repack-mali-apk.py`, the preview packaging helper.

### The spike artifacts (reproducible)
- `build/android-spike/dxvk-build/src/d3d8/libdxvk_d3d8.so` — the DXVK d3d8 library
- `build/android-spike/harness/d3d8_clear.cpp` — the go/no-go test harness
- `build/android-spike/dxvk-d3d8-spike-signed.apk` — installable spike APK
- `Patches/dxvk-android.patch` — all 4 DXVK source fixes (SSE gate, SDL3 soname,
  high-DPI func table, pixel-size WSI call)

## Changes made (2026-07-06)

### Engine source — mobile code reuse
- `GeneralsMD/Code/Main/SDL3Main.cpp` — introduced `SAGE_MOBILE` (iOS ∨ Android);
  widened touch-hint, high-DPI flag, `-xres/-yres` blocks; added Android bootstrap
  (VFS chdir to internal storage, portable stderr→file sink via `dup2`, DXVK
  cache path). `#elif TARGET_OS_IPHONE` keeps the iOS-only `funopen` block intact.
- `GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp` — widened the
  lifecycle render-gate, `iosLifecycleWatcher` (added `WINDOW_MINIMIZED/RESTORED`
  for Android's lifecycle), touch-mouse dedup, FINGER event routing, and long-press
  poll to `SAGE_MOBILE`. The ~230-line touch gesture state machine ports verbatim.
- `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` — documented the Linux/Android
  shared dlopen branch (bare `libdxvk_d3d8.so` resolves via the app lib dir).
- `Core/Libraries/Source/WWVegas/WW3D2/render2dsentence.cpp` — widened the iOS font
  locator to also match `__ANDROID__` (both lack fontconfig; both use bundled fonts).
- `GeneralsMD/Code/Main/CMakeLists.txt` — `add_library(z_generals SHARED)` named
  `main` on Android (SDL's `nativeRunMain` dlopens `libmain.so`).

### Build system
- `CMakePresets.json` — `android-vulkan` preset (CMAKE_SYSTEM_NAME=Android, API 24,
  arm64-v8a, NDK, ZH-only, tools off).
- `cmake/triplets/arm64-android.cmake` — vcpkg overlay triplet.
- `cmake/dx8.cmake` — `elseif(ANDROID)` branch (DXVK-from-source via Meson + NDK).
- `cmake/meson-android-aarch64-cross.ini.in` — Meson NDK cross-file template.
- `Patches/dxvk-android.patch` — DXVK source patches for aarch64.

### Android project + packaging
- `android/` — Gradle wrapper project (settings/build/properties, app/build.gradle
  with `externalNativeBuild` pointing at the repo-root CMakeLists.txt).
- `android/app/src/main/AndroidManifest.xml` — landscape, fullscreen, largeHeap,
  Vulkan feature requirement, configChanges (no Activity recreation).
- `android/app/src/main/java/me/generalsx/zh/GameActivity.java` — SDLActivity
  subclass pinning the native library load order.
- `android/config/dxvk.conf` — DXVK runtime config (mirrors iOS).
- `scripts/build/android/package-android-zh.sh` + `stage-fonts.sh` + `README.md`.

## Not done (follow-up)

1. **TCL Mali productization** — move the pinned DXVK Native 1.9.2b and
   `d3d8to9` patch sets into the normal Android build, then select the legacy
   lane by product flavor or runtime capability.
2. **TCL gameplay and correctness pass** — run skirmish/replay and
   background/resume soak tests, verify every DXT conversion path, and measure
   memory growth from expanded RGBA textures.
3. **TCL render-scale control** — stop forcing a 2200x1440 backbuffer and add a
   lower internal resolution option for heavier matches.
4. **ffmpeg** — disabled by default; vcpkg ffmpeg:arm64-android is broken (#33963).
   Hand-build with the NDK standalone toolchain.
5. **GameData asset delivery** — sideload-push to internal storage for now; an
   OBB / first-run extraction pipeline is needed for a polished release.
6. **Vulkan surface lifecycle on resume** — SDL3 has open bugs (#10279, #12957)
   on Android resume; budget explicit surface teardown/recreation.
7. **Memory characterization** — Android per-process caps (worse on Android 17's
   MemoryLimiter) may bite the ~3GB-RSS engine; test on target hardware.

## Open questions / risks

- **GPU driver fragmentation**: Mali vs Adreno Vulkan drivers vary widely; DXVK's
  caps queries (the source of the iOS black-minimap bug) face a wider variance.
- **Memory**: Android's per-process ceiling is often below iOS's 3GB.
- **Resume**: surface destruction on background + open SDL3 bugs.
