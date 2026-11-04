# Playing from the player's own NSPs

Three C libraries. All three work on buffers or descriptors and write no files. The game links them through `host/player_files.c`;
the launcher reaches the reader through `libtotksetup.so` (`host/setup_jni.c`) on its Game files page.

| Folder | Header | Does | Test |
|---|---|---|---|
| `container/` | `container.h` | reads keys, modules and RomFS from the NSPs | `regress.sh` |
| `patcher/` | `patcher.h` | rewrites a module in memory | `regress.sh` |
| `tables/` | `tables.h` | makes `runtime-tables.bin` in memory | `regress.sh`, which also runs the whole path |
| `modbase/` | `modbase.h` | makes `mod-base.bin` in memory, for the mod merge | `regress.sh` |

The rules that replace the PC's overlays are in `host/prompts_romfs.c`; `host/tests/regress.sh` compares them with the overlays.

The whole path on a PC:

    tables_cli chain <prod.keys> <base.nsp> <update.nsp> --rewritten <dir> --game-data <dir> [--full] [--bench N]

## What each file of game-data becomes

| File | Decision | Why |
|---|---|---|
| `runtime-tables.bin` | made at every start, `tokt_make` | 16 MB of symbols, names and relocations from the game. It can not ship. About 3 ms on a PC once the image hashes are known, so a cache would gain nothing. |
| `rewriter.txt` | dropped | Nothing reads it on Android. `TOKT_REWRITER_TXT` gives its bytes if wanted. |
| `romfs-config.txt` | compiled in, `host/player_files.c` | Three constants of 1.4.3: size and two hashes. The reader reproduces them from the NSPs (`chain --full`). |
| `shared-fonts.txt` | not read | From the player's files one open font fills the six slots, with no firmware to check. |

The hashes that ship are the plan's "hashes only, no content".

## Integration, in order

The MacNativeGames files below are changed in that checkout and committed on the `android` branch of our fork (`port/setup.sh` pins it). Keep today's
path working until the new one matches it: everything new runs only when `TOTK_BASE_FD` is set.

### 1. Build: `port/android/CMakeLists.txt`

- `add_subdirectory(setup/tables EXCLUDE_FROM_ALL)`. It brings in the patcher and the container reader too.
- Add `toktables tokpatcher tokcontainer` to `target_link_libraries(totk ...)`.
- Capstone comes from `deps/build_deps.sh`; a checkout built before that script gained it has none in `deps/src`.

### 2. New file: `port/android/host/player_files.c`

Everything that holds a `tokc_game`, so that `loader.c` and `romfs.c` stay free of the libraries' types.

- `const tokc_game *player_game(void)`: NULL when `TOTK_BASE_FD` is not set. Otherwise, on first use:
  `tokc_keys_parse_fd` on `TOTK_KEYS` (a path), `tokc_game_open` on `TOTK_BASE_FD` and `TOTK_UPDATE_FD`, `tokc_keys_free`.
- `void player_load_modules(...)`, for main, subsdk0 and sdk in the file's order (`tokt_module_id`, not `tokc_module`):
  `tokc_game_module`, `tokp_rewrite_alloc`, then `tokt_check_main` on main, then `tokt_make`. The patcher's calls may run on
  separate threads, but all three take about 60 ms on a phone, so start without.
- Hash each image once with `tokt_sha256_hex`, compare with the shipped hashes if they are there, and pass the result as
  `image_sha256` so that `tokt_make` does not hash again.
- Any error: `runtime_fail` with the library's text. That text names keys, never their values.
- If `<TOTK_ROMFS>/runtime-index.bin` is missing, write it from `tokc_romfs_index`. This is the plan's cached index and it leaves
  every reader of the index unchanged (36 ms on a PC to make, so rebuilding it in memory at each start is also open).

### 3. The launcher

The game runs in the process `:game`, so the launcher's descriptors do not reach it.

- `Setup.java` holds every decision and no Android classes (`tools/setup_test/run.sh` runs it on a PC). `SetupFiles.java` is the
  Android side. `LauncherActivity.setupPage` is the page: first on a new install, then under Tools.
- The two picked URIs (`takePersistableUriPermission`) and the private copy of `prod.keys`, mode 600, are in the no-backup folder
  (`Context.getNoBackupFilesDir`), not `files/`: cloud backups leave it out, and bug reports copy nothing from it.
- `GameActivity.onCreate`, before `super.onCreate`, since that is what loads `libtotk.so`: open both URIs with
  `ContentResolver.openFileDescriptor(uri, "r")`, call `detachFd()`, and pass the numbers and the keys path with `Os.setenv` as
  `TOTK_BASE_FD`, `TOTK_UPDATE_FD` and `TOTK_KEYS`. The descriptors stay open for the life of the process.
