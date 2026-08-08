# Android port — working state

Last updated 2026-08-06. Written to compress a long session; read this before
picking the work back up.

## Where things stand

Both engines ship in one APK and both run on device.

| | Zero Hour | Generals |
|---|---|---|
| Boots, native resolution, terrain | yes | yes |
| Skirmish / campaign | full missions | reaches mission intro |
| Touch, mouse, audio, video | verified | shared code, unverified |

Mods work: `.gib` archives load, Rise of the Reds and ShockWave both playable
with their own menus, factions and skirmishes.

## Open issues

1. ~~**Black models — Rise of the Reds only.**~~ **FIXED 07/08/2026.**
   `DDSFileClass::Get_4x4_Block()` had no DXT3 case — it returned without
   writing anything, so every DXT3 texture reached the GPU as an all-zero
   surface and drew black. DXT1 and DXT5 were implemented; DXT2/3/4 were not.
   This device cannot sample DXT directly so everything goes through that
   decompression path, which is why it bit here and not on desktop. Proof was a
   texel readback of every bound texture correlated against its source format:
   22/22 DXT1 and 6/6 DXT5 held real data, 5/5 DXT3 were 100% zero. It also
   explains the observation that derailed the search for hours — the plant's
   body is DXT3 and drew black while the fence around it is DXT5 and drew fine.
   Fixed by implementing DXT2/DXT3 and routing DXT4 through the DXT5 decoder.
   Only RotR was affected because its building textures are DXT3; vanilla and
   ShockWave are DXT1/DXT5 throughout. Full trail in
   `docs/port/KNOWN_ISSUE_BLACK_MODELS.md`.
2. **DXVK 2.6 crashes on the S24/Adreno** in
   `DxvkResourceAllocationPool::alloc()`, fault addr `0x2000000001`. Works fine
   on DXVK Native 1.9.2b (TCL/Mali). The operator new/delete version script IS
   correctly applied to both engines -- checked, not assumed.
   **Corrected 08/08/2026: this is NOT Generals-only.** Zero Hour was recorded
   here as fine on both; it is not. On v0.10 the S24 boots and renders the Zero
   Hour main menu correctly at 3120x1440, then dies entering the skirmish setup
   screen with the same fault address, through the text path:
   `Render2DSentenceClass::Render()` -> `DX8Wrapper::Draw()` ->
   `D3D9DeviceEx::UpdateFixedFunctionVS()` -> `D3D9ConstantBuffer::AllocSlice()`
   -> `DxvkResourceAllocationPool::alloc()`. Generals crashes on the same
   allocator via the swapchain path instead (`Presenter::createSwapChain()`).
   **Not a v0.10 regression -- settled by A/B on device 08/08/2026.** v0.9's
   `libmain.so` swapped into the v0.10 APK (everything else in the two Vulkan
   APKs is byte-identical except the two engine libs, so that is the only
   variable) crashes at the same point, same fault address, same backtrace,
   with only the BuildId differing (`bbd357b9` v0.9 vs `7d448b17` v0.10). No
   uninstall was needed for this: keeping the v0.10 shell keeps versionCode 10,
   which sidesteps the downgrade block below. Use that trick rather than
   uninstalling whenever the engine is the variable under test.
3. **Loose mod files are not read.** The engine reads only archives and videos
   from a `-mod` dir, so `Data/Scripts/SkirmishScripts.scb` never loads. Affects
   Generals Continue and Project Raptor (AI may not work). Mods packing
   everything into `.gib` are unaffected.
4. **AI set to `Random` army in a mod = instant win.** Random resolves to one of
   the mod's added generals whose AI data the engine cannot drive, so that
   player is immediately defeated. Pick a faction explicitly. Not a port bug as
   far as we know.

## Things that cost time — do not re-derive

- **Shader stubs.** `shaders.big`/`ShadersZH.big` in this install are ~1 KB
  stubs. `terrain.pso`, `roadnoise2.pso`, `monochrome.pso` therefore always fail
  to create (`hr=0x8876086c`). This is normal here and is NOT the black-model
  cause: ShockWave ships real shaders, still fails to create them, and has no
  black models.
