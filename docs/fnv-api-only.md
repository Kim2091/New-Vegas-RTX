# Traced draws go through the Remix API

Every ray-traced draw goes to Remix through the Remix API. D3D9 carries only
draws that are never traced: HUD / UI / menus (PASS_2D, POSITIONT), offscreen
passes (shadow maps, render targets that are not the back buffer), draws
without a view / projection, and draws the runtime is told to ignore
(PASS_NO_DIFFUSE_ROLE: Ignore-tagged, never traced). Draws with the game's sky
shader (SKIP_GAME_SKY) and the near land LOD (PASS_LOD_SHADER) are dropped:
the NUMOS sky draws the sky, and the far LOD and the terrain cover the land.

A few traced draws keep their FFP conversion. They are counted as `d3d9=` on
the status line:

- The Pip-Boy's own draws: first-person draws (the viewmodel projection)
  while the Pip-Boy is not closed (its model, screen FX and glass), and every
  draw while the interface can't be read (main menu, loading). Reason
  `pipboy`, `api::pipboy_draw`. The world behind the Pip-Boy (the
  menu-background capture, the closing animation's world) renders with the
  world projection and keeps its API routes, so opening or closing the
  Pip-Boy never swaps a world object between an API instance and a D3D9 one.
- Draws whose albedo is in a runtime list that the runtime reads only on D3D9
  draws (`api_rtxconf.hpp` `d3d9_only()`, reason = the list). These are light
  sources or screen elements, not geometry the API could carry:
  - `rtx.lightConverter`: the runtime spawns an effect light at the draw's
    geometry centroid, keyed by the D3D9 draw's material hash. An API material
    has the wrapper's own hash, so an API draw would never get its light.
  - `rtx.uiTextures`: screen UI.
  - `rtx.lightmapTextures`: skipped when the runtime picks a D3D9 draw's
    albedo stage.
  - `rtx.raytracedRenderTargetTextures`: keyed by a render target's
    descriptor hash, which the wrapper can't reproduce. With any entry in the
    list, every draw whose albedo is a render target goes D3D9 (the API can't
    upload a render target anyway).

  The renderer checks this before any API path, on every route that can carry
  such a draw: world draws (indexed and non-indexed: FFP_WORLD, FFP_BI,
  land-shaped objects, PASS_NO_NORMAL, DP_FFP), unlit FX
  (FFP_NOLIGHT_UVXFORM) and particles (FFP_EXPAND_PARTICLE). The scene graph
  statics refuse such a geometry as `rtxconf` before taking a build slot, so
  they never claim it. Skinned actors and foliage have no D3D9 route: a
  listed texture there is refused as `rtxconf` and not drawn.

Any other traced draw that no API path takes is logged once with its reason
and is not drawn.

## How to verify

`rtx-remix\logs\remix-comp.log`, every `[RemixApi] StatsIntervalFrames`
frames:

    [RemixApi] traced->D3D9 over 600 frames: d3d9=X/frame [route:reason=..] | dropped=Y/frame [route:reason=..] | budget: built=../frame (small .., reserved for the oldest ..) refused=../frame | queue=N oldest=Nf | waited>30f: N built, N still waiting (both should be ~0) maxWait=Nf

- `d3d9=` counts traced draws that went through D3D9: only the exceptions
  above. Outside the Pip-Boy it shows only the D3D9-only lists, e.g.
  `FFP_NOLIGHT_UVXFORM:rtx.lightConverter=6.0 FFP_WORLD:rtx.lightConverter=..`
  (and `DP_PASS:no declaration`). With the Pip-Boy open it also shows
  `...:pipboy` for the Pip-Boy's own draws (a few per frame), not the world
  behind it.
  Each (route, reason) pair is logged once as
  `traced->D3D9: <route> drawn through D3D9 (<reason>) -- an allowed exception (...)`,
  and each listed albedo once (at most 64) as
  `d3d9-only: albedo 0x... (WxH fmt N) is in rtx.lightConverter -- its draws keep the D3D9 route ...`.
  The startup line `statics: runtime options -- N tagged textures in N lists
  (N in D3D9-only lists...)` gives how many hashes those lists name.
- `[RemixApi] UnlitDebugLog` lines `unlit draw #N: ... [rtx.ignoreTextures]
  ... -> API (DrawInstance)` are expected: the instance carries the IGNORE
  category and the runtime skips it, as it skips the D3D9 draw. A
  lightConverter texture never reaches the unlit path.
- `budget:` is the build budget (below). `waited>30f` should stay near 0: it
  counts builds that waited more than 30 frames (granted in the window /
  still waiting). `queue` / `oldest` are the current waiters.
- `dropped=` counts traced draws no API path took (not drawn), by route
  (FFP_WORLD, FFP_BI, PASS_NO_NORMAL, SKINNED, FFP_EXPAND_GRASS, ...) and
  reason. Each (route, reason) pair also gets one
  `traced->D3D9: <route> NOT drawn (<reason>)` line the first time it happens.
- Per path: `capture:` (draw capture: sent, passes dropped, `refused=` per
  reason), `skinned:` (`refused=`), `foliage:` (`refused=`, `stale=`,
  `transient=`), `unlit:` / `particles:` (`refused=`), and land / far LOD /
  water `dropped=`. Draw capture also logs each refused (reason, PS) once:
  `capture: PS 0x... -> not drawn (<reason>)`.
- The `[Frame]` route histograms contain FFP_WORLD, FFP_BI, FFP_TERRAIN,
  FFP_EXPAND_*, FFP_NOLIGHT_UVXFORM, SKINNED, PASS_NO_NORMAL or
  DP_FFP only for the Pip-Boy's own draws. Everything else shows as
  API_STATIC, API_STATIC_SKIP_PASS, API_* and DROP_*.
- In the runtime's `remix-dxvk.log`, `[MaterialDump] route=d3d9-legacy` lines
  come only from the Pip-Boy's own draws and the D3D9-only lists' textures.

## Statics: scene membership, not draws

A rigid static is in the ray-traced scene while it is part of the loaded
scene graph and the game doesn't hide it. Whether the engine drew it this
frame (frustum, portal / room and fade culling) never adds or removes it.

- **Membership.** The engine monitor plugin walks a reference's 3D when the
  engine sets or attaches it (OnRefSet3D / OnRefAttach) and every reference
  of a cell that attaches. Each geometry of a non-actor reference goes to
  the wrapper through the member feed (`take_members`, drained at each
  Present). Nothing walks the scene graph per frame.
- **Seed.** Each member is classified, placed and recorded from the scene
  graph alone, under `SceneWalkBudgetUs` per frame: the mesh from its packed
  buffers (or, before the engine packed it, its CPU arrays), the material
  from its properties. The resolve then claims it like a drawn geometry.
- **Provisional until drawn.** The first time the engine draws a seeded
  claim, its passes reach the renderer once: the base pass's record (the
  proven draw-time material) confirms the claim or replaces it in the same
  tick. Its draws that frame are dropped; the instance draws it. The
  `scene members` line counts confirmed / replaced claims and which fields
  differed (`diffuse normal glow height gloss alphaTest blend wrap filter
  twoSided window layout decal glowTint material`): a field that keeps
  differing is a property read to fix.
