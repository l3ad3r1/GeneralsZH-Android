# Handover: Port C&C Generals Zero Hour to TCL NXTPAPER Tab (9469X)

**For:** Gemini (implementation agent)
**From:** Claude (diagnosis + planning session, 2026-07-29)
**Repo:** https://github.com/l3ad3r1/GeneralsZH-Android (user `l3ad3r1` has ADMIN; `gh` CLI is authenticated on this PC)
**Working dir on PC:** `E:\claude-projects\GeneralsZH-Android-Debug`
**Read first:** `WORKLOG.md` (full detail of every binary patch + the DXVK swapchain investigation), `android.md` (upstream's engineering log), `docs/KNOWN_ISSUES`.

---

## 1. Context — what already works

The port **runs stably on the user's Samsung S24 Ultra** (Adreno GPU, Vulkan 1.3). Getting there required fixes that are now in `main`:

| Fix | Where |
|---|---|
| Double-free of SDL-owned storage-path pointers → SIGABRT at every launch | source: `GeneralsMD/Code/Main/SDL3Main.cpp` (commit `a1e461c27`); binary: 5 NOPs in released libmain.so |
| Portrait-native panel + landscape surface → `VK_SUBOPTIMAL_KHR` every frame → DXVK rebuilds swapchain ~20×/s → allocator race → SIGSEGV | source: `SDL_HINT_ORIENTATIONS` in SDL3Main.cpp (same commit); binary: dex patch (orientation 13→6) + 3-instruction patch in libdxvk_d3d9.so presenter (treat SUBOPTIMAL as success) — see WORKLOG.md for offsets |
| Launcher icon | commit `3e783ca3a` |

Released as `v0.3-android` (APK asset on the GitHub release = the fully binary-patched, S24-verified build).

**IMPORTANT — none of these APKs were rebuilt from source.** All fixes on-device are binary patches on the upstream v0.2 APK. The source fixes in `main` are equivalent but have never been compiled (no working build toolchain on this PC — see §6).

## 2. Target device — verified facts (probed 2026-07-29 over adb)

| Property | Value |
|---|---|
| Device | TCL NXTPAPER tab, model **9469X**, board `Bellona_WF_GL` |
| adb serial | `987800005DB3824` |
| SoC | MediaTek **MT8781** (Helio G99; platform `mt6789`) |
| GPU | **ARM Mali-G57 MC2** (Valhall gen1), driver `r32p1-01eac0` |
| **Vulkan** | **1.1 only** — `feature:android.hardware.vulkan.version=4198400` (0x401000); ICD at `/vendor/lib64/hw/mt6789/vulkan.mali.so` |
| Android | 15 (SDK 35) |
| Screen | 1440×2200 portrait-native, density 320 |
| RAM | 8 GB |
| ABI | arm64-v8a |

App state on the device right now:
- `me.generalsx.zh` **installed**, signed with `my-release-key.jks` (SHA-256 `b8227749…`). The currently installed build is `GeneralsZH-tcl-diag.apk` — the full S24 patch set re-signed with this key, plus one diagnostic byte-patch (`DXVK_LOG_LEVEL` env name corrupted so DXVK logs at default level).
- **GameData already on device and readable by the app**: `/sdcard/Android/data/me.generalsx.zh/files/GameData/` (74 entries incl. all .big archives, fonts already extracted). Shell-pushed (owner `shell`) but the app reads it fine on this Android 15 build — do NOT repeat the S24 handover's claim that adb-pushed files are unreadable; it is disproven on both devices.
- Leftover from an old attempt: `files/commandline.txt` containing `-win` — the engine has **no** file-based command-line reader; it's dead weight, ignore/delete.

## 3. The blocker — exact failure, with evidence

With the fixed (no-double-free) APK, the engine boots fully on the TCL: chdir to GameData OK, all .big archives load, audio init OK, `ThingFactory done`. It dies at **D3D device creation**:

```
E GeneralsX: GetAdapterDisplayMode returned D3DFMT_UNKNOWN, defaulting to D3DFMT_X8R8G8B8   (×3)
I Zygote  : Process NNNN exited cleanly (1)
```

and the engine writes `GameData/ReleaseCrashInfo.txt`:

```
; Reason Please make sure you have DirectX 8.1 or higher installed. Also verify that your
video card meets the minimum requirements
```

That is the engine's generic "CreateDevice failed" path. Root cause: **the bundled DXVK (fbraz3 fork of DXVK 2.6) requires a Vulkan 1.3 device; the Mali-G57 r32p1 driver exposes Vulkan 1.1.** `GetAdapterDisplayMode` returning `D3DFMT_UNKNOWN` (it returns a real format on the S24) is the first symptom — DXVK has no usable VkPhysicalDevice, so the whole d3d9 layer is a husk. No native crash, clean `exit(1)`.

Note: `app_process64_d3d9.log` in GameData is created but 0 bytes even with logging enabled — DXVK exits before writing. Don't burn time on it; instrument `vkEnumeratePhysicalDevices`/instance creation in the fork source if you need more detail.

## 4. Implementation plan

### Milestone 0 — confirm driver ceiling (½ h)
Install a Vulkan caps viewer (e.g. Sascha Willems' `VulkanCapsViewer` APK) or run a tiny NDK vulkaninfo binary via `adb shell` to dump `VkPhysicalDeviceProperties.apiVersion` directly. The framework feature flag says 1.1; confirm the driver doesn't secretly report higher. **If it somehow reports ≥1.3, skip to Milestone 3 — everything should already work.** (Unlikely.) Also check TCL OTA updates: ARM's unified r41+ driver gives Valhall GPUs Vulkan 1.3; a vendor update would dissolve the blocker for free.

### Milestone 1 — SwiftShader fallback: prove the whole stack (1–2 days)
Goal: get the game rendering on the TCL with **zero DXVK changes**, using SwiftShader (Google's software Vulkan implementation, supports 1.3, supports `VK_KHR_android_surface`).

1. Obtain/build `libvk_swiftshader.so` for arm64 (from AOSP/SwiftShader repo, or extract from Chrome/an emulator image).
2. The fbraz3-dxvk Android WSI loads the system `libvulkan.so`. Add an env/property-gated override so it `dlopen`s a bundled `libvk_swiftshader.so` instead (SwiftShader exports `vk_icdGetInstanceProcAddr`; you can loader-shim it or link its exported `vkGetInstanceProcAddr` directly — SwiftShader also ships a full libvulkan-compatible build target `swiftshader_libvulkan`, easiest to consume).
3. Stage it in `jniLibs/arm64-v8a/`, rebuild, run.
4. Expectation: menu + gameplay at low FPS (Helio G99 CPU). This validates every layer above Vulkan on this device (input, audio path, formats, lifecycle) and gives the user something playable immediately. Cap resolution via `GameData/Options.ini` → `Resolution = 1100 720` (or similar 3:2-ish) to keep software rasterization affordable; confirm the engine honors it (it does — `OptionPreferences::getResolution`).

### Milestone 2 — hardware path: DXVK 1.10.3 + d3d8→9 shim (1–2 weeks, the real port)
DXVK **1.10.3** (last 1.x) runs on Vulkan 1.1-class devices. But the d3d8 frontend only exists in DXVK 2.x. Plan:

1. Branch `references/fbraz3-dxvk` at the Android-port commits; identify the Android-specific deltas (Meson cross files, ELF-not-PE build, SDL3/ANativeWindow WSI, SSE-flag gating — upstream documented these in `android.md`). Transplant those deltas onto a `v1.10.3` checkout. This is the bulk of the work: the WSI layer moved a lot between 1.x and 2.x, so expect to port the *concepts*, not cherry-pick patches.
2. For d3d8: integrate **d3d8to9** (Crosire) as a static translation layer compiled into (or in front of) the d3d9 lib, exposing the `Direct3DCreate8` entry point `libmain.so` links against. Keep the exported soname/symbols identical to the current `libdxvk_d3d8.so` so the engine needs no changes.
3. Mali-specific care: G57 lacks BC texture compression (desktop DXT/S3TC). Check what the engine's textures use — Generals ships DXT-compressed .dds inside .big. DXVK 1.x can decompress BC→RGBA on upload when `VK_EXT_texture_compression_astc`/BC is missing? **It cannot on 1.x** (BC support is a hard device requirement for most D3D9 games). If Mali reports no BC: this becomes the second big work item — add a BC-decode-on-upload path (dxvk has `d3d9.supportDFmt` style toggles but not BC emulation in 1.x; Adreno exposes BC which is why the S24 never surfaced this). Budget for it; check `VkPhysicalDeviceFeatures.textureCompressionBC` in Milestone 0 to know early.
4. Apply the same presenter lesson from WORKLOG.md: treat `VK_SUBOPTIMAL_KHR` as success (1.10.3 has the same desktop-minded recreate logic) — the TCL panel is also portrait-native (1440×2200), so the SUBOPTIMAL storm WILL recur in landscape.

### Milestone 3 — device polish + soak (1 day)
- Landscape: already handled by the dex patch in the installed APK / `SDL_HINT_ORIENTATIONS` in source.
- `Options.ini`: `Resolution = 2200 1440` (or panel-matched), `StaticGameLOD = Custom`, `HeatEffects` can stay on if Milestone 2 is solid.
- `SagePatch.ini`: `ShellMapOn = No` optional (menu battle scene; on Helio G99 it may be worth keeping off for thermals).
- Soak: 8 min menu + 15 min skirmish + campaign mission load, `logcat -b crash` clean, then hand to user.

### Recommended sequencing
M0 → M1 (ship to user as "TCL beta, software rendering") → M2 in parallel. If M0 reveals a Vulkan 1.3 driver via OTA, or ARM releases r41 for this SoC, M2 collapses to "rebuild + test".

## 5. Signing / device safety rules (do not violate)

- **TCL (9469X):** installed cert = `my-release-key.jks` (repo root), store/key password `android`. `adb install -r` with that key preserves data. GameData on the TCL is re-pushable from the PC source (`C:\Users\renja\Downloads\Command and Conquer Generals + Zero Hour\Command & Conquer Generals Zero Hour\`), so a wipe is recoverable — but avoid it anyway.
- **S24 Ultra (serial `RZCY51R2A8D`):** installed cert = `debug.keystore` (repo root), password `android`. **NEVER `adb uninstall` on the S24** — its GameData was hand-copied via Solid Explorer by the user and an uninstall deletes `Android/data/me.generalsx.zh/`. NOT `~/.android/debug.keystore` — wrong cert, install will fail with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.
- Don't ship the TCL diagnostic APK further: it contains the `DXVK_LOG_LEVEX` byte-patch (logging left enabled). Rebuild clean from `GeneralsZH-icon2-aligned.apk` (pre-signing artifact, all fixes, no diag patch) or from source.

## 6. Build environment on this PC (Windows 11, git-bash + PowerShell)

- **Present:** Android SDK platform-tools (`C:\Users\renja\AppData\Local\Android\Sdk\platform-tools\adb.exe`), build-tools 35/36 (zipalign, apksigner, aapt2), NDK **28.2.13676358** (upstream used r27 — watch for differences), JDK/keytool, Python w/ PIL+numpy at `C:\Users\renja\AppData\Local\hermes\hermes-agent\venv\Scripts\python.exe`.
- **Missing:** CMake ≥3.25, Meson, Ninja, Gradle (no wrapper JAR in repo — `android/` has `gradle-wrapper.properties` but no `gradlew`). Install these first.
- **Scripts assume macOS**: `scripts/build/android/package-android-zh.sh` hardcodes `darwin-x86_64` NDK prebuilt paths and `~/Library/...`. Adapt to Windows (`windows-x86_64` prebuilts) or build under WSL.
- Gotchas that cost hours: `zipalign -P 16 4` (capital P on build-tools 36; run BEFORE signing); git-bash mangles `/sdcard/...` paths — `export MSYS_NO_PATHCONV=1`; `.so` entries in the APK must stay STORED/uncompressed and 16K-page-aligned (`extractNativeLibs=false`).
- The DXVK submodule is **not initialized** locally: `git submodule update --init references/fbraz3-dxvk`.

## 7. Success criteria

1. Game reaches main menu on the TCL 9469X and survives an 8-minute idle soak (no swapchain-recreate storm in the d3d9 log, no crash-buffer entries).
2. Skirmish map loads and 15 minutes of play completes without SIGSEGV/SIGABRT.
3. Whatever renderer path ships (SwiftShader or DXVK-1.x), the S24 Ultra build is unaffected (it stays on the current DXVK 2.6 path — gate any loader changes by device/driver probe, not build-time).
4. All source changes land in `main` with the same `GeneralsX @bugfix/@feature` comment conventions used in `SDL3Main.cpp`, and a release tag with APK assets for both signing keys.
