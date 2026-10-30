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
   `d3d9.dll`.
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
- Prebuilt terrain LOD packs with broken LOD meshes show stretched walls of
  ground at the edge of the loaded area. Remove the pack, or generate terrain
  LOD for your own load order with xLODGen.
- Mods that replace or patch the renderer (ENB, ReShade, shader replacers, New
  Vegas Reloaded and others) are not supported. `docs/INCOMPATIBLE_MODS.md`
  lists the known ones; the wrapper names any it recognises in its in-game
  notice and log.

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