- **Missing-texture placeholder is magenta** (`0x7FFF00FF`), not black. A black
  model is never a missing texture.
- **Shadow volumes draw as opaque black geometry** through DXVK on Mali. The
  launcher passes `-noshadowvolumes` by default — but note that flag was inert
  in every release build until 07/08/2026 (its table entry was inside
  `#if defined(RTS_DEBUG)`, and the LOD table and Options.ini both reassigned
  the value afterwards), so anything previously credited to it should be
  treated as unverified.
- **DXT2/3/4 decompression was missing** in `DDSFileClass` until 07/08/2026 and
  silently wrote nothing. If a texture is black, read its texels back off the
  GPU before trusting anything the engine reports about it — `init=1`,
  `missing=0` and a correct-looking format all held true while the surface was
  entirely zero.
- **`SDL_HasMouse()` returns 0 on Android** with a mouse attached, and a real
  mouse reports `SDL_MouseID` **0** -- the same value a zeroed synthetic event
  carries. Discriminate by tagging synthetic events `SDL_TOUCH_MOUSEID` instead.
- **Both touch/mouse synthesis hints default ON for mobile.** Turn off
  `SDL_HINT_TOUCH_MOUSE_EVENTS` *and* `SDL_HINT_MOUSE_TOUCH_EVENTS`.
- **Right-click arrives as BACK** unless `SDL_HINT_ANDROID_TRAP_BACK_BUTTON`.
- **`RELEASE_DEBUG_LOGGING` cannot be enabled**: global `LogClass` ctors run
  before the engine allocator exists; Scudo aborts.
- **Files pushed with `adb` are shell-owned and invisible to the app** until
  `chmod -R 777`. Files the app created cannot be chmod'd by shell -- that
  direction is fine and expected.
- **`adb` is a Windows binary**: give it `C:/...` paths, not MSYS `/c/...`.
- **`adb logcat`'s default buffer is 256 KiB** and silently rotates a per-draw
  trace away in seconds. `adb logcat -G 64M` before any high-volume tracing.
- **Cap diagnostic traces per unique key, never globally.** A flat line cap is
  spent on a match's opening seconds, so whatever you are actually investigating
  never gets logged.
- **A busy spinner is not evidence of success.** A "download" once ran to
  completion visually while cleartext HTTP had blocked it and nothing was
  written. Check the destination.

## Build / release

```
android/gradlew.bat :app:assembleRelease
scripts/build/android/package-mali-apk.py --base <fresh APK> \
    --dxvk-from <old Mali APK> --require-dex-string LauncherActivity --output out.apk
zipalign -f -P 16 4 ; apksigner sign --ks my-release-key.jks   (TCL, pass: android)
                                     --ks debug.keystore       (S24, pass: android)
```

- Mali variant MUST be built with `--dxvk-from`, not by grafting onto an old
  base: an old base brings its own `classes.dex`, which silently ships stale
  Java. The `--require-dex-string` guard exists because that happened.
- Never `adb uninstall` -- it deletes `Android/data/me.generalsx.zh/` including
  GameData. Always `install -r`.
- **v0.10 bumped versionCode 1 -> 10, so older builds can no longer be
  installed over it.** `install -r` fails with INSTALL_FAILED_VERSION_DOWNGRADE
  and `-d` is refused on Android 16 for a non-debuggable package. Downgrading
  now means uninstalling, which costs GameData -- so back the data up first,
  or keep a device on the old build when an A/B is needed. This is what
  blocked the v0.9-vs-v0.10 comparison on the S24.

## Devices

| Device | Serial | Keystore | Notes |
|---|---|---|---|
| TCL NXTPAPER | `987800005DB3824` | `my-release-key.jks` | Mali-G57, Vulkan 1.1, DXVK Native 1.9.2b |
| Galaxy S24 Ultra | `RZCY51R2A8D` | `debug.keystore` | Adreno 750, Vulkan 1.3, DXVK 2.6 |

Releases are at v0.8; `main` has since gained the shadow toggle, `.gib`/`.scb`
mod support, the Generals fixes, text fixes and the mod downloader. A v0.9 is
due.