- **Hidden by the game, read from engine data only.** A node app-culled in
  the reference's 3D (scripts, visibility controllers, BSRangeNode stages,
  light on / off nodes) or above it (cell nodes), the reference disabled
  (Disable detaches its 3D), the chain detached, or a switch / LOD ancestor
  showing another child (NiSwitchNode+0xB0). A member hidden so is held and
  looked at again; a claimed one is hidden by the hide poll and shown when
  that clears. The engine's per-frame culling never touches these: frustum,
  BSPortalGraph rooms and BSFadeNode fade only skip children in the cull
  pass, and ShadowSceneNode's temporary app-cull of the portal graph's
  shared objects is restored before the pass returns (FalloutNV.exe
  0xB5F9B0).
- **Rooms and portals.** A reference inside one room hangs under its
  BSMultiBoundRoom; one that spans rooms is detached from the cell tree and
  held only in the cell's BSPortalGraph (cell+0xD4, its shared array +0x3C).
  Membership comes from the reference walks, not from the cell tree, so
  both are members, wherever the camera is.
- **Left to the draw.** Land-shaped geometry (colour + two texcoords) is
  claimed from its draw, which tells land from a placed object.
- **Log.** `[RemixApi] scene members: N geometries of loaded references |
  traced T (provisional P, hidden by the game H) | not traced: waiting[...]
  engine draws decide[...] refused[...] | ...`. Every member that is not
  traced is counted under why.

