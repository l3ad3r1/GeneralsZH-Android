# Known issue: a few Rise of the Reds buildings render black

**Status:** open. Affects Rise of the Reds only. Vanilla Zero Hour and vanilla
Generals are unaffected.

**Impact:** two structures observed so far (the Russian coal power plant and the
barracks) draw as solid black silhouettes. Everything else in the mod — units,
command centre, menus, UI, terrain — renders correctly, and the mod is playable.

---

## What it looks like

The affected model draws pure black while **casting a correct shadow**, and a
neighbouring building of the same faction, in the same frame, under the same
lighting, renders in full detail. So geometry, transforms, shadow casting and
scene lighting are all working for that mesh. Only its surface shading is black.

---

## Do not confuse this with the shadow-volume bug

Most reports of "black terrain" or "black models" on this port were a different
fault: stencil shadow volumes drew as opaque black geometry through DXVK on Mali,
covering whole scenes. That is fixed — the launcher passes `-noshadowvolumes` by
default. If you are seeing large black blobs rather than a couple of specific
buildings, check that toggle first.

---

## Ruled out, by measurement

Each of these was tested on device, not reasoned about. Recording them so nobody
repeats the work:

| Theory | How it was eliminated |
|---|---|
| Missing textures | The missing-texture placeholder is **magenta** (`0x7FFF00FF`, `missingtexture.cpp`), not black. A missing texture would be obvious and pink. |
| Texture load failure | Instrumented `TextureLoadTaskClass::Load()` to log every texture with its path, format and result. **59/59 loaded successfully**, zero failures, formats 2 and 3 only. |
| Terrain rendering path | Vanilla Zero Hour skirmish renders every building, vehicle, fence and the ground correctly. |
| Stencil shadow volumes | Disabling them fixed the large black areas; these specific models stayed black. |
| Terrain LOD selection | Instrumented `BaseHeightMapRenderObjClass::adjustTerrainLOD()`; it never fires. The full `HeightMapRenderObjClass` is already in use. |
| `.gib` archives not loading | The mod's own menu art, faction UI and units all render — those assets live inside the `.gib` files. |
| Mods in general | **ShockWave shows no black models at all** — same engine, same failed shaders, full skirmish. So this is not a generic mod problem. |
| The model's material setup | Extracted `RBPwrPlnt.W3D` and `RBBarracks.W3D` (black) and `RBWarfct.W3D` / `RBSuplyCntr.W3D` (fine) from `!Rotr_W3D.gib` and compared: **identical** `W3dShaderStruct` combinations. |
| Texture format | Their DDS files are the same formats and sizes. `RBWarf.dds` (works) is the same 512×512 DXT3 as `RBCPwrPlnt1.dds` (black). All present, all power-of-two, same mip counts. |
| Multi-pass / multi-texture materials | All four models are **1 material pass, 1 texture stage per mesh**. No bump-map or multi-pass path involved. |
| Black vertex colours | Ambient and diffuse are `(255,255,255)` on every vertex material in all four. Not a black-diffuse modulate. |

---

## The mesh probe, and how to run it

`DX8TextureCategoryClass::Render()` in `dx8renderer.cpp` carries an opt-in trace,
compiled out unless you build with `-DGX_TRACE_MESH` (add it to `cppFlags` in
`android/app/build.gradle`). It is the right hook because it is where a batch's
texture is actually bound, in the same loop that has `mesh->Get_Name()`, so it
can name meshes *and* say what texture they drew with:

```
GX-MESH: UNTEXTURED mesh='...' pass=N shader=0x...      <- draws black
GX-MESH: mesh='...' tex0='...' pass=N shader=0x...      <- once per distinct mesh
```

Two things to know before using it, both learned the hard way:

- **Raise the log buffer first: `adb logcat -G 64M`.** The default is 256 KiB and
  rotates a per-draw trace away within seconds. A full capture was lost to this.
- **The trace dedupes by mesh name and must stay that way.** An earlier version
  used a flat 1500-line cap, which the opening seconds of a match consume
  entirely — so a building placed a minute later, which is exactly the subject,
  never appeared. Two runs were wasted before that was obvious.

Partial result so far: **0 `UNTEXTURED` draws across roughly 1600 mesh draws**
over two runs. That does *not* yet count as eliminating untextured draws,
because neither run had a black building on screen — a fresh RotR skirmish
starts with only a command bunker, and the coal power plant has to be built
before the failure is visible. Finish that run before trusting the number.

## The assets are not the difference

It is tempting to conclude "RotR's models are unusual" because ShockWave is
clean. An offline comparison of the actual `.gib` contents says otherwise: on
every dimension that could plausibly produce a black surface — shader structs,
texture formats, pass and stage counts, vertex material colours — the black
models and the working ones from the same mod are the same. Whatever differs is
not visible in the model files.

One caveat on that comparison, stated because it matters: the affected models
were identified **by eye** from screenshots (smokestacks plus an in-game
"Construction Complete: Coal Power Plant" message → `RBPwrPlnt`; the other →
`RBBarracks`). That inference was never confirmed against what the renderer
actually drew. If it is wrong, the comparison above compared the wrong files and
proves nothing. Confirming the real model names — by logging the mesh name at
draw time for a black mesh — is the cheapest way to put that beyond doubt, and
should probably come before any more analysis.

## Most likely remaining cause

The material/shader path for those particular meshes. Three pixel shaders fail to
create on this device, on every install, because the shipped `shaders.big` /
`ShadersZH.big` are ~1 KB stubs:

```
Failed to create PIXEL shader: 'shaders\terrain.pso'     hr=0x8876086c
Failed to create PIXEL shader: 'shaders\roadnoise2.pso'  hr=0x8876086c
Failed to create PIXEL shader: 'shaders\monochrome.pso'  hr=0x8876086c
```

Vanilla assets never depend on those, so they degrade cleanly. Rise of the Reds
uses more elaborate multi-pass and multi-texture materials, and the hypothesis is
that one such material resolves to no valid pass here and draws black.

That is a hypothesis, not a finding. It has not been tested.

---

## How to continue

Instrument the material/shader application in `DX8Wrapper` to log, per mesh at
draw time, which shader and texture stages are requested and whether the setup
succeeded. Then compare a black RotR building against a working one in the same
frame — the difference in that log is the answer.

This is per-frame instrumentation, so cap the output or gate it behind a flag;
the texture-load trace above already needed a 4000-line cap.

Reproduce with: Rise of the Reds installed as a mod, any skirmish as Russia. The
coal power plant is black within the first minute of building it.
