#!/bin/sh
# The tables for Android arm64 with the pinned NDK: libtoktables.a and tables_cli, with the patcher and the container reader.
# usage: build_android.sh <build dir> [capstone source dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../../tools/env.sh"
OUT=${1:?usage: build_android.sh <build dir> [capstone source dir]}
CAP=${2:-$HERE/../../deps/src/capstone}
[ -f "$CAP/cs.c" ] || sh "$HERE/../patcher/fetch_capstone.sh" "$CAP"
cmake -G Ninja -S "$HERE" -B "$OUT" -DCAPSTONE_SRC="$CAP" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build "$OUT" -j "$(nproc)"