## Build budget

New API objects (materials, meshes, land / water / far LOD builds) are
throttled per frame (`api_common.cpp` `take_build_budget(key, verts)`). A draw
that gets no slot is not drawn that frame (grass, leaves and held unlit FX
keep drawing their current copy) and asks again at its next draw. The game
issues its draws in the same order every frame, so a first-come budget would
starve the draws issued last (unlit FX, emissive signs) while a busy exterior
streams in. The budget is a fair queue instead:

- **Fair queue.** Every build has a key (material key / mesh key). A refused
  key becomes a waiter (first refusal frame, last ask). A waiter that stops
  asking for 2 frames (culled, unloaded) leaves the queue. At each frame
  start the oldest waiters get that frame's slots reserved. A reserved key's
  build is granted when its draw comes, and other builds get only the slots
  left after the reservations. A backlog drains at the budget's rate.
- **Small meshes.** A mesh build of at most `kSmallMeshVerts` (512) vertices
  (unlit FX, signs, bulbs, small statics, skinned parts) takes a slot from its
  own pool, `kSmallBuildsPerFrame` (64) per frame, fair-queued the same way.
  Material builds (texture uploads), land, water, far LOD and grass / leaves
  use MaxBuildsPerFrame, and also wait while the frame's 8 MB texture-upload
  soft budget is spent (a reservation stands until it is claimed).
- First-person parts never wait.

## Texture addressing

`[Addressing]` lines in the same log (at most 500 material / mesh lines, plus
20 `clamp changed` lines):

- `material path=<scene|capture|skinned|foliage|unlit|particles> albedo=0x...
  stage=N U=... V=... filter=... source=<engine clamp mode|stage sampler>
  class=<shade property class|(none)> mode=<raw clamp mode|(none)>`, for each
  material built with a non-repeating axis, or whose engine clamp mode
  overrode a stage sampler that said otherwise (`(stage sampler said U= V=)`).
  The engine mode comes from the geometry's shade property, read by exact
  class (PPLighting / Lighting30 +0xD8, NoLighting +0x6C). Anything else, and
  particles, use the albedo's sampler stage.
- `mesh path=... uv=[..]x[..] leaves [0,1] on a non-repeating axis: U|V|U+V|no`:
  a mesh built with such a material. `yes` on a clamped axis is where edge
  texels smear into stripes.
- `clamp changed #N geo=... mode A -> B -- unclaimed`: the hide poll found a
  claimed static whose shade property's mode no longer matches its material.
  It is rebuilt at its next draw.

## Unlit tint variants

The unlit path (`api_unlit.cpp`) folds MaterialColor into the vertex colours
and, with glow, into the material's glow tint, so every MaterialColor step of
a blinking bulb or chasing sign is another mesh (and material).

- Each geometry keeps up to 16 tint variants (key: geometry, material,
  quantised MaterialColor), least recently drawn evicted first, never one
  drawn this frame (the cap is soft then). After one animation cycle every
  step is cached and nothing is built.
- A variant not built yet (budget, retry, failed build) draws this frame with
  the geometry's nearest built variant (same texture first, then the closest
  tint). Only a geometry with no variant at all waits for its first build.
  Meshes up to 512 vertices take the small-mesh pool, and so does a material
  variant of a texture the geometry already drew (nothing to upload).
- A geometry that keeps evicting variants it drew within 120 frames (8 in 600
  frames: a continuous tint) is quantised more coarsely: 1/64, 1/16, 1/8, 1/4.
- A geometry whose set of drawn tints changes between frames is
  tint-animated: DrawInstance per frame for 300 frames after its last change,
  with no retained instance per step. Animated UVs are refreshed in place
  (MeshInfoRefreshGeometryEXT) on whichever variant draws.
