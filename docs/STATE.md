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

1. **Black models — Rise of the Reds only.** Two buildings (coal power plant,
   barracks) draw solid black while casting correct shadows. See
   `docs/port/KNOWN_ISSUE_BLACK_MODELS.md`. The `-DGX_TRACE_MESH` run is
   **done** (07/08/2026): the black object is confirmed to be `RBPWRPLNT`, every
   one of its meshes binds a real texture (`0 UNTEXTURED` in the whole run), and
   its shader `0x9441b` is the same one 134 correctly-rendered meshes in the
   scene use. Mesh, material, pass count and shader are therefore all
   eliminated. A second round then read **everything the draw call is given** at
   draw time and found it all identical to batches that render: the D3D texture
   is bound and initialised, the .dds is intact and unique across every archive,
   the vertex material, normals, mesh attributes, light environment, UV source
   and shader bits all match. The fence of the *same object* draws its texture
   at full brightness while the body is pure black. Bypassing `DX8Wrapper`'s
   redundant-state-set cache (`-DGX_NO_STATE_CACHE`, still in the tree, off)
   changed nothing either. A texture swap with a control then settled it:
   putting the plant's texture on the bunker draws the plant's bricks **bright**,
   and putting the bunker's texture on the plant leaves it **black**. So the
   texture is innocent and **the fault is in the vertex data**, not the material
   path. Next probe: read back the shared vertex buffer after
   `DX8TextureCategoryClass::Add_Mesh()` and compare normals/UVs/diffuse against
   the `.w3d` for one black and one working mesh. These meshes declare
   `vchan=LOC|NRM` — no colour channel — so the vertex diffuse comes from the
   filler, not the asset. **Update:** the failing stage is now isolated. With
   no texture bound the plant lights up correctly, so lighting, normals,
   material and transform are all fine; with a good texture *and* emissive
   white it renders in full detail, so nothing is drawn over it. The black is
   produced where the texture is combined with the lit vertex colour. Next:
   log the stage states (`D3DTSS_COLOROP`/`COLORARG1`/`COLORARG2`/
   `TEXCOORDINDEX`, `D3DRS_TEXTUREFACTOR`) *after*
   `Apply_Render_State_Changes()`, which is where they are actually set.
2. **Generals crashes on DXVK 2.6** (S24/Adreno) in
   `DxvkResourceAllocationPool::alloc()`, fault addr `0x2000000001`. Works fine
   on DXVK Native 1.9.2b (TCL/Mali). Zero Hour is fine on both. The
   operator new/delete version script IS correctly applied to both engines --
   checked, not assumed.
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
- **Shadow volumes draw as opaque black geometry** through DXVK on Mali. That
  was the cause of most "black terrain" reports. Fixed: launcher passes
  `-noshadowvolumes` by default.
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

## Devices

| Device | Serial | Keystore | Notes |
|---|---|---|---|
| TCL NXTPAPER | `987800005DB3824` | `my-release-key.jks` | Mali-G57, Vulkan 1.1, DXVK Native 1.9.2b |
| Galaxy S24 Ultra | `RZCY51R2A8D` | `debug.keystore` | Adreno 750, Vulkan 1.3, DXVK 2.6 |

Releases are at v0.8; `main` has since gained the shadow toggle, `.gib`/`.scb`
mod support, the Generals fixes, text fixes and the mod downloader. A v0.9 is
due.
