#!/bin/bash
# Setup.java on this PC, no device: port/android/tools/setup_test/run.sh [work folder]
# Its logic on fake readers, then the real reader through host/setup_jni.c, built here for the PC, on made-up game files
# (setup/container/tests/synth.h: made-up keys, no game content) and on damaged ones.
# TOTK_SETUP_REAL=<folder> also runs it on a real folder holding prod.keys and the two game files, read-only. Key values are
# never printed, and nothing from that folder is written anywhere.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
android=$(cd "$here/../.." && pwd)
reader=$android/setup/container
work=${1:-$(mktemp -d)}
mkdir -p "$work/classes" "$work/fakes"
java_home=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")

lib=("$reader/aes.c" "$reader/aes_hw.c" "$reader/sha256.c" "$reader/keys.c" "$reader/package.c" "$reader/bktr.c" "$reader/game.c" "$reader/romfs_index.c")
cc -O1 -g -Wall -shared -fPIC -I"$reader" -I"$java_home/include" -I"$java_home/include/linux" -o "$work/libtotksetup.so" "$android/host/setup_jni.c" "${lib[@]}"
cc -O1 -g -I"$reader" -I"$reader/tests" -o "$work/make_fakes" "$here/make_fakes.c" "$reader/tests/synth.c" "${lib[@]}"
"$work/make_fakes" "$work/fakes"

java_src=$android/app/java/dev/tok/totk
javac -Xlint:all -Xlint:-options -source 11 -target 11 -d "$work/classes" "$java_src/Game.java" "$java_src/Setup.java" "$java_src/SetupNative.java" "$here/SetupTest.java"
java --add-opens java.base/java.io=ALL-UNNAMED -Djava.library.path="$work" -cp "$work/classes" dev.tok.totk.SetupTest "$work"
