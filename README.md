# Command & Conquer: Generals Zero Hour — Android

<img src="assets/android-screenshot.png" alt="Generals Zero Hour main menu running on Android tablet" width="600">

**The full 2003 RTS engine running natively on Android tablets** — not emulation,
not a compatibility layer. The real C++ engine compiled for arm64, rendering
DirectX 8 → [DXVK](https://github.com/doitsujin/dxvk) → Vulkan on Adreno/Mali GPUs.
This is the **first-ever DXVK build for Android**.

> ⚠️ **You must own a legal copy of the game.** No game assets are included or
> distributed. See [How to Play](#how-to-play) below.

---

## Status

| Feature | Status |
|---------|--------|
| Engine init (all subsystem stores) | ✅ |
| DXVK D3D8→Vulkan rendering | ✅ |
| Main menu renders with text | ✅ |
| Touch input (tap, drag, pinch) | ✅ |
| Audio playback | ❌ OpenAL initializes but no sound yet |
| Full gameplay session (skirmish) | ✅ |

### Verified devices

| Device | GPU / driver | Renderer | Result |
|---|---|---|---|
| OnePlus Pad 2 | Adreno 830, Vulkan 1.3 | Current DXVK lane | Full gameplay verified |
| TCL NXTPAPER 9469X | Mali-G57 MC2, Vulkan 1.1.177 | Mali legacy lane | Main menu verified at 29–30 FPS, 2200×1440 |

The TCL result was measured on physical hardware on 2026-07-30. Text, icons,
colors, animated menu rendering, and touch input are correct. The Mali build is
a technical preview: full skirmish/replay soak, suspend/resume, and memory-growth
testing are still open.

---

## How to Play

### What you need

1. **An Android tablet** with:
   - **arm64-v8a** architecture (all modern tablets)
   - **Android 7.0+** (API 24+, for system Vulkan support)
   - A **Vulkan-capable GPU** (modern Adreno, or a supported Mali device using
     the separate Mali preview APK)
   - **~3GB free RAM** for the game process
   - **~2.5GB storage** for game data

2. **A legal copy of C&C Generals Zero Hour**:
   - [Steam](https://store.steampowered.com/app/2732960/) (~$5 on sale, includes base game + Zero Hour)
   - Or any retail/EA App/Origin install
   - You need the **Complete Edition** or both Generals + Zero Hour installed

### Step 1: Download the APK

Grab the latest APK from the [**Releases page**](../../releases):

- Use the normal Android APK for modern Adreno devices with Vulkan 1.3.
- Use `GeneralsZH-TCL-Mali.apk` for Mali-G57-class devices limited to Vulkan
  1.1 and without BC/DXT texture support.

### Step 2: Install the APK

```bash
# Enable "Install from unknown sources" for your file manager first
adb install GeneralsZH-full.apk

# Or transfer the APK to your tablet and tap it in Files
```

### Step 3: Copy your game data

The game needs its `.big` archive files on your tablet's filesystem:

```bash
# Create the game data directory on the tablet
adb shell mkdir -p /sdcard/Android/data/me.generalsx.zh/files/GameData/Data

# Copy ALL .big files from your PC install to the tablet
# (from your Generals install directory, typically):
#   C:\Program Files (x86)\Steam\steamapps\common\Generals\
adb push "*.big" /sdcard/Android/data/me.generalsx.zh/files/GameData/Data/

# The fonts are bundled in the APK and extract automatically on first launch.
```

**Required .big files** (copy all of these from your install's `Data/` folder):

| File | Contents |
|------|----------|
| `INI.big` | Base game INI data (weapons, objects, etc.) |
| `INIZH.big` | Zero Hour INI data |
| `Textures.big` / `TexturesZH.big` | Game textures |
| `Audio.big` / `AudioZH.big` | Sound effects |
| `Music.big` / `MusicZH.big` | Music tracks |
| `MapsZH.big` | Map data |
| `Terrain.big` / `TerrainZH.big` | Terrain data |
| `W3D.big` / `W3DZH.big` | 3D models |
| `English.big` / `EnglishZH.big` | English text/speech |
| `Speech*.big` | Voice-over files |
| `Window.big` / `WindowZH.big` | UI textures |
| `ShadersZH.big` | Shaders |

### Step 4: Play

Launch **"Generals ZH"** from your app drawer. The main menu should appear
within a few seconds.

---

## Touch Controls

The touch input system maps touchscreen gestures to the RTS mouse semantics the
2003 engine expects:

| Gesture | Action |
|---------|--------|
| **Tap** | Left-click (select unit, click button) |
| **Tap and hold (600ms)** | Right-click (context menu, deselect) |
| **Drag** | Left-click drag (selection box) |
| **Two-finger drag** | Right-click drag (camera pan) |
| **Two-finger pinch** | Mouse wheel (zoom in/out) |

---

## Build from Source

### Prerequisites

- **Android NDK r27** (27.1.12297006)
- **Android SDK** with build-tools 35.0.0
- **CMake 3.25+** and **Ninja**
- **Meson** (for DXVK cross-compilation)
- **Android Studio** or Gradle 8.7+ (for the APK packaging)

### Build steps

```bash
# Clone
git clone https://github.com/l3ad3r1/GeneralsZH-Android.git
cd GeneralsZH-Android

# Initialize the DXVK fork submodule
git submodule update --init references/fbraz3-dxvk

# Configure + build the native engine (arm64-v8a)
cmake --preset android-game
cmake --build build/android-game --target z_generals

# Package the APK (stages fonts, DXVK .so, SDL3 .so, etc.)
./scripts/build/android/package-android-zh.sh

# The signed APK appears at:
#   android/app/build/outputs/apk/release/app-release.apk
```

### Mali legacy renderer

The Mali preview uses two pinned upstream components:

- `Joshua-Ashton/dxvk-native`, tag `native-1.9.2b`, commit
  `c8dc91fabd00cac11d697ccf07426e798393cd40`
- `crosire/d3d8to9`, commit
  `6cdb8a82184898f1b9371e4c8412c2d33ebb7b51`

Apply [`Patches/dxvk-native-1.9.2b-android-sdl3.patch`](Patches/dxvk-native-1.9.2b-android-sdl3.patch)
and [`Patches/d3d8to9-android-arm64.patch`](Patches/d3d8to9-android-arm64.patch)
to those exact revisions. The `android/mali-spike` module provides the D3D9 and
D3D8 hardware gates used before packaging the full app.

Mali GPUs do not expose the game's BC/DXT texture formats. The renderer reports
those formats unavailable, allowing the existing engine path to decode DXT1,
DXT3, and DXT5 textures to uploadable RGBA surfaces. No game assets are modified.

For more details, see [`android.md`](android.md) — the complete engineering log
of every bug found and fixed during the port.

---

## What This Port Involved

Getting a 2003 Windows DirectX 8 game running natively on Android required:

1. **DXVK for Android aarch64** — DXVK had never been built for Android. Required
   gating x86 SSE flags, fixing the SDL3 WSI soname, and a high-DPI WSI patch.
2. **Android NDK cross-compilation** of the 500k LOC engine — resolving every
   missing POSIX function (`pthread_cancel`, `glob`, `sys/timeb`), every libc++
   difference (`std::from_chars` float overload missing), and every assumption
   the engine made about having a writable filesystem.
3. **BIG archive file override** — the engine's archive system loaded base
   Generals data instead of Zero Hour data because of a `multimap::insert` hint
   ordering bug. Fixed with erase-and-reinsert to guarantee override precedence.
4. **Memory allocator coexistence** — the engine's custom DMA allocator
   intercepted all `operator delete` calls, including those from OpenAL and
   libc++. Fixed with a magic cookie check to distinguish engine allocations.
5. **DXVK device creation** — the `CreateDevice` call failed because the engine's
   fullscreen format-selection path returned `D3DFMT_UNKNOWN` on Android (no
   desktop display modes). Fixed by forcing the windowed format path on all
   non-Windows platforms.
6. **Font extraction** — Android APK assets are invisible to `fopen()`. The
   engine's FreeType font locator expects `fonts/*.ttf` on the filesystem.
   Added JNI-based extraction from `AAssetManager` on first launch.

**Full bug list with root causes and fixes:** [`android.md`](android.md)

---

## Architecture

```
┌──────────────────────────────────────────────┐
│              Game Engine (C++)                │
│         (~500k LOC, GPL v3 source)            │
├──────────────────────────────────────────────┤
│  Graphics: DirectX 8 API calls                │
│     ↓                                         │
│  DXVK (libdxvk_d3d8.so) — D3D8 → Vulkan      │
│     ↓                                         │
│  Android Vulkan Driver (Adreno / Mali)        │
├──────────────────────────────────────────────┤
│  Windowing: SDL3 (touch → synthetic mouse)    │
│  Audio: OpenAL Soft                           │
│  Video: FFmpeg (stubbed for now)              │
├──────────────────────────────────────────────┤
│  Android OS (arm64-v8a, API 24+)              │
└──────────────────────────────────────────────┘
```

The engine speaks DirectX 8. DXVK translates those calls to Vulkan. The Android
Vulkan driver renders to the screen. No Wine, no QEMU, no emulation — the engine
itself is compiled as a native Android shared library (`libmain.so`).

---

## Lineage & Credits

This port stands on a chain of community work:

- **Westwood / EA Pacific** — the original game
- **EA** — the GPL v3 source release
- **[TheSuperHackers](https://github.com/TheSuperHackers/GeneralsGameCode)** — community mainline: build modernization, VC6→modern toolchain, cross-platform groundwork
- **[Fighter19](https://github.com/Fighter19/CnC_Generals_Zero_Hour)** — original Unix/64-bit port: SDL3, DXVK approach, FreeType text rendering
- **[fbraz3/GeneralsX](https://github.com/fbraz3/GeneralsX)** — macOS/Linux port integrating the above
- **[ammaarreshi/Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad)** — iOS/iPadOS port (DXVK-on-iOS, touch controls, app lifecycle)
- **This fork** — the Android arm64 port
- **DXVK, SDL3, OpenAL Soft, Liberation Fonts** — the open-source load-bearing walls

Engine code is **GPL v3** (EA's source release → the chain above → this fork).
Game assets are not included, not licensed here, and not distributed.

---

## Legal

This project does not include or distribute any game assets, data files, or
 copyrighted game content. It is an engine port that requires the user to
 provide their own legally-obtained copy of Command & Conquer Generals Zero Hour.

Command & Conquer is a trademark of Electronic Arts Inc. This project is not
affiliated with or endorsed by Electronic Arts.
