#!/bin/sh
# Sync the game inputs the runtime reads from /sdcard/tok to the Odin 3 (adb push --sync only sends changed files).
# The 17 GB RomFS image itself is pushed once by hand: adb push romfs.bin /sdcard/tok/v143/romfs/
set -e
# botw160: Breath of the Wild (tools/build_botw_apk.sh)
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); V=${TOTK_VERSION:-v143}
X=$ROOT/work/totk/extracted/$V; R=$ROOT/work/android/$V/rewritten; DATA=/sdcard/tok/$V
adb shell mkdir -p $DATA/romfs $DATA/rewritten /sdcard/tok/fonts
for m in main sdk subsdk0; do adb push --sync -q $R/$m.patched.bin $DATA/rewritten/; done
adb push --sync -q $X/romfs/runtime-index.bin $DATA/romfs/
# Breath of the Wild: the add-on content volumes (extracted/<version>/aoc/<index>/, games/botw/scripts/extract_dlc.py; runtime/aoc_romfs.c)
[ -d $X/aoc ] && adb push --sync -q $X/aoc $DATA/
# Tears of the Kingdom's RomFS overlays (none for Breath of the Wild)
for d in romfs-patches romfs-patches-out; do [ -d $X/$d ] && adb push --sync -q $X/$d $DATA/; done
# the 1.0.0 side of the mod merge (tools/make_mod_base.py; the startup screen's Mods page needs it to install a mod)
[ -f $X/mod-base.bin ] && adb push --sync -q $X/mod-base.bin $DATA/
adb push --sync -q $ROOT/work/totk/extracted/firmware/shared-fonts/. /sdcard/tok/fonts/
# the PlayStation button icons, a built-in mod the startup screen merges in for Button letters = PlayStation
# (tools/make_icon_mod.py, from the player's RomFS)
# the icon mod reads Tears of the Kingdom's RomFS
if [ $V = v143 ]; then
  [ -f $ROOT/work/android/$V/mods/_buttons-playstation/files.tsv ] || "$ROOT/work/venv/bin/python" "$ROOT/port/android/tools/make_icon_mod.py"
  adb shell mkdir -p $DATA/mods/_buttons-playstation
  adb push --sync -q $ROOT/work/android/$V/mods/_buttons-playstation/. $DATA/mods/_buttons-playstation/
fi
# the game data folder (tools/gen_runtime.py): module tables, RomFS and shared-font hashes the runtime reads at start
adb shell mkdir -p $DATA/game-data
adb push --sync -q $ROOT/work/android/$V/tree/build/game-data/$V/. $DATA/game-data/
