# Draw classification

The wrapper decides what each draw is (land, far LOD, water, grass, leaves,
particles, a FaceGen head, a lighting-only pass, ...) from what the engine is
rendering, not from the bound D3D shaders. Shader packages differ (a shader
loader's replacements, a PS or VS compiled without a constant table, SM2
builds), but the engine's own pass and shader do not. The code is in
`src/comp/game/engine_class.*`.

## What the engine tells the wrapper

The render-pass dispatcher `0xB994F0` (hooked by `scene_graph`) draws one
`BSRenderPass` at a time:

| Field | Meaning |
|---|---|
| `pass+0x00` | the `NiGeometry` |
| `pass+0x04` | the pass type: the engine's `BSSM_*` pass enum, 1..0x2F5 (0x2F6 is image space) |
| `NiGeometry+0xC0` | the `BSShader` rendering it (set when the geometry attaches) |
| `BSShader+0x68` | that shader's class id (1 ShadowLight, 2 Hair, 4 Skin, 6 TallGrass, 7 DistantLOD, 0xD Water, 0x16 NoLighting, ...) |
| `NiGeometry+0xBC` | the skin instance (skinned geometry) |

The hook records the geometry and the pass type for the length of the call;
every D3D draw inside it reads them. The pass names come from
`BSShaderManager_GetPassName` (`0xB4F9D0`); `engine_class.cpp` groups the 757
values into kinds: lit (binds the base map on s0), lighting-only (s0 is the
normal map), land, land LOD, grass, leaves, distant trees, NoLighting
(textured, untextured, particle systems, SUBTEX particles), sky, water,
LOD water, water point-light passes, depth / shadow passes, decals, effects,
tiles. Two flags come from the names too: `Fg` (the FaceGen maps are bound)
and `PREMULT_ALPHA`.

## Which route asks what

| Route | Engine answer | Fallback when the engine has none |
|---|---|---|
| Expanders: grass, leaves, distant trees, SUBTEX particles | pass kind; read with the engine's registers for the family | VS constant names |
| Distant objects (DistLod) | shader class 7 | VS constant names |
| Unlit FX with a UV transform | NoLighting pass, VS declares the transform | VS constant names |
| Base pass / lighting-only pass (no-diffuse passthrough, skinned light passes) | pass kind | PS declares a BaseMap / DiffuseMap / TexMap |
| Water: near, LOD, point-light pass | WATER*, WATER_LOD*, *_SPECULAR_LIGHTING | PS water constants / samplers |
| Land | LAND* pass (layer count still from the PS's BaseMap[N]) | PS land layout |
| Near / far land LOD | ADT_Mn, ADT_MnShp, LANDLO_A (NORMAL in the declaration = near) | PS LOD samplers |
| FaceGen head | `Fg` pass, or SkinShader with the property's FaceGen flag | PS FaceGen samplers |
| Premultiplied alpha (flames) | PREMULT_ALPHA pass | PS that declares exactly DiffuseMap + MaterialColor |

The engine registers, identical in all 16 shader packages: grass MVP c9,
ScaleMask c2, AlphaParam c5, InstanceData c20; distant objects MVP c0,
EyePos c9, AlphaParam c12, InstanceData c20; distant trees c0 / c9; leaves
c0, BillboardRight c13, BillboardUp c14, LeafBase c34; NoLighting c0,
TexCoordTranform c4, SubTexOffsets c15; water ShallowColor c2, DeepColor c3
(c5 / c6 in the SM2 package 002).

A PS without a constant table reads as its pass's vanilla layout (albedo s0,
normal s1, FaceGen s2 / s3, water and NoLighting constants), so it still
routes and binds its albedo.

## Draws with no engine answer

Draws outside the dispatcher: the volumetric fog (`0xB651E0`), the
immediate-mode accumulator (`0xB63320`), and anything not drawn by a
`BSShader` (clears, readbacks). Unnamed pass types (0x158-0x159,
0x179-0x17E, the fog's 0x257-0x25B) count as no answer too. These use the
shader fallback.

## Checking it

Every `[RemixApi] StatsIntervalFrames` frames, `remix-comp.log` gets an
`[EngineClass]` status line: per route, the decisions per frame made by the
engine and by the shader fallback, and the disagreements (`differ a+b`:
engine yes / shader no, engine no / shader yes). The first disagreement and
the first positive fallback of each (route, pass type, VS, PS) are logged
once with both answers. In vanilla FNV the differ counts should be zero; a
replaced or stripped shader shows up as a difference the engine decided.
