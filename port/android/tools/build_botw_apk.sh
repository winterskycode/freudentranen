#!/bin/sh
# Breath of the Wild 1.6.0 as its own app (package dev.tok.botw, "Breath of the Wild", botw.apk), from the same runtime,
# NVN layer, compat layer and host code as Tears of the Kingdom; only the game module patches, the game's addresses
# (runtime/game_version.h) and the data folder /sdcard/tok/botw160 differ. Once: configure the native build
#   cmake -G Ninja -S port/android -B work/android/botw160/build-host -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DTOTK_VERSION=botw160 -DCMAKE_BUILD_TYPE=Release
# and make the game data (ExeFS, RomFS merge, DLC; then tools/gen_runtime.py --version botw160 and
# TOTK_VERSION=botw160 tools/push_data.sh). usage: port/android/tools/build_botw_apk.sh [--install]
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
[ -d "$ROOT/work/android/botw160/build-host" ] || { echo "configure work/android/botw160/build-host first (see the header)"; exit 1; }
TOTK_VERSION=botw160 APK_PACKAGE=dev.tok.botw APK_LABEL="Breath of the Wild" APK_NAME=botw exec "$(dirname "$0")/build_apk.sh" "$@"
