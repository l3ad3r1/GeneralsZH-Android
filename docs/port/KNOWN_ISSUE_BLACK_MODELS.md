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

### Second round, 07/08/2026 — everything the draw call is given

All of the following were read **at draw time, on device**, from the probe in
`DX8TextureCategoryClass::Render()`, comparing `RBPWRPLNT.COALPLANT` (black)
against `RBCOMBNKR.STRUCTURE01` (renders, same faction, same frame, adjacent on
the map) and against `RBPWRPLNT.FENCE` (renders, **same object**):

| Theory | How it was eliminated |
|---|---|
| No texture bound | `0 UNTEXTURED` across every run once a black building was on screen. |
| Texture bound but no D3D surface | Added a `NOD3D` check on `Peek_D3D_Base_Texture()` — the thing `Apply()` actually binds, and not the same as a non-null `TextureClass`. **`0 NOD3D`.** The black meshes carry `d3d=0xb400…` `init=1` `missing=0` `512x512` with the right format. |
| The .dds is corrupt or black | Decoded `RBCPwrPlnt1.dds` offline: an intact 512×512 DXT3 brick-and-signage atlas, mean RGB (47,41,38) — *brighter* than `RBCmdBnkr1.dds` (23,22,20), which renders. |
| A duplicate texture from another archive wins | Indexed all 17 RotR `.gib`s and all 31 base `.big`s: exactly **one** `RBCPwrPlnt1.dds` and one `RBCPwrPlnt2.dds` exist. No override, no shadowing. |
| Texture format / size / mip count | `rbradar.dds` is DXT3 512×512 mips=10 on the *working* bunker; `rbcpwrplnt2.dds` is DXT1 512×512 mips=10 like `rbcmdbnkr1/2/3.dds`. Both shapes render elsewhere. |
| HSV shift (team recolour) | `hsv=(0.00,0.00,0.00)` on black and working meshes alike. |
| Vertex material | Identical at draw time: `lit=1 amb=(1,1,1) dif=(1,1,1) emi=(0,0,0) spc=(0.9,0.9,0.9) op=1.00 shin=0.1`, colour sources `0/0/0` (material, not vertex array) — the same values the bunker draws with. |
| Vertex normals | Parsed both `.w3d`s: every mesh in both models has a full normal array, **zero degenerate normals, all unit length**. |
| Prelit / vertex-channel differences | Mesh header attributes match: `attr=0x0 geom=0x0 prelit=0x0 sort=0 nmat=1 vchan=LOC\|NRM` on black and working meshes, with the same chunk list. |
| Per-object light environment | `RBPWRPLNT` gets `lenv=1 amb=(0.50,0.40,0.30) l0=(0.90,0.70,0.60)` — a perfectly serviceable lighting environment, and **the crane and the Mishka carry the byte-identical one and render normally**. |
| UV source / texture mapper | `uvsrc=0/0 mapper=-1/-1` on black and working meshes alike. |
| Shader bits | `0x9441b` is the black `COALPLANT` *and* the working `RBCOMBNKR.STRUCTURE01`. Decoded: `DEPTHCOMPARE=3 DEPTHMASK=1 COLORMASK=1 DSTBLEND=0 PRIGRADIENT=1 SRCBLEND=1 TEXTURING=1 CULLMODE=1`. Identical. |

The result of that round is worth stating plainly, because it is what makes the
bug strange: **every input to the draw call is identical between a batch that
renders and a batch that draws black, including two batches of the same object
in the same frame.** The fence around the plant draws its chain-link texture at
full brightness while the plant body two metres away is pure black.

### The bisection — it is not the texture, it is the geometry

With no property left to compare, the pipe was cut in half by experiment
instead. Two texture swaps, in one build, in `DX8TextureCategoryClass::Render()`:

1. bind the bunker's `rbcmdbnkr2.tga` — known to render — onto the black
   `RBPWRPLNT.COALPLANT`;
2. bind the plant's `rbcpwrplnt1.tga` — the suspect — onto the working
   `RBCOMBNKR.STRUCTURE01`.

The second is the control, and it is what makes the first mean anything: without
it, "still black" cannot be told apart from "the swap never happened".

**Result: the swap works, and the textures are innocent.**

- The bunker visibly changed — it drew the plant's pink-striped panels and
  signage instead of its own — and it stayed **fully lit and bright**. So
  `rbcpwrplnt1.dds` samples and shades perfectly well; it just needs to be on
  different geometry.
- The plant stayed **pure black** while bound to a texture that was rendering
  correctly on the building next to it in the same frame.

That flips the investigation. Ten months of this have been spent on textures,
materials and shaders; the failing half is the other one. The texture, the
material, the shader and the light environment are all fine, and something about
the **vertex data these particular meshes present to the draw** is not.

