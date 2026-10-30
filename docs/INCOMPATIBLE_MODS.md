# Mods incompatible with the FNV Remix wrapper

These mods overlap features the wrapper provides, or replace the renderer.
The wrapper has no special handling for them. It names the ones it can
recognise in the in-game problem notice, `rtx-remix\logs\session.json` and
`loadorder.txt`, from the table in `src/comp/game/fault_report.cpp`:
`NewVegasReloaded.dll`, `NewVegasRTXHelper.dll`, `RealTimeMenus.dll`,
`MobilePipBoyLight.dll`, `Fallout Shader Loader.dll`, and any DLL whose
product name is ReShade. B42 Optics has no file of its own to recognise (it
is ESP-less and runs on JIP LN NVSE). The notice also names a code site
another module patched. Each is expected to fail gracefully (a feature stops
working, never a hang or a black frame); the symptom column comes from
in-game tests where there were any.

| Mod | Why | Symptom with the wrapper |
|---|---|---|
| Real-Time Menus (WallSoGB) | Draws the live world behind the Pip-Boy, terminals and barter / container / dialogue menus. The wrapper draws the live world behind the Pip-Boy itself ([MenuBackground]) and relies on the engine's frozen background for the other menus, and it reads the menu-mode state that mod changes. The engine monitor checks the menu-background capture code (feature `menu-bg`); when another module has patched it, the wrapper turns its live background off with a `[MenuBg]` line and leaves the menus to that module. | untested |
| Mobile Pip-Boy Light (WallSoGB) | Moves the Pip-Boy light to the wrist screen; the wrapper places the Pip-Boy light itself. | untested |
| Shader Loader, shader replacers (for example Vanilla Plus shaders) and other mods that change how the game renders (ENB, ReShade, lighting / shadow / AO / reflection / parallax / tonemap shader mods) | Remix replaces the game's renderer: lighting, shadows, reflections and tonemapping are path traced, so these mods have nothing to change, and replaced shaders can hide what the wrapper reads from the engine to route draws. The way to change the look is a Remix mod (materials, meshes, lights made for the renderer). | not supported |
| New Vegas Reloaded / TESReloaded, NewVegasRTXHelper | Replace or patch the same render paths; not meaningful together with Remix. New Vegas Reloaded also resolves depth through NvAPI (see below). | crash at startup in 1.0.3 and earlier |
| Depth Resolve - NVSE (`DepthResolve.dll`), Better Flashlight NVSE (`BetterFlashlightNVSE.dll`) | At renderer start they check for the `RESZ` depth format. Remix doesn't offer it, so they fall back to NVIDIA's NvAPI and register INTZ depth textures with it. NvAPI then creates its private `NVCS` texture on the Remix device, which the bridge can't create. Better Flashlight carries New Vegas Reloaded's depth code. This runs whatever their INI settings say: the DLL has to be removed. The bridge refuses the texture instead of crashing, so their depth-based effects (blur, depth of field, flashlight effects) do nothing. | effects missing; crash at startup in 1.0.3 and earlier |
| Improved Console (`improved_console.dll`) | At startup, the game's main thread recurses through Improved Console and xNVSE's script expression parser (`ExpressionParser::ParseLambda` → `FormatString`) until the stack runs out. The wrapper isn't on the crashing stack. Removing the DLL fixes it; `nvse_console_autocomplete.dll` and `nvse_console_clipboard.dll` run fine. | crash at startup (`EXCEPTION_STACK_OVERFLOW` in `nvse_1_4.dll`) |
| Mods disabled in a working Nuclear Sunset (TTW) list: Real Time Reflections (RTR) and its patches, Riven NVR, the NVR-only shader packs (Anti Aliasing Plus, Sharpening Plus, Uranium Haze, Hikikomori), Cloud Upgrade NVSE, Moonlight NVSE, LOD Flicker Fix NVSE, FlashlightNVSE, Atmospheric Rain Drops (overlay), weather and sky mods (Desert Natural Weathers, Supreme Weather, Neutral Weathers, Decent Horizons, Accurate NASA Stars) | Most are shader or NVR add-ons (see the rows above). The others change the sky, weather, LOD or the flashlight. A 1,762-mod list works with all of these off and broke with them on; which mod caused which problem wasn't narrowed down. | broke the game (symptoms not recorded) |
| Some prebuilt terrain LOD packs: Nuclear Sunset's `NS Z LOD Terrian M 00.bsa` (with its `.override` file), the VNV LOD Guide plugins merge | The stretched walls are vertices in the LOD meshes themselves (source Z down to -14098), not the wrapper's copy of the game's LOD sink. Terrain LOD generated with xLODGen for the player's own load order has no walls. The README setup checklist tells players to remove the pack or regenerate. | stretched walls of ground at the edges of the loaded area; removing the pack fixes it |
| B42 Optics and other picture-in-picture (render-to-texture) scope mods | The magnified view is a second camera that JIP LN NVSE's `ProjectExtraCamera` renders into a render-target texture (the engine's rendered-texture call) and projects onto a lens mesh in the first-person weapon. Remix path traces only the main camera, and the Remix API cannot take a render target as a material: the lens draw is refused (`unlit refused` / `not drawn`) and the scope view comes out empty. The extra camera's own render is rasterised, and the passes of statics the wrapper draws through the API are not in it. B42 Optics also removes the vanilla scope model of every weapon it configures and wants HDR on, which the mod's game settings turn off. A real fix needs a render-to-texture camera in the runtime, not a wrapper workaround. Vanilla scope overlays work (the UI boundary starts at the HUD's 3D render). | static analysis only (lens refused, empty scope view expected) |

Untested, possibly affected:

| Mod | Concern |
|---|---|
| Viewmodel Shading Fix (WallSoGB) | Shifts player-attached lights mid-frame; may displace the third-person Pip-Boy light. |
