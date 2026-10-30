# Packaging Remix mods for New Vegas RTX

How to package a Remix mod so that players can install it and sort it in Mod
Organizer 2 or Vortex like any other mod.

A Remix mod replaces the game's materials, meshes and lights by name. It lives in
the game folder, in `rtx-remix\mods\<ModName>\`, not in `Data`. To learn how to
make one, see the
[RTX Remix Toolkit documentation](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-toolkit.html).

## The short version

Your archive holds your mod folder plus one small **ownership file** for each
asset your mod replaces:

```
rtx-remix\
  mods\
    MyMod\
      mod.usda
      (sublayers, textures, meshes...)
    owners\
      mat_0123456789ABCDEF     contains the line: MyMod
      mesh_FEDCBA9876543210    contains the line: MyMod
```

## Why ownership files matter

When two mods replace the same asset, both ship an ownership file with the same
path, each naming itself. To a mod manager, that's an ordinary file conflict, so
the player's mod order decides which copy stays, and the game follows that copy:

| Installed with | The winner |
|---|---|
| Mod Organizer 2 with Root Builder | The mod lower in the left pane |
| Vortex | The mod that loads after the other |
| By hand | The archive extracted last |

Without ownership files, the player's order has no effect on that asset. The game
falls back to your declared dependencies, then to the alphabetical order of the
mod folder names.

## Step by step

### 1. Package the mod in the Toolkit

Use the Toolkit's Mod Packaging tab
([NVIDIA's guide](https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-packaging.html)).
If your mod builds on another Remix mod, make it a dependency: your mod then wins
wherever it overrides that mod, even with no ownership files.

### 2. Lay out the folder

Copy the packaged output to a working folder, away from your game:

```
MyModPackage\
  rtx-remix\
    mods\
      MyMod\
        mod.usda
        ...
```

The folder name (`MyMod`) is your mod's identity, because ownership files and other
mods' dependencies refer to it. Keep it the same in every version.

### 3. Write the ownership files

Each file is named after the asset and holds your mod's folder name. The asset names
are `mat_<HASH>`, `mesh_<HASH>` and `light_<HASH>`, where the hash is 16 upper-case
hex digits.

Open PowerShell in your working `rtx-remix\mods` folder, set `$mod` to your folder
name and run this:

```powershell
$mod = "MyMod"
$owners = Join-Path (Get-Location) "owners"
if (Test-Path $owners) { Remove-Item -Recurse -Force $owners }
New-Item -ItemType Directory $owners | Out-Null
Get-ChildItem $mod -Recurse -Filter *.usda |
  Select-String -AllMatches '(?:over|def\s+\w+)\s+"((?:mat|mesh|light)_[0-9A-F]{16})"' |
  ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique |
  ForEach-Object { [IO.File]::WriteAllText((Join-Path $owners $_), "$mod`n") }
(Get-ChildItem $owners).Count
```

Run it again after every export. It replaces the `owners` folder, so **only run it
in your working folder, never in a game folder**, where it would delete other mods'
ownership files.

Check that the count it prints looks right for your mod. The snippet only reads
text layers (`.usda`). If your mod has binary `.usd` or `.usdc` layers, use
`remix_mod_owners` instead, which reads the composed mod:

```
remix_mod_owners.exe rtx-remix\mods\MyMod
```

Its source and `build.bat` are in
[`tools/remix_mod_owners`](https://github.com/Kim2091/dxvk-remix/tree/fnv/main/tools/remix_mod_owners)
of the runtime.

The folder must be called `owners`, with no leading dot: Root Builder doesn't copy
names that start with a dot.

### 4. Make the archives

Offer two downloads with the same content:

**Manual and Vortex:**

```
rtx-remix\mods\MyMod\...
rtx-remix\mods\owners\...
vortex_override_instructions.json
```

`rtx-remix` must deploy to the game folder, not to `Data`. The
`vortex_override_instructions.json` file tells Vortex that, and holds this line:

```json
[{ "type": "setmodtype", "value": "dinput" }]
```

**Mod Organizer 2 with Root Builder:**

```
Root\rtx-remix\mods\MyMod\...
Root\rtx-remix\mods\owners\...
```

### 5. Check it in game

1. Install the mod the way a player would, and start the game.
2. Open the Remix menu (Alt+X) and go to **Enhancements > Mod Conflicts**. It lists
   your mod and how many materials, meshes and lights it replaces.
3. If you have another mod replacing the same asset, swap their order and press
   **Reload Enhancements**. Mod Conflicts shows the new winner and "ownership file"
   as the reason. `rtx-remix\logs\remix-dxvk.log` has the same in its `[ModOrder]`
   lines.

## What to tell your players

Replacements match the original asset, so another mod can stop yours from applying
without any error. Say in your mod's description:

- **What it replaces.**
- **Which originals you made it against**, such as vanilla or a named texture pack.
  A texture or mesh mod that changes the same original changes its name, and your
  replacement then no longer applies.
- **What it needs:** New Vegas RTX, plus any Remix mods it depends on.

## What can be replaced

| Asset | Materials | Meshes |
|---|---|---|
| Objects, architecture, clutter | Yes | Yes |
| Grass, distant objects, distant trees | Yes | Yes |
| Faces | Yes | No: they change every frame |
| Particles | Yes | No: they're rebuilt every frame |
| Tree leaves, skinned meshes | Yes | Untested |
| Land, water | Untested | Untested |

Lights placed in the world can be replaced. Lights from effects, such as muzzle
flashes and explosions, can't. A material replacement replaces the whole material,
so supply its normal and roughness maps too.

## What to leave out

- The capture, your Toolkit project and your source images.
- Ownership files for assets your mod doesn't replace, or files naming another mod.
  A file naming another mod makes yours lose that asset.
- Anything you don't have the right to redistribute. Credit authors where their
  licence requires it.

## More detail

- [Packaging and capturing](https://github.com/Kim2091/dxvk-remix/blob/fnv/main/documentation/RemixModPackaging.md)
- [How the game picks a winner](https://github.com/Kim2091/dxvk-remix/blob/fnv/main/documentation/RemixModOrder.md)
- [Installing Remix mods, for players](https://github.com/Kim2091/dxvk-remix/blob/fnv/main/documentation/RemixModsForPlayers.md)