### What to probe next

The mesh's own `.w3d` is clean — positions produce a correct silhouette on
screen, and every normal in the file is unit length — so the corruption, if that
is what it is, happens between the file and the GPU. Read back what actually
reaches the shared vertex buffer for one black mesh and one working mesh:
`DX8TextureCategoryClass::Add_Mesh()` and `Vertex_Split_Table` are where the
data is written, and the FVF for that batch says what the layout should be.
Compare, per vertex, the normal, the UV and the diffuse against the values in
the `.w3d`. Note that these meshes declare `vchan=LOC|NRM` — **no colour
channel** — so whatever ends up in the vertex diffuse comes from the filler
rather than the asset; if `D3DRS_COLORVERTEX` is on and the material's colour
source resolves to `COLOR1`, a zero diffuse there shades exactly this black.

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

### Shadow volumes: ruled out, and a real bug found on the way

ShockWave was checked as the control, since it has never shown this. It is not
doing anything different:

| | RotR coal plant (black) | ShockWave power plant (fine) | RotR supply centre (fine) |
|---|---|---|---|
| `Shadow` | `SHADOW_VOLUME` | `SHADOW_VOLUME` | `SHADOW_VOLUME` |
| Draw module | one `W3DModelDraw`, whole model animated, `LOOP` | same | same |

So there is nothing in ShockWave's setup to copy across. Chasing it did turn up
a separate, real defect though: **`-noshadowvolumes` never worked in a release
build.** Its entry in `paramsForEngineInit` sat inside `#if defined(RTS_DEBUG)`
(`CommandLine.cpp`), so in every shipped APK the launcher's "Disable shadow
volumes" switch arrived in argv, matched nothing, and was skipped. Measured: the
flag is present as `argv[8]`, `parseNoShadows` never runs, and
`m_useShadowVolumes` still reads 1 inside
`W3DVolumetricShadowManager::renderShadows()`. Two later assignments would have
undone it anyway -- `GameLOD::setStaticLODLevel()` and the `OptionPreferences`
path both write the flag back after the command line is parsed -- so the fix is
the move out of the debug block plus an `m_shadowsForcedOff` latch they honour.

With the switch actually working (`useShadowVolumes=0`, `forcedOff=1`, an empty
shadow list, and every ground shadow gone from the frame) **the buildings are
still black.** Stencil shadow volumes are therefore not the cause -- and the
note elsewhere crediting `-noshadowvolumes` with fixing "black terrain" cannot
be right either, because the flag was inert when that was written.


### The stage is isolated: it is the texture stage, not lighting

Four overrides on `RBPWRPLNT.COALPLANT`, each with `RBCOMBNKR.STRUCTURE01` as a
control so a null result cannot be confused with an override that never ran:

| # | Override on the black mesh | Result |
|---|---|---|
| A | bind a known-good texture (`rbcmdbnkr2.tga`), lighting untouched | still black |
| B | force material emissive to white, its own texture | still black |
| C | **both** — good texture *and* emissive white | **renders**, full detail |
| D | bind **no** texture, lighting untouched | **renders**, correctly lit |

Taken one at a time A and B are each ambiguous, which is why both were run. Read
together they are not:

- **D settles lighting.** With the texture stage out of the picture the plant is
  lit exactly like the bunker beside it -- correct form, correct shading. So the
  light environment, the normals, the material and the transform all work, and
  every "maybe it is the lighting" theory is finished. (Note this is also the
  second time the light environment has come back clean: the crane carries a
  byte-identical one and always rendered.)
- **C settles the geometry.** The mesh rasterises with full detail. Nothing is
  painted over it -- the overdraw theory is finished too.
- So the black is produced where the texture is combined with the lit vertex
  colour. Both terms are individually fine -- D proves the vertex colour is
  non-zero, and the same `.dds` draws bright on the bunker -- yet their product
  for this batch is zero.

### What to probe next

The texture-blend stage for this batch, read at the point it actually reaches
D3D. `ShaderClass::Apply()` sets the stage states during
`Apply_Render_State_Changes()`, which runs *after* the hook in
`DX8TextureCategoryClass::Render()` -- so the existing probe cannot see them and
a new one has to sit later. Log `D3DTSS_COLOROP`, `COLORARG1`, `COLORARG2`,
`ALPHAOP` and `TEXCOORDINDEX` for stages 0 and 1, plus `D3DRS_TEXTUREFACTOR`,
for the plant and the bunker. A `COLORARG2` resolving to something zero
(`TFACTOR` with a zero factor, or `SPECULAR`) multiplies a perfectly good
texture by nothing, which is exactly the signature above.


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
