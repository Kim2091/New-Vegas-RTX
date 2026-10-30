# Third-party code

The wrapper is built on [remix-comp-base](https://github.com/xoxor4d/remix-comp-base)
by xoxor4d (MIT, see `LICENSE`). The libraries under `deps/` keep their own
licences:

| Library | Folder | Licence | Licence text |
|---|---|---|---|
| Dear ImGui 1.91.7 (Omar Cornut) | `deps/imgui` | MIT | `deps/imgui/LICENSE.txt` |
| MinHook (Tsuda Kageyu), with HDE (Vyacheslav Patkov) | `deps/minhook` | BSD 2-Clause | `deps/minhook/LICENSE.txt` |
| xxHash 0.8 (Yann Collet) | `deps/xxhash` | BSD 2-Clause | header of `deps/xxhash/xxhash.h` |
| miniz 3.1.2 (Rich Geldreich, RAD Game Tools, Valve Software), the release's `miniz.c` / `miniz.h` unchanged | `deps/miniz` | MIT; one part of `miniz.c` is also released into the public domain (Unlicense) | `deps/miniz/LICENSE`, and the header of `deps/miniz/miniz.c` |
| Tracy Profiler client (Bartosz Taudul), only with `build.bat --tracy` | `deps/tracy` | BSD 3-Clause | `deps/tracy/LICENSE` |
| RTX Remix API headers (NVIDIA) | `deps/bridge_api` | MIT | header of each file |
| Remix bridge settings template (NVIDIA bridge-remix) | `assets/.trex/bridge.conf` | MIT | [bridge-remix LICENSE](https://github.com/NVIDIAGameWorks/bridge-remix/blob/main/LICENSE) |
| Menu icon from remix-comp-base (xoxor4d) | `assets/rtx_comp/textures/berry.png` | MIT | `LICENSE` |
| DirectX SDK (June 2010) headers and import libraries (Microsoft) | not included: installed separately, found through `DXSDK_DIR` | Microsoft DirectX SDK licence terms | with the SDK |

Binaries built from this repository contain MinHook, Dear ImGui, xxHash and
miniz code; their notices above must ship with them.

[NewVegasRTXHelper](https://github.com/BlueAmulet/NewVegasRTXHelper)
(Wall_SoGB, BlueAmulet, xoxor4d) first mapped many of the FalloutNV.exe
addresses and offsets the wrapper uses. Those are facts about the game's
executable; no NewVegasRTXHelper code is included, so its licence does not
apply to this repository or its binaries.

The engine monitor plugin reads the DataHandler's plugin list at the
address and offsets xNVSE's `GameData.h` documents
([xNVSE](https://github.com/xNVSE/NVSE)); those are facts about the game's
executable too, and no xNVSE code is included. The session report finds the
crash logs [Crash Logger](https://www.nexusmods.com/newvegas/mods/82540)
writes by their folder name only (`Crash Logs`). The zip writer
(`src/comp/game/zip_store.cpp`) is the wrapper's own (the published ZIP
format); its deflate is miniz's low-level compressor (`tdefl`).
