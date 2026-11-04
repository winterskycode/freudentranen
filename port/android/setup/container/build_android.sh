#!/bin/sh
# The reader for Android arm64 with the pinned NDK: libtokcontainer.a and container_cli.
# usage: build_android.sh <build dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../../tools/env.sh"
OUT=${1:?usage: build_android.sh <build dir>}
cmake -G Ninja -S "$HERE" -B "$OUT" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build "$OUT" -j "$(nproc)"
