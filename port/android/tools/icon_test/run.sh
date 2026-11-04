#!/bin/bash
# IconMod.java on this PC, no device, on the player's own files: port/android/tools/icon_test/run.sh <folder> [work folder]
# <folder> holds prod.keys and the two NSPs, read-only. The real PlayerRomfs and zstd (host/player_romfs_jni.c, host/mods_jni.c)
# are built here for the PC, with stand-ins only for android.* and org.json. IconTest makes the mod, merges it with Mods.apply and
# tries damaged files. Then compare.py checks the result against the game and the PC tool, if MacNativeGames, work/ and its
# mod are there (TOTK_ROOT, default the repository). Key values are never printed, and the work folder holds game
# data: keep it out of the repository. Needs fetch_font.sh's asset, the system's zstd and Python 3.14 with fontTools.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/../.." && pwd)
real=$1
work=${2:-$(mktemp -d)}
asset=${TOTK_ICON_ASSET:-$android/deps/out/icons/playstation.bin}
root=${TOTK_ROOT:-$(cd "$android/../.." && pwd)}
mkdir -p "$work/classes"
java_home=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
reader=$android/setup/container
lib=("$reader/aes.c" "$reader/aes_hw.c" "$reader/sha256.c" "$reader/keys.c" "$reader/package.c" "$reader/bktr.c" "$reader/game.c" "$reader/romfs_index.c")
cc -O2 -g -Wall -shared -fPIC -pthread -I"$reader" -I"$android/setup/modbase" -I"$java_home/include" -I"$java_home/include/linux" \
   -o "$work/libtotkmods.so" "$android/host/mods_jni.c" "$android/host/player_romfs_jni.c" "$android/setup/modbase/modbase.c" "${lib[@]}" -lzstd

java_src=$android/app/java/dev/tok/totk
mapfile -t standins < <(find "$here/standins" -name '*.java')
javac -Xlint:all -Xlint:-options -source 11 -target 11 -d "$work/classes" "${standins[@]}" "$java_src/Game.java" "$java_src/Mods.java" "$java_src/ModData.java" \
      "$java_src/IconMod.java" "$java_src/Zstd.java" "$java_src/PlayerRomfs.java" "$here/IconTest.java"
java --add-opens java.base/java.io=ALL-UNNAMED -Djava.library.path="$work" -cp "$work/classes" dev.tok.totk.IconTest "$work" "$real" "$asset"

if [ -f "$root/MacNativeGames/games/totk/scripts/make_button_icons.py" ]; then
  "$root/work/venv/bin/python" "$here/compare.py" "$work/files/mods/_buttons-playstation" --root "$root" --font "$android/deps/src/noto-sans-cjk/NotoSansCJKsc-Bold.otf" --asset "$asset"
fi
