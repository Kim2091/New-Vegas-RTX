# Reading the game-thread profile in remix-comp.log

`[Perf]`, `[PerfSlow]` and `[PerfSummary]` carry a sampled profile of the game
thread (`src/comp/game/sampler.cpp`, 25 samples a second while in game).
Only samples from the frames the window counts are kept (in game, not a
menu or loading, no frame over 1 s), so the rate in `(N Hz)` is per counted
second and stays near 25. `thread CPU` is the game thread's CPU time over
the same frames.

- `[Perf]`: the top 5 modules, each with the system code it calls, then the
  share of samples spent waiting by kind (sleep, wait, lock, message wait,
  file I/O) and the module that called each wait.
- `[Perf]` also splits the wrapper's own time: "wrapper draws by route" is the
  draw time per frame and the draws per frame of each renderer route (the
  route names are the renderer's tags: `API_STATIC`, `PASS_2D`, ...), top 8;
  "wrapper end of frame by part" is the work before the bridge's Present by
  named part (`d3d9ex.cpp` pre_present), top 5.
  "wrapper calls into the engine monitor plugin" is the time the wrapper spent
  in fnv_remix_nvse.dll's API (facts, owner_of, cells, lights, the event
  drain); it is part of the engine, draw and end-of-frame times.
- `[PerfSlow]` (window) and the second `[PerfSummary]` line (session): own
  code by module, hot 256-byte blocks as `module+0xRVA`, system code by kind,
  the top call sites into system code as `kind module+0xRVA` (the RVA is the
  return address, so it names the calling function), and the measured
  suspend-to-resume time per sample and per frame.

A 30 s window holds about 750 samples (a share is good to about ±2%); the
session line is the one to read for small modules and hot blocks.

## Resolving module+0xRVA

**FalloutNV.exe** (no PDB; the exe always loads at 0x400000):
VA = 0x400000 + RVA, then with retools from Vibe-Reverse-Engineering:

    python retools/funcinfo.py "<game folder>\FalloutNV.exe" 0x<VA>

**d3d9.dll (remix-comp) and fnv_remix_nvse.dll**: take the DLL from the
player's release and its PDB from the matching `releases\<version>-symbols`
(`remix-comp.pdb` for d3d9.dll, `fnv_remix_nvse.pdb`), put both in one
folder, and run llvm-symbolizer, which ships with MSVC
(`VC\Tools\MSVC\<version>\bin\Hostx64\x64\llvm-symbolizer.exe`):

    llvm-symbolizer --obj=d3d9.dll --relative-address 0x12300 0x4A5F0

**d3d9_remix.dll** (bridge client): the same way, with the bridge build's
PDB if it was kept. **NVSE plugins and other modules**: the RVA goes to the
plugin's author with the plugin's version.
