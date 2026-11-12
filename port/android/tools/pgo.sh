#!/bin/bash
# The profile release builds optimise libtotk.so with (pgo/totk.proftext; CMakeLists.txt TOTK_PGO, tools/release.sh):
#   port/android/tools/pgo.sh        (the device: ANDROID_SERIAL; the 100% save of bench.py)
# builds the instrumented library (TOTK_PGO=generate), installs it, flies the benchmark route once (host/pgo.c writes the
# counters every 30 s after the first load), merges what the device wrote and stores it as text with this checkout's
# path replaced by @PGO_ROOT@, so the file matches any checkout and holds no local path. A profile from older code still
# builds; it only helps less where the code changed. Reinstall a day-to-day build afterwards: the instrumented one is slower.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); T=$ROOT/port/android/tools
. "$T/env.sh"
B=$ROOT/work/android/v143/build-pgo-gen; W=$ROOT/work/android/v143/pgo
cmake -G Ninja -S "$ROOT/port/android" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DTOTK_VERSION=v143 -DCMAKE_BUILD_TYPE=Release -DTOTK_PGO=generate > /dev/null
TOTK_BUILD_DIR=$B sh "$T/build_apk.sh"
adb install -r -d "$ROOT/work/android/v143/apk/totk.apk"
adb shell 'run-as dev.tok.totk rm -rf files/pgo'
python3 -I "$T/bench.py" --label pgo-collect
mkdir -p "$W"
adb exec-out run-as dev.tok.totk cat files/pgo/totk.profraw > "$W/totk.profraw"
[ -s "$W/totk.profraw" ] || { echo "no profile on the device (files/pgo/totk.profraw)" >&2; exit 1; }
"$NDK_BIN/llvm-profdata" merge -o "$W/totk.profdata" "$W/totk.profraw"
"$NDK_BIN/llvm-profdata" merge --text -o "$W/totk.proftext" "$W/totk.profdata"
sed "s|$ROOT/|@PGO_ROOT@/|g" "$W/totk.proftext" > "$ROOT/port/android/pgo/totk.proftext"
if grep -q "$ROOT" "$ROOT/port/android/pgo/totk.proftext"; then echo "a local path is left in pgo/totk.proftext" >&2; exit 1; fi
echo "pgo/totk.proftext: $(grep -c '^@PGO_ROOT@' "$ROOT/port/android/pgo/totk.proftext") functions"
