# Scope presets for mod authors

A scoped weapon does not need a lens file for a path-traced scope: the lens is
placed from the weapon's first-person mesh as the game loaded it, and a weapon
without a file gets a default telescopic lens. A lens file chooses the optics
and the look (reticle, tint, magnification, vision modes, ...) by EditorID, and
may give the lens geometry, which the mesh then checks. This page is for
authors who add scoped weapons, or who change a scoped weapon's first-person
mesh.

## How the lens is placed

At each equip (and when the weapon's mods or its first-person model change),
the wrapper reads the triangles of the weapon's first-person model from the
scene graph and measures its scope: round glass, rims and tube mouths along
one axis, the tube's walls around it, the eyepiece at the end away from the
muzzle (`ProjectileNode`) and the objective at the other, with the objective
point just past the scope's front end. The barrel, axes whose eyepiece the eye
cannot see, and axes with geometry right ahead of the objective are turned
down; when two axes are about as good, the fit is not confident.

Which geometry the lens uses, by the file's `Fit` key:

| `Fit` | The lens geometry |
|---|---|
| `auto` (default) | The file's when the fit is confident and agrees with it; the fitted geometry when the fit is confident and the file's is off (or the file gives none); the file's when the fit is not confident but the file's passes a check against the mesh (the eyepiece in an opening, nothing between the eye and the eyepiece, free space ahead of the objective); otherwise the game's scope. |
| `file` | The file's when it passes the check against the mesh; otherwise the fitted geometry when the fit is confident; otherwise the game's scope. |
| `mesh` | The fitted geometry when the fit is confident; otherwise the game's scope. |
| `off` | No lens: the game's own scope. |

A file without `EyepieceCenter` gives no geometry: the fitted geometry is used
with the file's optics. The fitted values are in the frame of the scope's
nearest uniquely named node, so a scope on an animated node keeps its lens.

The log has one line per weapon and mod set, starting `Scopes: fit for
<EditorID> (mods ...), <model>:` with the fitted eyepiece, axis, radii and
confidence, and ending with the decision: `file geometry (confirmed by the
mesh)`, `fitted geometry (the file's eyepiece is 1.31 from the mesh's)`, `file
geometry (fit not confident: ...; passes the mesh check)`, or `no lens: ...;
the game's own scope stays`. With verbose logging every axis the fit looked at
is logged too.

`[Scopes] FitLens=0` turns the fit off: the file's geometry is used as written,
with the `Models` check below. `[Scopes] DefaultLens=0` keeps the game's scope
on weapons without a file.

## Where the files go

`Data\Config\NewVegasRTX\Scopes\<EditorID>.ini`, one file per weapon, named
after the weapon record's EditorID (characters other than letters, digits,
`_` and `-` become `_`).

Ship yours in a subfolder of that folder, for example
`Data\Config\NewVegasRTX\Scopes\MyRifleMod\<EditorID>.ini`:

- A file in any subfolder (at any depth) wins over the same name in the
  `Scopes` folder itself.
- Between subfolders, the one whose path sorts last wins (alphabetically,
  ignoring case). A compatibility patch that must win over another mod's can
  use a `zz_` prefix.
- The winning file is used whole: its sections are not merged with another
  file's.

The log names the file each weapon's entry came from (`Scopes: lens file for
<EditorID>: <path>`).

## The file

```ini
[Lens]
Kind = telescopic
EyepieceCenter = 1.16, 10.02, 0
Axis = 1, 0, 0
Up = 0, 1, 0
EyepieceRadius = 0.89
ObjectiveDistance = 24.6
ObjectiveRadius = 0.95
EyeRelief = 5.6
Reticle = german
Models = weapons\2handrifle\huntingriflescope.nif

[Lens.13]
; the same weapon with mods 1 and 3 installed: only what differs
EyepieceCenter = 1.16, 10.4, 0
Models = mymod\weapons\myriflemod13.nif
```

- `[Lens]` is the weapon's entry. `[Lens.<slots>]` is an entry for one
  combination of installed weapon mods, written as the installed slot numbers
  in order (`1`, `2`, `3`, `12`, `13`, `23`, `123`; `0` for none). Its keys
  override the `[Lens]` keys one by one; keys neither gives keep their
  defaults.
- Values are in the first-person weapon model's root node space (or `Node`'s):
  +X toward the muzzle, +Y up, game units (about 1.43 cm).
- Lines starting with `;` or `#` are comments.

## Keys

