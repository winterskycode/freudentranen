#!/bin/sh
# NativeAOT build of the shader translator for Android (linux-bionic-arm64), linked with the NDK's clang, so the game can
# translate shaders on the device (packaged in the APK as lib/arm64-v8a/libtotk_xlate.so and started as a program).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/port/android/tools/env.sh"; TC=$NDK_BIN
export PATH=$TC:$PATH
# the ILCompiler looks for a clang it can drive for the target; point it at the NDK's API-35 wrapper
mkdir -p $ROOT/port/xlate/ndk-bin
# a script, not a link to the NDK's API-level wrapper: that wrapper runs "clang" from PATH, which would be this file again
rm -f $ROOT/port/xlate/ndk-bin/clang
printf '#!/bin/sh\nexec %s/clang --target=aarch64-linux-android35 "$@"\n' "$TC" > $ROOT/port/xlate/ndk-bin/clang
chmod +x $ROOT/port/xlate/ndk-bin/clang
ln -sf $TC/llvm-objcopy $ROOT/port/xlate/ndk-bin/objcopy
export PATH=$ROOT/port/xlate/ndk-bin:$PATH
"$(dirname "$0")/publish.sh" linux-bionic-arm64 2>&1