- Status: the `unlit:` line ends with `variants: built +N reused=N/frame
  nearest=N/frame evicted +N coarsened +N (coarser now N) maxPerGeometry=N
  animated=N | undrawn>30f: N drawn late, N still waiting (both should be ~0)
  maxUndrawn=Nf`.
- Log: one `[RemixApi] unlit animated #N: geo ... | tex ... | route API unlit
  (...) | glow <source> | emissive intensity X | MaterialColor (...) |
  animation <MaterialColor (N variants, quantised 1/Q)> + <UV transform>` line
  per animated geometry (at most 128). `glow NONE: [RemixApi]
  UnlitEmissiveIntensity=0` means the ini (or Host Settings "Unlit FX emissive
  intensity") turned unlit glow off: non-additive unlit FX then don't glow at
  all. Additive ones glow only through the runtime's emissive-blend override
  (`rtx.emissiveBlendOverrideEmissiveIntensity`).

## Decals

Decal-tagged instances (`material::decal_of`: engine decal flags, a blended
draw with a depth bias, or an `rtx.decalTextures` list) are traced by the
runtime's decal route: unordered TLAS, blended onto the surface the primary
ray resolves, and only where the ray hits the decal's front face with an
opacity above 0.001. The API path matches the engine in two ways:

- **Opacity.** A decal's opacity is its albedo alpha. The vertex colour's
  alpha joins only when the shader property has Vertex_Alpha (flags1 bit 3);
  the engine ignores it otherwise (`material::shader_alpha`, capture and scene
  statics). The same rule applies to every static, glass included.
- **Facing.** A decal the game draws two-sided (cull NONE) gets a reversed
  copy of every triangle (`material::append_reversed`), so one of the two
  faces the camera, as in D3D9. Single-sided decals keep their triangles.

Verify: `[DecalRoute]` lines in `remix-comp.log`, one per decal instance
([Logging] Verbose only): path, object, albedo hash, retained / per frame,
categories, the alpha state the runtime gets and the route it will take, the
alpha source, the mesh's vertex alpha range, two-sided, winding vs vertex
normals. The runtime's `[DecalInstance]` lines in `remix-dxvk.log` show what
it did with each one.

## Game fog

The game's own fog, mist and haze cards are not traced: Remix's volumetrics
are the scene's fog. The engine monitor (`nvse_plugin/src/fog.cpp`) sets the
engine's app-culled flag on them when their reference's 3D loads, so the game
never renders them and no route sees a draw. The rule reads only the
geometry's render data, never file names, so mods' fog is covered however it
is named or placed. Hidden: geometry of a placed reference (REFR, any base
form type) that is

* unlit (BSShaderNoLightingProperty),
* alpha-over (NiAlphaProperty SrcAlpha / InvSrcAlpha),
* UV-animated (a texture transform controller on the geometry or one of its
  properties, found by NiRTTI class name), and
* not a particle system.

Z-write (shader flags2 bit 0) is logged as a supporting signal. Additive glow
(light shafts, neon, holograms), lit or static-UV cards, particle systems
(fire, torch, chimney smoke, explosions, sparks), actors and projectiles
stay. Alpha-over cards that stay reach the unlit path, which tags them
PARTICLE (translucent at every graphics preset).

Verify in `fnv_remix_nvse.log`: `[GameFog] hid N of M geometries of <model>
(base, type, ref): first <class> geo ..., shader, alpha, UV <controller>,
z-write` once per model (what vanished); `[GameFog] candidate <model>: ...
-- not <signal>: drawn` for blended, non-additive geometry that misses exactly
one signal (or is a particle system that has all three), once per model and
signal; a per-minute `[GameFog] hid +N geometries of +R references` line
while cells load. The geo pointers match the `geo 0x...` of `remix-comp.log`'s
`unlit animated` lines, which name the owning reference. `[GameFog]
Enabled=0` draws the game's fog again (A/B).

## Pip-Boy screen

The screen's contents (the NOLIGHT pass that blends the 1280x800 interface
render target onto the screen mesh, SRCALPHA / ONE) are tagged WorldUI and
emit at `[PipBoy] ScreenEmissive` (default 6, 0-16; `renderer.cpp`,
`is_pipboy_screen_contents`). The wrapper sets
`rtx.worldSpaceUiEmissiveIntensity` to 8 (runtime default 2.0); a runtime
without that option caps ScreenEmissive at 4.