- A URI that no longer opens: `super.onCreate` cannot be skipped, so `TOTK_SETUP_STOP` tells `ANativeActivity_onCreate` to do nothing,
  the activity finishes, and the launcher opens the Game files page with the reason (`game-files-problem.txt`).
- `LauncherActivity.homePage`: the game counts as present with valid picks or with the old prepared data. Picks win when both
  are there. `Game.ROMFS_SIZE` stays for the old path.

### 4. `host/app.c`, `start_game`

- `TOTK_ROMFS` defaults to `<files>/player-romfs` on this path (done). `TOTK_GAME_DATA` is still `/sdcard/tok/v143/game-data`, until the
  next point is done. `TOTK_ROMFS_PROOF_DIR` needs nothing: this path writes no proof record.
- Copy `romfs-config.txt` and `shared-fonts.txt` from the APK's assets into the `TOTK_GAME_DATA` folder. `game_data.c` then needs
  no change.
- `TOTK_ROMFS`, `TOTK_GAME_DATA` and `TOTK_ROMFS_PROOF_DIR` default to folders under `activity->internalDataPath`, not `/sdcard/tok`.
  `romfs.c` writes its proof record to `TOTK_ROMFS` unless `TOTK_ROMFS_PROOF_DIR` is set.
- `romfs-config.txt` is compiled in: `game_data.c` asks `player_game_file` first, and `shared_font.c` asks `player_shared_font`.
- `rewritten_dir` is still passed to `main` as `argv[1]`. It is simply unused on this path.

### 5. `MacNativeGames/games/totk/runtime/loader.c`

- `load_tables`: split it after the `mmap`. `load_tables_from(const uint8_t *b, uint64_t size)` does every check from
  `const TablesHeader *h` on, so the tables get the same checks from memory. `Module.name`, `.digest`, `.relocs` and the symbol
  names point into this buffer, so it is never freed.
- `main`: before `load_tables()`, call `player_load_modules`, then `load_tables_from` when `player_game()` is set.
- `map_module`, the non-iOS one: keep the `mmap` and the size. Replace `fopen`, `fread` and the `CC_SHA256` check with a `memcpy`
  from the rewritten image, then free the image and its `tokp_result`. The hash was checked in `player_load_modules`. The key
  page, `key[8]` and `make_hash` stay. The rest of `main` (`relocate`, `runtime_game_patches`, `seal`) does not change.
- The NSOs can be freed as soon as `tokt_make` returns. The peak on a PC is 160 MB, before the game's own memory.

### 6. `MacNativeGames/games/totk/runtime/romfs.c`

Every read of the image already goes through `src_pread`, which makes this small.

- `src_pread`: first line, `if(player_game()&&fd==image_fd)` call `tokc_romfs_pread`, turning a negative result into -1. It is safe
  on any thread and takes any offset and size.
- `runtime_fs_init`: in place of opening `romfs.bin` or `open_chunks`, set `image_fd=dup(TOTK_UPDATE_FD)`. A real descriptor keeps
  `if(image_fd>=0)return`, `handle_source` and `fd==image_fd` right, and no other file can have that number.
- `runtime_fs_init`: skip the `fstat` size check, and compare `tokc_romfs_size` with `ROMFS_IMAGE_SIZE` instead.
- `verify_image`: today it `fstat`s the image, and if the record `.romfs-verified-v1` matches it returns. Otherwise it hashes the
  whole 17 GB through `sha_fd`, compares with `ROMFS_IMAGE_SHA256`, checks the file did not change meanwhile, and writes the
  record. `verify_chunks` is the same over several files, so reuse it unchanged: in `runtime_fs_init` set `chunk_fds[0]` and
  `chunk_fds[1]` to the base and update descriptors and `nchunks=2`. `verify_image` then goes to `verify_chunks`, `sha_fd` hashes
  through `src_pread`, and the `!nchunks` test leaves `image_map` NULL, so `read_whole` and `runtime_romfs_read` take their
  `src_pread` paths. The new branch in `src_pread` must come before its `chunk_size` arithmetic. `chunk_proofs` needs `S_ISREG`:
  check that a picked file's descriptor is a regular file on the devices, as no test has opened one through the picker yet.
- This first-run hash is the only check of RomFS data: `tokc_romfs_pread` does not verify the IVFC hashes. It also runs on the
  plain `CC_SHA256` in `compat/src/sha256.c`. Use `tokt_sha256_update` here, or the 17 GB is limited by the hash and not the
  reader.
- `overlay_load` and `sha_range` need no change: they read through `src_pread`.

### 7. The other readers of the old files

