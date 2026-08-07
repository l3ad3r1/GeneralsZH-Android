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

### Result of the finished run (07/08/2026, TCL NXTPAPER)

The run was completed with a black coal power plant on screen for its whole
duration. Two things came out of it, and together they move the investigation
off the material path entirely.

**1. The model names were guessed right.** The renderer names the black object
`RBPWRPLNT` at draw time, so the by-eye identification below — and the asset
comparison built on it — stands.

**2. Untextured draws are eliminated.** `0 UNTEXTURED` across the entire run,
*with the black building visible*. Every sub-mesh of the plant binds a real
texture on stage 0:

```
mesh='RBPWRPLNT.COALPLANT'     tex0='rbcpwrplnt1.tga'  pass=0 shader=0x9441b
mesh='RBPWRPLNT.WIRES'         tex0='rbcpwrplnt1.tga'  pass=0 shader=0x5441b
mesh='RBPWRPLNT.TWRSCFLD02'    tex0='rbcpwrplnt1.tga'  pass=0 shader=0x5441b
mesh='RBPWRPLNT.CPFOUNDATION'  tex0='rbcpwrplnt2.tga'  pass=0 shader=0x9441b
mesh='RBPWRPLNT.FENCE'         tex0='rbfence3.tga'     pass=0 shader=0xd441b
mesh='RBPWRPLNT.TESLA01'       tex0='tesla5.tga'       pass=0 shader=0x94433
```

**3. The shader is not the difference either.** `0x9441b` — the shader on the
black `COALPLANT` and `CPFOUNDATION` — is the ordinary shader of this scene:
134 of the 161 distinct meshes logged use it, including every mesh of the
command bunker standing next to the plant and rendering correctly.

**4. The split inside one object is by texture, not by mesh or shader.** On the
same object, in the same frame: `FENCE` (`rbfence3.tga`) draws its chain-link
texture, the `TESLA*` meshes draw their blue arcs — and everything carrying
`rbcpwrplnt1.tga` or `rbcpwrplnt2.tga` is solid black. See the screenshots in
this run: the perimeter fence and the tesla effect are plainly visible against a
black plant body.

So the surviving mechanism is narrow: **the texture is bound, is not null, and
still shades black.** That is a property of the texture object's contents (or of
what DXVK/Mali does with this particular surface), not of the mesh, the material,
the pass count or the shader — all of which now match meshes that render.

### What to probe next

Stop instrumenting the material path; it is clean. Instrument the *texture* for
`rbcpwrplnt1` / `rbcpwrplnt2`: at bind time log the D3D surface description
actually in hand — width, height, format, mip count, pool — and compare it with
`rbfence3`, which draws correctly from the same object and the same archive. If
those descriptions match, read back the first mip's top-left texels; a texture
whose description is right but whose pixels are zero means the upload, not the
load, is what failed. Note that `TextureLoadTaskClass::Load()` reporting success
for all 59 textures does not contradict this — it says the load path returned
OK, not that the surface reached the GPU with data.

The other black building (the barracks) has not been confirmed by name at draw
time. Doing so, and recording which texture *its* black meshes bind, is the
cheapest way to check whether "a specific pair of DDS files" or "a class of DDS
files" is the right description of the fault.

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

## Previously suspected, now ruled out

The material/shader path was the standing hypothesis: three pixel shaders fail to
create on this device, on every install, because the shipped `shaders.big` /
`ShadersZH.big` are ~1 KB stubs:

```
Failed to create PIXEL shader: 'shaders\terrain.pso'     hr=0x8876086c
Failed to create PIXEL shader: 'shaders\roadnoise2.pso'  hr=0x8876086c
Failed to create PIXEL shader: 'shaders\monochrome.pso'  hr=0x8876086c
```

The idea was that Rise of the Reds uses more elaborate multi-pass materials and
one of them resolves to no valid pass here. The finished mesh-probe run rules
that out: the black meshes are single-pass, single-stage, and carry the same
shader bits (`0x9441b`) as the 134 meshes in the scene that render correctly.

---

## How to continue

See "What to probe next" above: the material path is clean, so the next probe
belongs on the bound texture's surface description and contents, not on shader
or pass selection.

Reproduce with: Rise of the Reds installed as a mod, any skirmish as Russia. A
Russia start on `Desert Fury [GEN] (2)` already includes a coal power plant, so
the failure is on screen the moment the match loads — no need to build one.