The screen's opaque base passes (the 64x64 DXT1 glass, or any opaque NOLIGHT
pass on the contents' mesh) become dark CRT glass (`is_pipboy_screen_glass`):
stage 0 TEXTURE x TFACTOR at `[PipBoy] ScreenGlassBrightness` (default 0.1,
slight green) plus a protocol material (RS 149 diffuse slot 0, RS 169 gloss
`[PipBoy] ScreenGlassGloss`, default 200: roughness 0.32, 10-1000, live;
metallic 0), so sun and sky don't wash out the background behind the text.

The NOLIGHT declaration has no normals, so glass and contents draws bind a
copy of the screen mesh with smoothed normals: `vs_expand::bind_smooth_normals`
appends a FLOAT3 NORMAL to each vertex and to the game's declaration
(positions welded, area-weighted face normals oriented by winding, a 50-degree
crease so a bezel or side wall stays separate). It is built once per mesh and
logged once as `[PipBoy] screen glass: smooth normals for N verts (M faces)`
with the largest normal-to-face bend.

## Known gaps

- A traced draw that its path declines is absent for that frame: land quads
  and new statics waiting for a build slot while a new area streams in (a few
  frames at most; `waited>30f` on the budget counter says otherwise), a
  skinned actor whose palette doesn't cover its mesh, a particle system whose
  quads don't match the pattern, a skinned actor / grass / leaf with a
  texture in a D3D9-only list, a world draw from a DYNAMIC buffer (draw
  capture can't read one), an FVF draw.
- A D3D9-only albedo is decided by the texture's own image hash (mip 0,
  cached per texture address and creation generation). A texture the game
  rewrites in place after the check keeps the first answer until its address
  is reused.
- Draw capture draws what the scene graph doesn't take. A world draw of a
  NiGeometry under a placed non-actor reference whose mesh and placement held
  for 60 drawn frames becomes a retained instance kept while culled
  (Residency::Cell, the keep-culled TTL; gone when its NiGeometry dies, the
  scene graph claims it or its cell leaves; hidden while app-culled). The
  rest (movers, changing meshes, draws without a NiGeometry, actor parts,
  draw-state blends, the first-person weapon) stays one DrawInstance per
  frame and is not in the scene while the engine culls it. The `capture:`
  line splits world draws per frame into `retained=` and `perFrame=` (with
  why).
- A buffer write does not rebuild anything by itself. Draw capture keys
  meshes by the window's bytes (hashed when the write generation moves), and
  the scene statics compare a claimed window's bytes with its mesh at the next
  tick: same bytes keep the claim (`claimed windows written: same bytes +N` on
  the `scene refused` line), different bytes unclaim it, and different bytes
  twice in 300 frames refuse it as `streamed.rewritten` (retried after 600).
- A lit shader's additive / multiply pass is dropped as raster lighting. An
  effect mesh that uses a lit shader with an additive blend as its only pass
  would disappear (non-lit additive effects are drawn with their own blend
  state).
- Draw capture's "first pass of an object per frame" rule keys objects by the
  dispatcher's NiGeometry and the draw's index range, so each run of a
  segmented object-LOD block (one draw per run of the cells it shows) is its
  own instance. A geometry drawn twice in one frame over the same range with
  different content shows only the first. Only a geometry's first range of
  the frame can become a retained instance; later ranges go out per frame
  (`moreRanges` on the `capture:` line).
- Skinned vertex buffers the engine keeps rewriting are refreshed in place
  every time their write generation moves (one mesh refresh each).
- Grass / leaves draw per frame for 30 frames after every camera jump (and
  always with RetainedInstances / StableOrigin off), and a held batch shows
  its old instance set until the hold ends.
- With `[Skinning] Enabled=0` skinned actors are not drawn.
- API materials take the shade property's Texture Clamp Mode, where D3D9
  takes the bound stage's sampler state. The engine applies the mode only to
  the diffuse / normal stages a pass binds, and some passes hard-code clamp 0
  or 3. On such a pass the API material can wrap where D3D9 clamped, or the
  reverse. `[Addressing] ... (stage sampler said U= V=)` lists those
  materials.
