# Android port — working state

Last updated 2026-08-08. Written to compress a long session; read this before
picking the work back up.

## Where things stand

Both engines ship in one APK and both run on device.

| | Zero Hour | Generals |
|---|---|---|
| Boots, native resolution, terrain | yes | yes |
| Skirmish / campaign | full missions (TCL) | reaches mission intro |
| Touch, mouse, audio, video | verified | shared code, unverified |

As of v0.11 that table holds on **both** devices. The S24/Adreno was menu-only
while the Vulkan build shipped DXVK 2.6 -- Zero Hour died entering skirmish
setup, Generals on the swapchain -- but that was the DXVK version, not the
hardware. On 1.9.2b the S24 plays a Zero Hour skirmish at 30 FPS and Generals
reaches the mission intro, matching the TCL. See open issue 2 below for the
history.

Mods work: `.gib` archives load, Rise of the Reds and ShockWave both playable
with their own menus, factions and skirmishes. RotR's black buildings are fixed
(open issue 1). **Generals Continue** carries both the Generals and Zero Hour
campaigns on the Zero Hour engine, missions tested on the TCL -- which is why
the standalone Generals game data was removed from that device (see below).

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
2. ~~**DXVK 2.6 crashes on the S24/Adreno.**~~ **RESOLVED 08/08/2026** by
   dropping DXVK 2.6 -- v0.11 ships DXVK Native 1.9.2b on every device and the
   Mali/Vulkan split is gone.
   [Issue #4](https://github.com/l3ad3r1/GeneralsZH-Android/issues/4) is closed
   as **avoided, not fixed**: the 2.6 fault itself was never diagnosed.

   What it was: `DxvkResourceAllocationPool::alloc()`, fault addr
   `0x2000000001` -- a corrupt or uninitialised value, not exhaustion. Both
   engines hit the same allocator by different routes. Zero Hour entering
   skirmish setup via the text path (`Render2DSentenceClass::Render()` ->
   `DX8Wrapper::Draw()` -> `UpdateFixedFunctionVS()` -> `AllocSlice()`);
   Generals earlier, on `Presenter::createSwapChain()`. Both are clear on
   1.9.2b, on both GPUs.

   Two things this cost, worth not repeating:
   - It was recorded here for months as Generals-only ("Zero Hour is fine on
     both"). It never was -- nobody had taken Zero Hour past the menus on that
     device.
   - The build split was organised around Vulkan version (1.3 -> 2.6, Mali ->
     1.9.2b). That was the wrong axis: 1.9.2b works on both GPUs tested and 2.6
     works on neither, so the split cost a working Adreno build and bought
     nothing anyone had measured.

   If 2.6 is ever revisited, the diagnosis and both backtraces are on issue #4.

3. **Loose mod files are not read.** The engine reads only archives and videos
   from a `-mod` dir, so `Data/Scripts/SkirmishScripts.scb` never loads. Affects
   Project Raptor (AI may not work). Mods packing everything into `.gib` are
   unaffected.

   Generals Continue is **less affected than this entry used to claim**: its
   campaigns, maps and INI are all inside `.gib` (verified 08/08/2026 -- 28 USA,
   23 GLA and 20 China campaign missions plus 12 Generals Challenge maps in
   `!Continue35Map.gib`), and the missions are tested working on the TCL. What
   is loose is 116 files under `Data/` -- mostly campaign `.bik` cinematics plus
   `generals.str` -- so expect missing mission videos rather than missing
   missions.
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

- **Check which DXVK you are staging.** For a Java-only change the native build
  can be skipped entirely -- stage the `.so` set into `android/app/src/main/
  jniLibs/arm64-v8a/` and build with `-PSAGE_SKIP_NATIVE_BUILD=true` -- but take
  those libraries from the **shipped** `GeneralsZH-vX.Y.apk`, never from
  `android/app/build/outputs/apk/release/app-release-unsigned.apk` or whatever
  happens to be sitting in `jniLibs` already. Both carried **DXVK 2.6** as late
  as 16/08/2026, which needs Vulkan 1.3 and hard-fails on the TCL: `Skipping
  Vulkan 1.1 adapter` -> `No adapters found` -> `TheDisplay->init() threw unknown
  exception`, then a clean exit with no crash and a task restart loop that looks
  nothing like a library problem. The tell is file size --
  1.09 MB / 2.64 MB for `libdxvk_d3d8/d3d9.so` is 1.9.2b; 6.2 MB / 37.7 MB is
  2.6. `libmain.so` is identical either way, so size is the only signal.
- Mali variant MUST be built with `--dxvk-from`, not by grafting onto an old
  base: an old base brings its own `classes.dex`, which silently ships stale
  Java. The `--require-dex-string` guard exists because that happened. Note the
  inverse is fine and useful for testing -- grafting one library onto a newer
  base is how the v0.9-vs-v0.10 A/B was run without an uninstall.
- **Check the diagnostic flags before shipping.** `-DGX_TRACE_MESH` lives in
  `cppFlags` and is easy to leave on; it puts a per-mesh trace and a per-texture
  `LockRect` readback in the draw path. Removing it changes the CMake hash, so
  it also forces a full native rebuild -- budget ~5 min. Verify against the
  finished APK, not the source: `grep -c GX-MESH <apk>` (also `GX-TEXEL`,
  `GX-STAGE`, `GX-SHADOW`) must all be 0.
- Never `adb uninstall` -- it deletes `Android/data/me.generalsx.zh/` including
  GameData. Always `install -r`.
- **The TCL has no Generals game data any more** (removed 08/08/2026, ~1 GB,
  superseded by Generals Continue). Selecting "Generals (original)" in the
  launcher there now produces a silent `exit(1)` with no crash and no message --
  that is missing data, not a graphics or engine fault. Do not re-diagnose it.
  A 3.5 KB app-owned `Profiles/Generals/GeneralsX/` survives because shell
  cannot chmod or delete an app-owned directory on a non-debuggable package.
- **v0.10 bumped versionCode 1 -> 10, so older builds can no longer be
  installed over it.** `install -r` fails with INSTALL_FAILED_VERSION_DOWNGRADE
  and `-d` is refused on Android 16 for a non-debuggable package. Downgrading
  now means uninstalling, which costs GameData -- so back the data up first,
  or keep a device on the old build when an A/B is needed. This is what
  blocked the v0.9-vs-v0.10 comparison on the S24.

## Devices

| Device | Serial | Keystore | Notes |
|---|---|---|---|
| TCL NXTPAPER | `987800005DB3824` | `my-release-key.jks` | Mali-G57, Vulkan 1.1, DXVK Native 1.9.2b. **No Generals profile** -- deleted 08/08/2026, superseded by the Generals Continue mod |
| Galaxy S24 Ultra | `RZCY51R2A8D` | `debug.keystore` | Adreno 750, Vulkan 1.3. Runs DXVK Native 1.9.2b like everything else since v0.11 -- 2.6 is what crashed it before gameplay, despite the driver being capable of it |

## Releases

**v0.12 is current** (16/08/2026), `versionCode 12` — a diagnostics release.
The engine and renderer are unchanged from v0.11: same `libmain.so`, same DXVK
Native 1.9.2b. What is new is in the launcher and the manifest.

- **Verify game files** — walks every `.big`/`.gib` in the selected profile and
  active mod, checking `BIGF`/`BIG4` magic and declared size against bytes on
  disk. A truncated archive takes the native engine down before the first frame
  and looks exactly like a driver bug from outside; this names the file instead.
  Also logs to logcat under `GameFileCheck`.
- **Export engine log** — writes `generals-stderr.log` (or `-prev.log`) out
  through the system file picker. That file is the only place a DXVK/Vulkan
  capability mismatch is explained: the process then exits cleanly, with no
  crash and no tombstone. Both sessions are offered because the engine rotates
  on every launch, so after a crash the failing run is already `-prev`.
- **Manifest hardening** — cleared the outstanding MobSF findings; see
  Security below.

| Artifact | Signed with | Verified |
|---|---|---|
| `GeneralsZH-v0.12.apk` (shipped) | `my-release-key.jks` | **TCL only.** Upgraded in place from v0.11 with data intact; shell map 30 FPS, intro cinematic with video+audio; 39 archives checked in ~60 ms; exported log byte-identical to source |
| `GeneralsZH-v0.12-debugkey.apk` (local, unpublished) | `debug.keystore` | same build, for the S24. **Not yet installed or tested on the S24** |
| `GeneralsZH-v0.11.apk` | `my-release-key.jks` | **both devices, both engines.** S24: ZH skirmish at 30 FPS, Generals to mission intro. TCL: RotR skirmish, coal power plant **and barracks** rendering |
| `GeneralsZH-v0.11-debugkey.apk` (local, unpublished) | `debug.keystore` | the same build, kept so the S24 can be updated in place |

### Security (v0.12)

MobSF findings cleared, verified against the merged manifest in the finished
APK: `GameActivity` is no longer exported; `LauncherActivity` carries an
explicit `taskAffinity=""` against StrandHogg 2.0; `GameActivity` moved from
`singleInstance` to `singleTop`; and `ProfileInstallReceiver` (arriving
transitively via AndroidX, exported under `permission.DUMP`) is removed at
merge time.

Dropping `singleInstance` needed checking, since it was what guaranteed a
single engine instance. That no longer depends on the flag: the launcher starts
the game with no `NEW_TASK` and both activities carry `taskAffinity=""`, so the
game sits on top of the launcher in one task. Verified on device — play, exit,
play again leaves one task, one `GameActivity`, one process.

Left as-is deliberately: cleartext traffic to `gen.insave.ovh`. The mod host has
no TLS listener, the exemption is scoped to that one domain rather than the
whole app, and downloads are checked against their published MD5.

**"This app may contain hardcoded secrets" is a false positive** (checked
16/08/2026). The GameSpy per-product keys are in the tree -- `h5T2f6`
(ccgenerals), `D6s9k3` (ccgenzh), `g3T9s2` (ccgeneralsb) in `PeerThread.cpp`
and `PersistentStorageThread.cpp` -- but every one of those literals sits
inside a comment block. The live code assembles the key a character at a time
(`secretKey[0]='D'; secretKey[1]='6'; ...`), which is EA's own obfuscation, so
no contiguous key string exists in the binary: grepping the shipped
`libmain.so` finds none of the three. What the scanner sees is the GameSpy SDK
symbol name `gcd_secret_key`, plus framework strings in the dex
(`VISIBILITY_SECRET`, `RESUME_TOKEN`, and `; password: ` from
`AccessibilityNodeInfo.toString()`). The game name `ccgenzh` is in the binary
and is not a secret. Worth noting these keys authenticate to GameSpy, shut down
in 2014, and are public in the upstream EA source regardless.

Still uninvestigated: the MD5 hash finding and external storage.

**Only the release-key APK is published**, so a device whose install came from a
debug-key build cannot be upgraded in place — `install -r` fails on signature
mismatch and the only way past is an uninstall, which costs GameData. The
unpublished debug-key APK exists for exactly that case; keep building one
whenever the S24 needs updating.

Over v0.10: DXVK 1.9.2b everywhere. v0.10 carried the DXT3 decode fix (the black
models) and the `-noshadowvolumes` fix. Diagnostic probes are compiled out --
confirmed by grepping the shipped APK for the `GX-*` log tags before release.

`ab-v09engine.apk` in the repo root is not a release: it is the A/B artifact,
v0.9's engine libraries inside the v0.10 shell.