- `vk/nvn_vulkan_shader_setup.inc`: `setup_read` does `pread` on `romfs.bin`: use `tokc_romfs_pread`. `setup_index` still reads the
  cached index but opens `romfs.bin` at its end. `setup_modules` maps `<data>/rewritten/*.patched.bin`, which no longer exist:
  rewrite the three modules again there with `tokp_rewrite_alloc`, about 60 ms on a background thread, rather than keep
  82 MB for it. `m->base` is no substitute: its data has been relocated.
- `app/java/dev/tok/totk/ModData.java`, `Romfs`: opens `romfs.bin` from the launcher's process. It needs the reader through JNI,
  beside `Zstd` in `libtotkmods`.

### 8. What replaced the PC's other files

From the player's files `host/app.c` sets `TOTK_ROMFS` to a private folder and turns the overlays off. Nothing below is read from
`/sdcard/tok`.

| File | Now | Why |
|---|---|---|
| `romfs-patches/UI/LayoutArchive/Common...` | the capture rule, `host/prompts_romfs.c` | Four keys of two animations, moved as the game opens the file. |
| `romfs-patches/Event/EventFlow/DmF_SY_GameOver...` | dropped | 1.4.3 does not run this file: `EventFlowFileEntry.Product.143` sets `DmF_SY_GameOver` to version 110, so the flow is `DmF_SY_GameOver.110`, which the overlay never touched. |
| `romfs-patches-out/` | the heap rule, same file | It already made any size. With `TOTK_OUTPUT_RES=0` the app uses `1920x1080-scene-` 1920x1080, 2136x1200 and 2560x1440; the other three need a debug setting. Below 1600x900 no file is changed. |
| `/sdcard/tok/fonts` | `fonts/SharedFont.otf` in the APK, from `tools/fetch_font.sh` | See below. |
| `mod-base.bin` | `modbase/`, through `host/player_romfs_jni.c` | Made once in private storage. The Java side is still to do, see below. |

`mods/_buttons-playstation` is no longer from the PC either: the first start with PlayStation buttons makes it in the app's storage
(`IconMod.java`), from the player's font and layout archives and `icons/playstation.bin` in the APK (`tools/make_icon_asset.py`, which
reads no game file). `tools/icon_test` compares it with the PC tool's. A folder pushed from the PC serves prepared data only: its
layout line names the PC's overlay as the original, which the player's files do not have, so with picks the app makes its own.

**Shared fonts.** The six slots are what `main+0x105dc60` maps these names to: `nintendo_udsg-r_std_003.bfttf` 0 (standard),
`udsg-r_org_zh-cn` 1, `udsg-r_ext_zh-cn` 2, `udjxh-db_zh-tw` 3, `udsg-r_ko` 4, `nintendo_ext_003` 5. A complex font
(`fcpx/*.bfcpx` in `Font/*.bfarc.zs`) lists fonts in order, each with the ranges it serves; a font with no ranges takes the rest.
Only the Chinese and Korean archives name a shared font, and only slot 0. Their Han and Hangul come from the game's own fonts in
those archives (`DFHEI5A.bfttf`, `AsiaKDREAM7R.bfttf`, ...), so slot 0 is asked for accented letters, punctuation, signs and kana:
30 characters of the Simplified Chinese messages, 32 of the Korean, 28 of the Traditional. `tools/check_shared_font.py` lists them.

So the font is Noto Sans CJK SC Regular cut down to those ranges: 222 KB, not 16 MB. All six slots hold it. A player's own text,
such as a horse's name, can reach characters the firmware font had and this one lacks (mostly Latin Extended).

The game takes a font at the address as it is when it starts with `00 01 00 00`, `ttcf` or `OTTO` (`main+0x10e3e48`), and otherwise
calls `ScalableFontEngineHelper::Decode` on it. So the slots hold the plain font, and `runtime/shared_font.c` skips its firmware
checks on this path. OTF works: the game's own fonts, the CID-keyed `ninP_RodinNTLG` for one, are OTF.

The firmware files as the old path serves them, scrambled from the address on, pass neither test: `Decode` returns NULL.

**The mods.** `ModData.Romfs` and `Mods.BASE` still read `/sdcard/tok/<version>`. The native side is here, in `libtotkmods`: a class
`dev.tok.totk.PlayerRomfs` with `static native void open(String keys, int baseFd, int updateFd)`, `boolean has(String)`,
`byte[] read(String)`, `String sha(String)` and `void makeModBase(String out)`. Left to do in Java: that class, `ModData.Romfs`
calling it (its four methods), `Mods.BASE` under `getFilesDir()`, `makeModBase` on a worker thread when the Mods page first opens
and the file is missing, and the Mods page's text, which still names `tools/make_mod_base.py`.
