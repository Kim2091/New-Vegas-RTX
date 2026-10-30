# Hook compatibility

Other mods (xNVSE plugins, ASI mods, NVTF, JohnnyGuitar, Stewie's Tweaks
and others) patch the same engine code the wrapper does, and they may load
before or after it. The rule:

**Every write the wrapper makes to engine code goes through
`src/comp/game/call_chain`.** No hook calls MinHook, writes a jump or
compares prologue bytes on its own. The module works the same way for every
mod: it decides from the bytes found at the site, never from a mod's name.

## What a site can hold

`call_chain` reads the site first and puts it in one of three states.

| State | Function entry (`hook_entry`) | Call site (`inspect`) |
|---|---|---|
| **Vanilla** | the engine's prologue from the address table (`shared/fnv_exe.hpp`) | the engine's `E8 rel32` call |
| **Chained** | another module's detour into executable code: `E9 rel32`, `FF 25 [abs]`, `68 imm32 ; C3` or `B8 imm32 ; FF E0` (`shared/hook_forms.hpp`) | an `E8 rel32` call into another module's executable code |
| **Foreign** | anything else: a partial overwrite, a patch inside the prologue, a jump into unmapped memory | anything else: a JMP, NOPs, a call into unmapped memory |

What happens in each:

- **Vanilla:** the wrapper hooks it. No log line beyond the feature's own.
- **Chained:** the wrapper hooks in front of the other module. Its hook runs
  first, then the other module's, then the engine's. At an entry, MinHook
  builds the trampoline from what is there: it relocates a leading `E9` to
  its target and copies the other forms whole. At a call site, the
  wrapper's thunk calls the target the site held. One INFO line names the
  previous target and its module.
- **Foreign:** nothing is written. One WARN line shows the bytes, and the
  feature that needed the hook stays off.

## Hooks added after the wrapper

The wrapper's own writes stay in standard forms, so a module that hooks
later finds something it can chain from:

- a plain `E9` to the wrapper's hook at a function entry (MinHook);
- an `E8` call to the wrapper's thunk at a call site. A thunk that must run
  code after the call (`emit_call_wrapper`) keeps the site's return address
  aside and returns to the site itself, so a module that calls it from its
  own hook gets control back.

Every place the wrapper writes or relies on is tracked (`track`). At the
first Present and every 600 frames, `on_present` checks each one. When
another module replaced it, one line names the module: INFO when the site
now leads into executable code (a module chained in front of the wrapper or
replaced it), WARN when it holds anything else. Nothing is ever written
back.

## Vtable slots

A hook that writes a vtable slot saves the slot's current pointer, whatever
it is, calls that pointer, and tracks the slot as `Kind::Slot`. It never
calls a hard-coded engine address. The wrapper prefers hooking the function
entry over the slot: the entry runs for every caller, including a module
that replaced the slot and calls what it replaced. `tex_pack` does this and
tracks NiDX9SourceTextureData's slot so a later change is logged.

## Bytes and data

- A byte patch (the skinned-path `jz` at 0xB992F2) is not a hook and is not
  chained. It is written only over the engine's byte; a site that already
  holds the patched byte is accepted as is; anything else is foreign. The
  byte is tracked as `Kind::Byte`.
- Engine data the wrapper changes is read before it is written, never
  assumed vanilla: game settings are found by name and changed from their
  live value (`game_ini`), decal flags are OR-ed into the live flags only
  while the property still holds what was read (plugin `decals`), and the
  menu-background capture bytes are written only while the capture code is
  vanilla (`menu_bg`).

## Deliberate exceptions

The engine monitor plugin checks some code sites for their vanilla bytes on
purpose, so the wrapper steps aside when another mod owns that code: the
decal path (`decals`) and the menu-background capture (`menu-bg`, which
Real-Time Menus patches). These are not hooks; the wrapper only reads
engine state there, and its feature turns off when the code is not vanilla.

## Integrity report

`d3d9.dll` exports `FnvRemix_HookChain` (`shared/fnv_remix_plugin_api.h`):
one record per code site the wrapper wrote, with what it led to before. The
plugin's integrity report reads it, so a wrapper site reads as `ours`,
`ours, chained after <module>`, `chained: <module> patched it after the
wrapper`, or `another mod's hook: <module>` before the wrapper installs.
Only unrecognised bytes read as `foreign`.

## Testing

`tests/hook_chain/build_test.bat` builds a standalone x86 test of
`call_chain` against dummy functions that already carry each detour form,
and runs it. It is not part of the shipped DLLs.
