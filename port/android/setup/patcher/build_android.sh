#!/bin/sh
# The rewriter for Android arm64 with the pinned NDK: libtokpatcher.a, libtok_capstone.a and patcher_cli.
# usage: build_android.sh <build dir> [capstone source dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../../tools/env.sh"
OUT=${1:?usage: build_android.sh <build dir> [capstone source dir]}
CAP=${2:-$HERE/../../deps/src/capstone}
[ -f "$CAP/cs.c" ] || sh "$HERE/fetch_capstone.sh" "$CAP"
cmake -G Ninja -S "$HERE" -B "$OUT" -DCAPSTONE_SRC="$CAP" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build "$OUT" -j "$(nproc)"
