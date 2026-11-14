# New Vegas RTX

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/J3J3BCC3L)
[![Support me on Patreon](https://img.shields.io/badge/Patreon-Support%20me-F96854?logo=patreon&logoColor=white)](https://www.patreon.com/c/sparkles1)

New Vegas RTX makes Fallout: New Vegas render through
[RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), NVIDIA's path
tracer for classic DirectX 9 games. This repository holds the two parts that
run inside the game:

- **The wrapper** (`d3d9.dll`): a Direct3D 9 proxy that the game loads in place
  of the system `d3d9.dll`. It loads the RTX Remix bridge behind itself and
  decides, draw by draw, what Remix receives.
- **The engine monitor** (`fnv_remix_nvse.dll`): an xNVSE plugin that tells the
  wrapper what the engine is doing: loaded cells and references, camera cuts,
  loading screens, the world camera, menu state, lights, and whether another
  mod has patched the engine code the wrapper relies on.

## How it works

Fallout: New Vegas draws everything with vertex and pixel shaders, which RTX
Remix cannot interpret on its own. The wrapper reads what the engine is
rendering instead and sends it to Remix through the Remix API:

- **Every ray-traced draw goes through the Remix API.** World statics come from
  the engine's scene graph as retained instances. Land, distant LOD, water,
  grass and tree leaves, unlit effects, particles and skinned actors each have
  their own path. D3D9 carries only what is never traced: the HUD, menus,
  offscreen passes and the Pip-Boy's own draws. See `docs/fnv-api-only.md`.
- **Transforms come from the engine.** World, view and projection are read from
  the renderer, so no matrix has to be recovered from shader constants.
- **Actors are skinned by Remix.** Bone palettes are captured from the engine
  and sent with the mesh.
- **Lights come from the engine.** Placed lights, the scene's active light
  lists and the Pip-Boy light become Remix lights.
- **Sun, moon and weather follow the game.** The sun and moon follow the
  in-game time, and the runtime's sky follows the game's current weather and
  its transitions.
- **Texture packs keep working.** Normal maps with gloss in the alpha channel
  and `_p` height maps are passed to Remix as roughness and displacement.

The wrapper is built on xoxor4d's
[remix-comp-base](https://github.com/xoxor4d/remix-comp-base) framework.

## Requirements

- Fallout: New Vegas 1.4.0.525 (Steam or GOG). The engine hooks use fixed
  addresses and are switched off on any other executable.
- [xNVSE](https://github.com/xNVSE/NVSE) 6.4.8 or newer.
- The RTX Remix runtime and bridge from a New Vegas RTX release package, or
  built from the `runtime/` submodule (see "Building from source"). The wrapper
  uses runtime features that standard RTX Remix releases do not have, and needs
  the bridge client and server from the same build.
- An RTX Remix capable GPU and driver.

## Installation

1. Install the RTX Remix runtime and bridge into the game folder (next to
   `FalloutNV.exe`), then **rename Remix's `d3d9.dll` to `d3d9_remix.dll`**.
2. Copy this wrapper's `d3d9.dll`, `NewVegasRTX-Reporter.exe` and
   `remix-comp.ini` into the game folder.
3. Copy `fnv_remix_nvse.dll` and `fnv_remix_nvse.ini` into
   `Data\NVSE\Plugins\`. The plugin must come from the same build as
   `d3d9.dll`. Copy the build's `Data\Config\NewVegasRTX\Scopes\` folder into
   the game's `Data` folder too (the scope lens entries).
4. Start the game.

The game loads the wrapper as `d3d9.dll`, and the wrapper loads
`d3d9_remix.dll`. The log line `[Proxy] Loaded Remix bridge: d3d9_remix.dll`
confirms the chain. If the wrapper cannot load the bridge, it falls back to the
system `d3d9.dll` and Remix does not start.

Without the engine monitor the wrapper still draws, but the features fed by the
engine (scene-graph statics, the stable render origin, the live Pip-Boy
background, cut detection) are off, and `[NvseLink]` in the log says why.

With Mod Organizer 2, files that belong in the game folder can be installed
with Root Builder; `remix-comp.ini` is read from the folder that holds the
wrapper's `d3d9.dll`.

### Setup checklist

- At startup the wrapper turns off, in memory, the game's own graphics features
  that Remix replaces: shadow maps, HDR, multisampling, depth of field and the
  water reflection passes (`[GameSettings]` in `remix-comp.ini`). The game's
  INI files are not changed.
- Mods that replace or patch the renderer (ENB, ReShade, shader replacers, New
  Vegas Reloaded and others) are not supported. `docs/INCOMPATIBLE_MODS.md`
  lists the known ones; the wrapper names any it recognises in its in-game
  notice and log.
- With NVTF, set `bUseDefaultPoolForTextures = 1` under `[DirectX]` and
  `bModifyDirectXBehavior = 1` under `[Main]` in
  `Data\NVSE\Plugins\nvtf.ini`. With it off, every game texture keeps a copy
  in the game's 4 GB of memory, and large mod lists can run out. NVTF's notes
  say the setting can break Alt-Tab in exclusive fullscreen. The wrapper warns in
  its in-game notice and log when it is off, and does not change it.

## Configuration

`remix-comp.ini` in the game folder holds every setting, each with a comment
that explains it. Most settings can also be changed in game: open the Remix
menu (**Alt+X**) and use the **Host Settings** tab. A change made there applies
between frames and is written back to `remix-comp.ini`; settings marked
"(restart)" apply at the next launch.

Frequently used sections:

| Section | What it controls |
|---|---|
| `[Remix]` | Loading the Remix bridge (`Enabled`, `DLLName`) |
| `[Chain]` | Other DLLs or ASIs to load before or after the d3d9 chain |
| `[GameSettings]` | The game graphics settings applied at startup |
| `[RemixApi]` | The Remix API paths: statics, terrain, water, foliage, materials |
| `[TexturePacks]` | Height maps from texture packs (`_p`) |
| `[Lights]` | Engine lights: intensity, range, the Pip-Boy light |
| `[SunCycle]`, `[MoonCycle]`, `[Weather]` | Sun, moon and weather driving |
| `[Logging]`, `[SessionReport]` | Logs and the bug-report zip |

The engine monitor reads `fnv_remix_nvse.ini` next to its DLL.

**F4** opens the wrapper's developer overlay (draw routing state and
statistics).

### Path-traced scopes

With **Path-traced scopes** on (`[Scopes] PathTraced`, default on, in the
Alt+X menu), aiming a scoped weapon that has a lens entry keeps the world and
the weapon on screen: the gun is raised in its ironsights pose and the runtime
draws a path-traced lens in the scope itself, through which the world is
magnified. There is no 2D overlay, no hidden weapon, no scissored view and no
camera zoom. Weapons without an entry keep the game's own scopes.

- **Which weapons:** a gun whose scope the game would show (the Has Scope
  flag and a scope model, and when the scope comes from a weapon mod, that mod
  installed) and that has a lens entry for its installed mods, and the
  binoculars (below). A camera (a scope, no projectile, but film as ammo)
  keeps the game's overlay.
- **The lens:** placed from the entry on the first-person weapon node, so it
  moves with the gun. Its magnification is the entry's, or by default the zoom
  the game would have applied: `tan(fDefaultWorldFOV / 2) / tan(scopeFOV / 2)`,
  where `scopeFOV` is the weapon's sight FOV minus an installed zoom mod's
  value (whatever supplies the scope, as the game's own zoom does). Reflex
  sights keep that magnification too. First person only.
- **Zoom:** while the lens is up, the mouse wheel or the d-pad (up / down)
  zooms between the entry's `MinMagnification` and `MaxMagnification`, and the
  game sees neither. Without a range, telescopic lenses zoom from their
  default magnification up to three times it (`[Scopes] VariableZoomAll`,
  default on); otherwise the zoom is fixed. Each aim starts at the default
  magnification. Zoom steps click with the game's `UIMenuPrevNext` sound.
- **Vision modes:** night by default on scopes with night vision (flags2
  `0x1000`; the game's own night-vision image-space effect stays off for lens
  weapons), unless the entry's `VisionMode` says otherwise. `[Scopes]
  VisionKey` (default N) or `GamepadVisionButtons` (default d-pad right)
  cycles the weapon's allowed modes (the entry's `VisionModes`; without it
  none and its default mode) while aiming; with `VisionAllModes` it cycles
  none, night and thermal on every lens. Which weapon allows which modes:
  [docs/scope-vision-modes.md](docs/scope-vision-modes.md). Switching plays the game's
  `UIPipBoyLightOn` / `UIPipBoyLightOff` (`[Scopes] Sounds`).
  Thermal and `HighlightHostiles` read per-actor tags the wrapper puts on the
  skinned actors it sends: living actors are heat sources, hostile ones
  (in combat with the player, or hostile on the compass) are highlighted.
- **Crosshair:** the HUD crosshair is hidden while a lens is up; the lens
  draws its own reticle.
- **Point of aim** (`[Scopes] AimMode`): the game's first-person shot flies
  along the player's view, from the camera while a scope is aimed, not along
  the gun.
  - `realistic` (default): the image follows the swaying scope, and while a
    lens weapon is aimed through its lens the shot flies along the scope's
    axis from its objective instead, so the reticle marks where it goes. The
    game's spread still applies; NPCs, third person, VATS and other weapons
    are unchanged.
  - `view`: the lens looks along the view, where the game's own shot goes,
    while the eyepiece stays on the swaying scope.
- **Weapon motion** (`[WeaponMotion]`, default on): while aiming in first
  person the gun has a small motion of its own, and the lens, the eye point
  and the realistic shot move with it: a slow breathing sway (larger after
  running, on low action points, with crippled arms or low Agility), a lag
  behind turns, a walk / run bob and the recoil of each shot. Each has a
  strength; `Ironsights` also applies it without a scope. It fades in after
  the aim-in; the eye follows the motion with `AlignSmoothing` (default
  0.45 s), so the scope moves against the eye and settles back to center.
- **Lens at the hip** (`[Scopes] LensAlways`, default on): the held
  scope's lens is there whenever the gun is drawn in first person, with no
  menu and the Pip-Boy closed, at its magnification; from the hip the eye
  sees it off axis, as a real scope looks. Vision modes, highlighting, the
  rangefinder, peripheral blur, the zoom keys and the realistic shot start
  once aimed and lined up; the zoom you set stays while hip firing.
- **Aim-in:** the gun comes up as the game animates it, and the eye slides
  from the camera onto the scope's eye point as the scope's axis lines up
  with the view (from about 9 degrees off to about 1).
- **Parallax and glass:** the reticle is focused at a distance (`[Scopes]
  Parallax`; an entry's `ParallaxDistance`, else 100 m on telescopic scopes),
  and the glass shows glare and dirt (`GlassEffects`, `GlareStrength` 0.5,
  `DirtStrength` 0.2 unless an entry sets its own).
- **Night and thermal brightness:** with a vision mode on, the wheel with
  Shift (`BrightnessKey`), or the d-pad with the left shoulder
  (`GamepadBrightnessButtons`), changes the brightness from 0.25x to 4x in
  1.25x steps, kept per weapon for the session. The plain wheel still zooms.
- **Third person:** aiming a lens weapon in third person switches to first
  person for the aim and back after (`[Scopes] ForceFirstPerson`, default on).
- **Peripheral blur:** `[Scopes] PeripheralBlur` (0..1) blurs the view outside
  the lens while it is up (`rtx.lens.peripheralBlur`).
- **Aim alignment** (`[Scopes] AlignAim`, default on): while aiming, the
  first-person view sits at the entry's eye point (`EyepieceCenter` minus
  `EyeRelief` along `Axis`), so the eye looks down the scope. It uses the
  engine's own sighting-node path (`bTrueIronSights`): the wrapper points the
  player's sighting node at that point while the lens exists. The eye stays
  on the eye point as the game poses the gun (raising it, walking, turning)
  and follows the weapon motion's offset (`AlignSmoothing`, seconds, default
  0.45; 0 = locked), so sway and recoil move the scope against the eye.
  `AlignStrength` (0..1, default 1) blends from the weapon's ironsights pose
  to the eye point. `AimDebugLog=1` logs the alignment every frame for the
  first 2 s of each aim.
- **Needs:** a runtime whose bridge client supports
  `remixapi_CameraInfoLensEXT` with its accent, edge and night-tint fields
  (the log says `[Scopes] the bridge client forwards CameraInfoLensEXT ...` or
  `does NOT`),
  the engine monitor plugin (EditorIDs are read from the plugin files in its
  load order) and `[RemixApi] FallbackCamera=1`, since the lens rides on that
  camera. The wrapper sets `rtx.lens.enable` on the first lens.

#### Lens entries

One file per weapon, `Data\Config\NewVegasRTX\Scopes\<EditorID>.ini`, so a
mod adds or replaces a weapon's lens by shipping one file. The wrapper ships
presets for the vanilla and DLC scoped weapons. Values are in the
first-person weapon node's space:
`+X` toward the muzzle, `+Y` up, game units (about 1.43 cm).

```ini
[Lens]
EyepieceCenter = 1.16, 10.02, 0
Axis = 1, 0, 0
Up = 0, 1, 0
EyepieceRadius = 0.89
ObjectiveDistance = 24.6
ObjectiveRadius = 0.95
EyeRelief = 5.6
Reticle = duplex
```

`[Lens]` is the weapon's entry. `[Lens.<slots>]` is an entry for one
combination of installed weapon mods, written as the installed slot numbers
(`[Lens.2]`, `[Lens.13]`, `[Lens.123]`, `[Lens.0]` for none); its keys take
over `[Lens]`'s one by one. A key neither section gives has its default:

| Key | Meaning | Default |
|---|---|---|
| `EyepieceCenter` | x, y, z of the eyepiece disc's center | 0, 0, 0 |
| `Axis` | optical axis, from the eye toward the objective | 1, 0, 0 |
| `Up` | the reticle's up | 0, 1, 0 |
| `EyepieceRadius` | | 0.9 |
| `ObjectiveDistance` | from the eyepiece along the axis | 20 |
| `ObjectiveRadius` | | 1 |
| `EyeRelief` | the eye's distance behind the eyepiece | 5.6 |
| `Magnification` | 0 = the game's own zoom for the weapon | 0 |
| `FieldStop` | degrees | 18 |
| `Reticle` | `none`, `crosshair`, `duplex`, `dot`, `circledot`, `mildot`, `german`, `drop`, `stadia`, `frame`, `grid` or `dial` | `duplex` |
| `ReticleColor` | r, g, b | 0, 0, 0 |
| `ReticleRadiance` | 0 = dark; above 0 it glows | 0 |
| `ReticleLineWidth` | half-width, a fraction of the field stop radius | 0.004 |
| `Tint` | the glass's r, g, b | 0.92, 0.92, 0.92 |
| `Kind` | `telescopic` (magnified, exit pupil) or `reflex` (a window with the reticle at infinity: holographic and red-dot sights) | `telescopic` |
| `Aperture` | `circle` (radius `EyepieceRadius`), `rectangle` (`EyepieceRadius` half high) or `binocular` (two circles of radius `EyepieceRadius`) | `circle` |
| `ApertureAspect` | a rectangle's width over its height; for `binocular`, each circle's center from the axis in radii | 1 |
| `VisionMode` | `none`, `night` or `thermal` | night on scopes with night vision, else none |
| `VisionModes` | the modes the vision key cycles, in order, any of `none, night, thermal` (the starting `VisionMode` is added first if missing); `VisionAllModes=1` overrides it with all three | none and the default mode |
| `HighlightHostiles` | 1 = outline hostile actors in the lens | 0 |
| `MinMagnification` | the mouse wheel's lowest zoom; 0 = the default magnification | 0 |
| `MaxMagnification` | the mouse wheel's highest zoom; 0 = the default range (`VariableZoomAll`) or fixed | 0 |
| `ReticleAccentColor` | r, g, b of the second reticle color (mil-dots, center dots) | 0, 0, 0 |
| `ReticleAccentRadiance` | 0 = unlit ink; above 0 the accent glows | 0 |
| `EdgeGlowColor` | r, g, b of a glow ring at the field stop | 0, 0, 0 |
| `EdgeGlowStrength` | 0 = no ring | 0 |
| `EdgeShade` | 0..1, darkening toward the field stop | 0 |
| `EdgeTint` | r, g, b of a colored vignette toward the field stop; 0, 0, 0 = none | 0, 0, 0 |
| `NightTint` | r, g, b of the night-vision phosphor; 0, 0, 0 = the runtime's | 0, 0, 0 |
| `NightGain` | a multiplier on the night-vision gain; 0 = 1 | 0 |
| `Markings` | `labels` (the reticle style's scale numbers), `rangefinder` (a range readout in meters along the axis) or `labels,rangefinder`; `[Scopes] RangefinderAll` adds the rangefinder to every lens | none |
| `ReticlePlane` | `second` (the reticle keeps its size at any zoom) or `first` (it grows with the zoom from the default magnification) | `second` |
| `ReticleTexture` | a .dds image whose alpha is the reticle, relative to the game's `Data` folder (e.g. `textures\weapons\scope\...`) or absolute; it covers the field stop (-1..1 across, the aperture aspect wide on a rectangle). A missing file, another format or a non-ASCII path keeps `Reticle` | none |
| `MarkingColor` | r, g, b of the labels and the rangefinder; 0, 0, 0 = the accent's color. Not given: `[Scopes] MarkingColorR/G/B` with `MarkingRadiance` (red by default) | `[Scopes]` |
| `MarkingRadiance` | with `MarkingColor`: 0 = the accent's lit or ink look, above 0 lit | 0 |
| `MarkingOpacity` | 0..1, how much the markings cover; 0 = opaque | 0 |
| `ParallaxDistance` | meters the reticle is focused at; 0 = none | 100 on telescopic scopes, 0 on reflex sights |
| `GlareStrength` | 0..1, glare on the glass | `[Scopes] GlareStrength` |
| `DirtStrength` | 0..1, dirt on the glass | `[Scopes] DirtStrength` |
| `Grain` | `off` = no film grain on the lens (night vision and the glass keep their other effects); needs a runtime with the glass fields | `on` |
| `Node` | a node of the first-person weapon model, by name, whose space the values are in (for a part that moves on its own, as Red Glare's `##RGScope`); the model's root when not given or not found | the root |

**Scope editor:** the Alt+X menu's **Scope editor** section edits the
equipped scoped weapon's entry live (its **Editing** line names the weapon,
its installed mods and the section). Changing a value on a weapon without an
entry creates one. **Only for these weapon mods** sends the values to the
installed combination's section instead of `[Lens]`. **Save scope** writes the
file; **Reload scope files** reads every file again.

- **Engine hooks** (installed at startup when the setting is on; each one
  passes through for anything but a weapon with a lens entry):
  - the scope model attach (`0x77F2F0`): the scope model is not attached;
  - the HUD scoped state (`0x77F3C0`): stays off, so no overlay and no
    scissor;
  - `Actor::SetAiming`'s scope model test before the first-person cull
    (`0x8BB826`): the weapon is not hidden;
  - `SetAiming`'s ScopeFromMod test before the ironsights anim (`0x8BBB4A`)
    and the weapon's "first-person ironsights anims" test (`0x8B2BF0`): the
    ironsights pose, including for records that turn ironsights anims off;
  - `PlayerCharacter::UpdateFOV`'s aiming test before the ironsights zoom
    (`0x95E043`): no zoom;
  - the HUD reticle state's sighting-node test (`0x771B67`): the crosshair
    hides while a lens weapon is aimed in first person, as the engine hides
    it for a true-ironsights sighting node; the center reticle tile is also
    kept hidden every frame the lens is up;
  - the player update's ScopeHasNightVision test (`0x93F098`): no vanilla
    scope night-vision effect for lens weapons;
  - `OSInputGlobals::Poll` (`0xA23010`): the mouse wheel goes to the lens's
    zoom while it is up;
  - the game's `XInputGetState` import (`xinput1_3.dll` ordinal 2): the d-pad
    up / down and the vision buttons go to the lens while it is up;
  - the first-person camera setup's sit / sleep test before the sighting
    node (`0x874ECE`, `0x87565F`): with `AlignAim`, the eye point is used
    while a lens weapon is aimed in first person.
- **Binoculars:** a weapon with a scope model and neither a projectile nor
  ammo (the binoculars; a camera has film as ammo, so it is not one) needs no
  entry. While aimed in first person it shows one camera-attached lens on the
  view axis with the binocular aperture (two overlapping circles, each about
  42% of half the screen height, the view black outside them), magnified by
  the record's zoom, with no overlay, no zoom and no crosshair. Vision modes
  and the wheel zoom work as on scopes; the alignment does not apply.
- **Stays vanilla for now:** a weapon's scope effect shader (EFSD).
- A change of the setting applies at the next aim; whether the scope model
  is attached follows at the next equip.
- **Presets for other mods' weapons:** lens files in subfolders of
  `Data\Config\NewVegasRTX\Scopes\` win over the base folder; see
  [docs/scope-presets-for-mod-authors.md](docs/scope-presets-for-mod-authors.md)
  for the format, the folder rule and the in-game editor.

#### Compatibility

- Weapons without a lens file keep the game's own scope.
- A mod that gives a scoped weapon another first-person mesh falls back to the
  game's scope for it, unless it ships a lens file for that mesh (the
  presets' `Models` check).
- Weapon sway or inertia mods add to the wrapper's own weapon motion: turn
  `[WeaponMotion] Enabled` off with one.
- Picture-in-picture scope mods draw their own scope image and conflict with
  the lens: use one or the other.
- If another mod changes where shots come from, use `[Scopes] AimMode = view`.
- When another mod holds a piece of game code the wrapper hooks in a way it
  cannot chain, only the feature that needs it stays off, and the log says
  which (`Scopes:` lines at startup).

## Logs and bug reports

Logs are written to `rtx-remix\logs\` in the game folder:

- `remix-comp.log`: the wrapper. It starts with the system, GPU, driver and the
  DLLs loaded in the game, and writes a `[Session]` status line every 60
  seconds.
- `fnv_remix_nvse.log`: the engine monitor.
- `remix-dxvk.log`, `bridge32.log`, `bridge64.log`: the Remix runtime and
  bridge.

Each run is zipped to `rtx-remix\logs\archive\` with its logs, the load order,
the relevant config files and, after a crash, a crash report
(`[SessionReport]`). `NewVegasRTX-Reporter.exe` makes the zip after the game
has closed, however it closed, and after a crash or hang shows where the zip
is. It has no network access. The zip's text files replace the Windows user
name with `<user>`.

Press **F8** in game to mark a moment: it writes a `[Mark]` line to both logs
and saves a screenshot, so a report can point at where a problem shows.

## Making Remix mods

To package a Remix mod so that players can sort it in Mod Organizer 2 or Vortex, see
[Packaging Remix mods](docs/packaging-remix-mods.md).

## Building from source

The wrapper and plugin build from a plain clone:

```bat
git clone https://github.com/Kim2091/New-Vegas-RTX
```

The runtime and bridge are the `runtime/` submodule, which points to the
[`fnv/main` branch of Kim2091/dxvk-remix](https://github.com/Kim2091/dxvk-remix/tree/fnv/main)
at the commit this wrapper works with. To get them as well, clone with
`--recursive`, or run `git submodule update --init --recursive` in an existing
clone. See `runtime/README.md` for how to build them. They have their own
licences (`runtime/LICENSE` and `runtime/ThirdPartyLicenses.txt`).

Requirements for the wrapper and plugin:

- Visual Studio 2022 with the C++ desktop workload (the scripts use its x86
  toolset).
- The [DirectX SDK (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=6812).
  Its installer sets `DXSDK_DIR`. To use a copy elsewhere, set `DXSDK_DIR` to
  the folder that holds `Include\d3dx9.h` and `Lib\x86\d3dx9.lib`.

```bat
build.bat                       :: release d3d9.dll and NewVegasRTX-Reporter.exe
build.bat debug                 :: debug build
build.bat --release-tag 1.2.0   :: release build that reports "release 1.2.0" in its log (default: dev)
build.bat --tracy               :: link the Tracy profiler client

nvse_plugin\build_plugin.bat    :: release fnv_remix_nvse.dll
```

Output goes to `build\bin\<config>\`, with a copy of `remix-comp.ini` and
`fnv_remix_nvse.ini`. Ship `d3d9.dll` and `fnv_remix_nvse.dll` from the same
commit: the wrapper compares the plugin's build with its own.

The wrapper is C++ (`/std:c++latest`) with the static CRT. It links MinHook,
Dear ImGui and miniz from `deps\` and D3DX9 from the DirectX SDK. `tests\`
holds small standalone tests, each with its own `build_test.bat`.

## Project layout

```
src/comp/        Game code: engine hooks, Remix API paths, draw routing, session reports
src/shared/      Framework from remix-comp-base: config, module loader, FFP state, Remix API setup
src/shared/fnv_remix_plugin_api.h   The interface between the wrapper and the engine monitor
src/reporter/    NewVegasRTX-Reporter.exe
nvse_plugin/     The engine monitor (fnv_remix_nvse.dll)
runtime/         Submodule: the RTX Remix runtime and bridge (Kim2091/dxvk-remix, fnv/main)
deps/            Third-party libraries (see THIRD_PARTY_LICENSES.md)
docs/            Design notes and the list of incompatible mods
tests/           Standalone tests
remix-comp.ini   Default configuration
```

## Known limitations

- Only `FalloutNV.exe` 1.4.0.525 is supported.
- Post-process effects and screen-space overlays are rasterised, not traced.
- Picture-in-picture scopes (render-to-texture cameras) show an empty view.

## Credits

- [xoxor4d](https://github.com/xoxor4d): the `remix-comp-base` framework this
  wrapper is built on.
- Wall_SoGB, BlueAmulet and xoxor4d:
  [NewVegasRTXHelper](https://github.com/BlueAmulet/NewVegasRTXHelper), which
  first mapped many of the engine addresses and offsets the wrapper uses. The
  wrapper contains none of its code.
- Reverse engineering was done with
  [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering).
- NVIDIA: [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix).

## Licence

The wrapper and the engine monitor are released under the MIT licence (see
`LICENSE`). Third-party libraries under `deps\` keep their own licences; see
`THIRD_PARTY_LICENSES.md`.
