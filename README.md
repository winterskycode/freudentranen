# freudentranen

The Legend of Zelda: Tears of the Kingdom (Nintendo Switch, 1.4.3) running natively on Android: the game's own
ARM64 code executes on the device, with a host for the Switch's system services and NVN graphics. It is not an
emulator. Built for and tested on an AYN Odin 3 (Snapdragon 8 Elite / Adreno 830).

**Bring your own copy.** This repository holds no game content: no game code, data, shaders, keys or firmware.
Everything the game needs is made locally from the player's own dump and keys, and the outputs stay local.

## Status: pre-alpha
- Plays from the title screen through the open world on the AYN Odin 3 (Snapdragon 8 Elite, Adreno 830), the device
  it is developed on. It also runs on the AYN Thor (Adreno 740) and a Sony Xperia 1 VI (Adreno 750), with less testing.
  Other devices and GPUs are untested.
- Vulkan renderer for the game's NVN graphics, on the bundled Turnip driver, the system driver, or a driver installed
  from a zip.
- Frame rate above the Switch's 30 FPS cap, upscaling (FSR 1, Arm ASR), anti-aliasing (SMAA) and frame generation
  (built-in, AMD FSR 3.1, or lsfg-vk with the player's own copy of Lossless Scaling).
- Startup screen and in-game menu with grouped settings, remappable field controls, Xbox / Switch / PlayStation button letters, RomFS mods,
  and bug reports with a video of the last 30 seconds.
- No PC needed: pick your keys and game files in the app. The game plays from them where they are, with nothing copied.
- Saves can be exported and imported in the app, including saves from Switch emulators.

Reports and pull requests are welcome; changes are reviewed before they are merged.

## How this was made
This project was built with extensive AI assistance (Claude, through Claude Code): porting, the graphics backend,
debugging on the device and the tools around them. Without it, the project would not exist. We say so up front
because we have nothing to hide, and because you should know how the code you run was written. Changes are tested
on the device, and their commit messages say how each one was found and checked.

## Building
`BUILDING.md` describes building the APK, including the bundled Turnip driver, from source. Releases are built from a
tag (`.github/workflows/release.yml`).

## Layout
- `port/android/`: the Android app: the compatibility layer for MacNativeGames' Apple APIs, the host (window, input,
  audio, bug reports), the Vulkan backend (`vk/`), the startup screen (`app/`) and the build and device tools (`tools/`).
- `port/rewriter/`: the code rewriter with the Android target (jhaizhou's rewriter as MacNativeGames uses it, with the
  D2R Android port's Android target merged in).
- `port/xlate/`: builds of the Ryujinx shader translator (NativeAOT).
- `port/build_all.py`: extract, verify and rewrite the player's game files on the PC (until the app does it).
- `patches/`: this project's changes to MacNativeGames (`port/setup.sh` applies them) and to Turnip.
- Device tools in `port/android/tools/`: `saves.py` (import and export saves over adb, with backups; the app does the same on its own under
  Tools > Saves), `bench.py` (a repeatable
  camera-orbit benchmark on a given save), `aot_shaders.py` and `push_shaders.sh` (translate every shader ahead of time;
  optional, as the app translates them on demand).

## Credits and licences
This port stands on other people's work:

- [MacNativeGames](https://github.com/ly05010419/MacNativeGames) (MIT; our changes are in [our fork](https://github.com/winterskycode/MacNativeGames)): the native runtime, the game's patches and the
  rewriter it builds on.
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) by jhaizhou (MIT): the code rewriter and the NVN graphics
  layer.
- [d2r-android](https://github.com/sydeEvans/d2r-android) by sydeEvans (MIT): the first Android port of d2r-mobile.
- The D2R Android port by ImL1s (AGPL-3.0; it was published at gitlab.com/aa22396584/d2r-mobile, and neither it nor its
  GitHub mirror is public any more): the Vulkan renderer, the GPU driver loader and the rewriter's Android target all
  started as its code.
- Ryujinx's shader translator and the .NET runtime compiled into it (MIT), the Turnip driver from
  [Mesa](https://mesa3d.org) (MIT, some parts Apache-2.0 / BSL-1.0), libadrenotools (BSD-2-Clause), Dear ImGui,
  lsfg-vk, volk, pe-parse, AMD FSR 1, AMD FSR 3.1 (FidelityFX SDK v1.1.4), Arm ASR, SMAA and the RenderDoc API header (MIT), DXVK's DXBC compiler (zlib), the
  Khronos SPIR-V headers (MIT-style), Opus, libvpx and zstd (BSD), the LLVM
  C++ runtime (Apache-2.0 with LLVM exception), and the Figtree and Marcellus SC fonts
  (SIL OFL 1.1).

Because the renderer and the rewriter's Android target come from the D2R Android port, this project is distributed
under the AGPL-3.0 (`LICENSE`). `NOTICE` says what came from where, what was changed and where each release's complete
source is. The permissive code keeps its own licences and notices. The APK carries every bundled component's licence
text (`port/android/app/licenses.tsv` lists them; the startup screen shows them under Tools > Licences).
Not affiliated with Nintendo. The Legend of Zelda and Nintendo Switch are trademarks of Nintendo.