| Key | Value | Default |
|---|---|---|
| `Kind` | `telescopic` or `reflex` (a red dot or holographic window) | `telescopic` |
| `EyepieceCenter` | x, y, z of the rear glass's center | 0, 0, 0 |
| `Axis` | x, y, z of the optical axis, eye toward the objective | 1, 0, 0 |
| `Up` | x, y, z of the reticle's up | 0, 1, 0 |
| `EyepieceRadius` | the rear glass's radius (a rectangle's half height) | 0.9 |
| `ObjectiveDistance` | the front glass's distance from the eyepiece along the axis | 20 |
| `ObjectiveRadius` | the front glass's radius | 1 |
| `EyeRelief` | the eye's distance behind the eyepiece | 5.6 |
| `Magnification` | 0 = the game's own zoom for the weapon | 0 |
| `MinMagnification`, `MaxMagnification` | the mouse wheel's zoom range; both 0 = fixed | 0 |
| `FieldStop` | degrees | 18 |
| `Aperture` | `circle` or `rectangle` | `circle` |
| `ApertureAspect` | a rectangle's width over its height | 1 |
| `Reticle` | `none`, `crosshair`, `duplex`, `dot`, `circledot`, `mildot`, `german`, `drop`, `stadia`, `frame`, `grid`, `dial` | `duplex` |
| `ReticleColor`, `ReticleRadiance`, `ReticleLineWidth` | the reticle's color, glow and line half-width | black, 0, 0.004 |
| `ReticleAccentColor`, `ReticleAccentRadiance` | the second reticle color (mil-dots, center dots) | none |
| `ReticleTexture` | a `.dds` whose alpha is the reticle, relative to `Data` (ASCII path) | none |
| `ReticlePlane` | `second` or `first` (the reticle grows with the zoom) | `second` |
| `Markings` | `labels`, `rangefinder` or `labels,rangefinder` | none |
| `MarkingColor`, `MarkingRadiance`, `MarkingOpacity` | the markings' look | `[Scopes]` settings |
| `Tint` | the glass's color | 0.92, 0.92, 0.92 |
| `EdgeGlowColor`, `EdgeGlowStrength`, `EdgeShade`, `EdgeTint` | the field stop's glow ring, darkening and vignette color | none |
| `VisionMode` | the starting mode: `none`, `night` or `thermal` | night on scopes with the game's night-vision flag |
| `VisionModes` | the modes the vision key cycles, in order | none and `VisionMode` |
| `NightTint`, `NightGain` | night vision's color and gain | the runtime's |
| `HighlightHostiles` | `1` outlines hostile actors | 0 |
| `ParallaxDistance` | meters the reticle is focused at; 0 = none | 100 (telescopic), 0 (reflex) |
| `GlareStrength`, `DirtStrength` | 0..1, the glass's glare and dirt | `[Scopes]` settings |
| `Grain` | `off` = no film grain on the lens | `on` |
| `Node` | the model node the values are in, for a scope that moves on its own | the root |
| `Models` | the first-person models the values fit (see below) | no check |
| `Fit` | `auto`, `file`, `mesh` or `off`: where the geometry comes from (see above) | `auto` |

## The Models check

`Models` lists the first-person model paths (relative to `meshes\`, case and
slashes do not matter) the values were made for. It is used only with
`[Scopes] FitLens=0`, or when the weapon's mesh cannot be read: then, when it
is given and the weapon is drawn with a different first-person model for its
installed mods (the record's first-person model for that combination, else
its own), the lens is not used for that weapon: the game's own scope stays,
and the log says which model was drawn. Without `Models` there is no check.

With the fit on, a mesh replacer that keeps the original path, or a mod that
points a scoped weapon at a new mesh, gets a lens fitted to its own mesh; the
vanilla file's geometry is used only where that mesh confirms it.

## Making one in game

1. Equip the weapon and aim it.
2. Open the Alt+X menu, section **Scope editor**. The sliders start from the
   lens on screen (the fitted geometry, or the file's), and the status line
   says which it is. Change the optics and look as wanted. Moving a geometry
   slider sets the entry's `Fit = file`: your geometry then wins over the
   mesh's as long as it passes the check against the mesh.
3. **Use the fitted geometry** moves the geometry sliders onto the fit and
   sets `Fit = auto` again. **Log the scope fit** measures the mesh again and
   writes every axis it looked at to the log, with the reason each was taken
   or turned down.
4. **Save** writes the file it came from, or, for a weapon without one,
   `Data\Config\NewVegasRTX\Scopes\<EditorID>.ini`. Save adds the model the
   weapon is drawn with now to `Models`. "Only for these weapon mods" saves
   the edit as the installed combination's section.

With a mod manager's virtual file system the file lands where that manager
puts new files (its overwrite folder); move it into your mod.
