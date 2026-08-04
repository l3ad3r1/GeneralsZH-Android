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

---

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
