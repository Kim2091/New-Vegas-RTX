# Scope vision modes by weapon

While aiming through a scope, the vision key (`[Scopes] VisionKey`, default N)
or the gamepad's vision buttons (`GamepadVisionButtons`) cycle the weapon's
allowed vision modes in the order listed. The weapon starts in its default
mode. `[Scopes] VisionAllModes=1` allows none, night and thermal on every
scope instead.

A scope's allowed modes come from its lens file's `VisionModes` key
(`Data\Config\NewVegasRTX\Scopes\<EditorID>.ini`), and can be changed in the
Alt+X menu's Scope editor (Vision key: none / night / thermal). In the
presets, the gauss and laser scopes and Christine's rifle allow none, night
and thermal; Red Glare and the holorifle start in night and allow night,
thermal and none; the other night-vision scopes (the game's
ScopeHasNightVision flag) allow night and none; every other scope none. A
lens file without `VisionModes` cycles none and its default mode.

| EditorID | Name | Default mode | Allowed modes | Source |
|---|---|---|---|---|
| ModTest | ModTest | none | none | no vanilla vision |
| NVDLC01WeapGaussRifleElijah01 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapGaussRifleElijah10 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapGaussRifleElijah20 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapGaussRifleElijah30 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapGaussRifleElijah40 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapGaussRifleElijah50 | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC01WeapHolorifle | Holorifle | night | night, thermal, none | override (VISION_MODES) |
| NVDLC01WeapNV9mmPistolDean | 9mm Pistol | none | none | no vanilla vision |
| NVDLC01WeapNV9mmPistolDean10plus | 9mm Pistol | none | none | no vanilla vision |
| NVDLC01WeapNV9mmPistolDean20 | 9mm Pistol | none | none | no vanilla vision |
| NVDLC01WeapNV9mmPistolDean30 | 9mm Pistol | none | none | no vanilla vision |
| NVDLC01WeapNV9mmPistolDean40 | 9mm Pistol | none | none | no vanilla vision |
| NVDLC01WeapNV9mmPistolDean50 | 9mm Pistol | none | none | no vanilla vision |
| NVDLC03WeapLaserRifleNoDrop | Laser Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC03WeapSniperRifleUniqueChristine | Christine's COS Silencer Rifle | none | none, night, thermal | override (VISION_MODES) |
| NVDLC04WeapRedGlare | Red Glare | night | night, thermal, none | override (VISION_MODES) |
| NVDLC04WeapTrailCarbineNoDrop | Trail Carbine | none | none | no vanilla vision |
| NVDLC05WeapNVAntiMaterielRifleCustom | Anti-Materiel Rifle (GRA) | none | none | no vanilla vision |
| NVDLC05WeapNVHuntingRevolverCustom | Hunting Revolver (GRA) | none | none | no vanilla vision |
| NVDLC05WeapNVLightMachineGunUnique | Bozar (GRA) | none | none | no vanilla vision |
| NVDLC05WeapNVPlasmaDefenderCustom | Plasma Defender (GRA) | none | none, night, thermal | override (VISION_MODES) |
| VHDOliverSniperAntiMaterielRifle | Anti-Materiel Rifle | none | none | no vanilla vision |
| WeapBooneSniperRifle | Boone's Scoped Hunting Rifle | none | none | no vanilla vision |
| WeapHuntingRifle | Hunting Rifle | none | none | no vanilla vision |
| WeapLaserRifle | Laser Rifle | none | none, night, thermal | override (VISION_MODES) |
| WeapNV44Revolver | .44 Magnum Revolver | none | none | no vanilla vision |
| WeapNV44RevolverRaul | .44 Magnum Revolver | none | none | no vanilla vision |
| WeapNV9mmPistol | 9mm Pistol | none | none | no vanilla vision |
| WeapNVAntiMaterielRifle | Anti-Materiel Rifle | none | none | no vanilla vision |
| WeapNVCowboyRepeaterUnique | La Longue Carabine | none | none | no vanilla vision |
| WeapNVDebugMegaPistol | Debug MegaPistol | none | none | no vanilla vision |
| WeapNVDLC00Faderator | The Faderator | none | none, night, thermal | override (VISION_MODES) |
| WeapNVGaussRifle | Gauss Rifle | none | none, night, thermal | override (VISION_MODES) |
| WeapNVGaussRifleUnique | YCS/186 | none | none, night, thermal | override (VISION_MODES) |
| WeapNVHuntingRevolver | Hunting Revolver | none | none | no vanilla vision |
| WeapNVMarksmanCarbine | Marksman Carbine | none | none | no vanilla vision |
| WeapNVMarksmanCarbineUnique | All-American | none | none | no vanilla vision |
| WeapNVSniperRifleUnique | Gobi Campaign Scout Rifle | none | none | no vanilla vision |
| WeapNVTrailCarbine | Trail Carbine | none | none | no vanilla vision |
| WeapNVVarmintRifle | Varmint Rifle | night | night, none | vanilla flag (ScopeHasNightVision) |
| WeapNVVarmintRifleUnique | Ratslayer | night | night, none | vanilla flag (ScopeHasNightVision) |
| WeapSniperRifle | Sniper Rifle | none | none | no vanilla vision |
