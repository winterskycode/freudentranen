#!/bin/sh
# A release APK from a checkout of this repository: every part built from source, then build_apk.sh with the release's
# version and key, checked, into dist/. CI runs it for each tag (.github/workflows/release.yml); it runs on the PC too.
#   port/android/tools/release.sh <tag>   (tag YYYYMMDD-HHMM, e.g. 20261029-1325)   -> dist/totk-<tag>.apk, .sha256 and
#   totk-<tag>-source.tar.xz, the complete corresponding source (AGPL-3.0 section 6; NOTICE): this repository at HEAD and
#   the exact upstream sources the build used, so a release can be rebuilt even if an upstream goes away
# The app's version name is the tag; its version code is the minutes from 2025-01-01 00:00 to the tag's date and time, so
# every later tag is a larger code (Android only updates to a larger one).
# Signing: TOTK_KEYSTORE (the release .jks), TOTK_KEYSTORE_PASSWORD, TOTK_KEY_ALIAS. TOTK_RELEASE_DEBUG_KEY=1
# signs a test build with this PC's debug key instead.
# Needs: the Android SDK with the NDK, build-tools and platform of tools/env.sh (ANDROID_SDK_ROOT), git, curl, python3 with
# venv, cmake, ninja, pkg-config, bison, flex, glslangValidator, a JDK (17+), zip and unzip (tools/ci-packages.txt).
# No game files: the APK carries none, and nothing in its build is made from them.
set -e
TAG=$1
case $TAG in [0-9][0-9][0-9][0-9][01][0-9][0-3][0-9]-[0-2][0-9][0-5][0-9]) ;; *) echo "usage: release.sh YYYYMMDD-HHMM" >&2; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); T=$ROOT/port/android/tools
. "$T/env.sh"
CODE=$(python3 -c "import datetime,sys; t=datetime.datetime.strptime(sys.argv[1], '%Y%m%d-%H%M'); print(int((t - datetime.datetime(2025, 1, 1)).total_seconds()) // 60)" "$TAG")
if [ -z "$TOTK_KEYSTORE" ] && [ "$TOTK_RELEASE_DEBUG_KEY" != 1 ]; then echo "no release key: set TOTK_KEYSTORE and TOTK_KEYSTORE_PASSWORD" >&2; exit 2; fi
for p in "$NDK_BIN/clang" "$BUILD_TOOLS/apksigner" "$ANDROID_JAR"; do [ -e "$p" ] || { echo "missing from the Android SDK: $p" >&2; exit 2; }; done
step() { echo "== $(date +%H:%M:%S) $*"; }

step "MacNativeGames at the pinned commit of our fork"
sh "$ROOT/port/setup.sh"
step "Opus, libvpx, zstd"
sh "$ROOT/port/android/deps/build_deps.sh" > /dev/null
step "libadrenotools and hooks"
python3 "$T/build_gpu_driver.py"
step "Turnip"
sh "$T/build_turnip.sh"
step "shader translator (.NET NativeAOT)"
DOTNET_VERSION=10.0.402
if ! "$ROOT/tools/dotnet/dotnet" --list-sdks 2>/dev/null | grep -q "^$DOTNET_VERSION "; then
  mkdir -p "$ROOT/tools"; curl -sSfL https://dot.net/v1/dotnet-install.sh -o "$ROOT/tools/dotnet-install.sh"
  bash "$ROOT/tools/dotnet-install.sh" --version $DOTNET_VERSION --install-dir "$ROOT/tools/dotnet" --no-path > /dev/null
fi
DOTNET=$ROOT/tools/dotnet/dotnet sh "$ROOT/port/xlate/publish_android.sh" | tail -1
step "native libraries"
B=$ROOT/work/android/v143/build-host
cmake -G Ninja -S "$ROOT/port/android" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DTOTK_VERSION=v143 -DCMAKE_BUILD_TYPE=Release > /dev/null
step "APK $TAG (version code $CODE)"
TOTK_VERSION_NAME=$TAG TOTK_VERSION_CODE=$CODE sh "$T/build_apk.sh"

step "checks"
APK=$ROOT/work/android/v143/apk/totk.apk
LIBS=$(unzip -Z1 "$APK" 'lib/arm64-v8a/*')
for l in libtotk.so libtotkmods.so libtotklsfg.so libtotk_xlate.so libvulkan_freedreno.so libadrenotools.so libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so; do
  echo "$LIBS" | grep -qx "lib/arm64-v8a/$l" || { echo "the APK lacks lib/arm64-v8a/$l" >&2; exit 1; }
done
for l in libVkLayer_khronos_validation.so libVkLayer_GLES_RenderDoc.so; do
  echo "$LIBS" | grep -qx "lib/arm64-v8a/$l" && { echo "a debugging layer ($l) is in the release APK" >&2; exit 1; }
done
[ "$(unzip -p "$APK" assets/licenses/index.tsv | wc -l)" -ge 20 ] || { echo "the licence notices are incomplete" >&2; exit 1; }
"$BUILD_TOOLS/apksigner" verify "$APK"
"$BUILD_TOOLS/aapt2" dump badging "$APK" | grep -q "versionCode='$CODE' versionName='$TAG'" || { echo "the APK's version is not $TAG ($CODE)" >&2; exit 1; }
mkdir -p "$ROOT/dist"; cp "$APK" "$ROOT/dist/totk-$TAG.apk"
step "source archive"
git -C "$ROOT" diff --quiet HEAD || echo "warning: uncommitted changes are in the APK but not in the source archive (HEAD)" >&2
S=$ROOT/work/release-source; N=totk-$TAG-source; rm -rf "$S"; mkdir -p "$S/$N/upstream"
git -C "$ROOT" archive --prefix=totk/ HEAD | tar -x -C "$S/$N"
MNG=$(sed -n 's/^COMMIT=//p' "$ROOT/port/setup.sh")   # the parts of MacNativeGames the build uses
git -C "$ROOT/MacNativeGames" archive --prefix=upstream/MacNativeGames/ "$MNG" LICENSE README.md THIRD_PARTY_NOTICES.md games/totk vendor |
  tar -x -C "$S/$N"
MESA=$(sed -n 's/.*COMMIT=\([0-9a-f]*\).*/\1/p' "$T/build_turnip.sh")
git -C "$ROOT/work/turnip-build/mesa" archive --prefix=upstream/mesa/ "$MESA" | tar -x -C "$S/$N"
for d in opus zstd libvpx capstone; do git -C "$ROOT/port/android/deps/src/$d" archive --prefix=upstream/$d/ HEAD | tar -x -C "$S/$N"; done
cp -r "$ROOT/work/android/gpu-driver-src/." "$S/$N/upstream/"
cp -r "$ROOT/port/android/deps/src/noto-sans-cjk" "$S/$N/upstream/"
cat > "$S/$N/SOURCE.md" <<EOF
# Source of release $TAG

The complete corresponding source of \`totk-$TAG.apk\` (GNU AGPL-3.0, section 6; \`totk/NOTICE\`).

- \`totk/\`: this project at the release ($(git -C "$ROOT" describe --always HEAD)).
- \`upstream/MacNativeGames/\`: MacNativeGames at $MNG, the parts the build uses (\`totk/port/setup.sh\` fetches it from the
  android branch of github.com/winterskycode/MacNativeGames).
- \`upstream/mesa/\`: whitebelyash/mesa-unified at $MESA (\`totk/port/android/tools/build_turnip.sh\` applies
  \`totk/patches/turnip-a8xx-pvtmem-stack-pad.patch\` and its NDK build fixes).
- \`upstream/opus/\`, \`upstream/zstd/\`, \`upstream/libvpx/\`: the tags in \`totk/port/android/deps/build_deps.sh\`.
- \`upstream/capstone/\`: the tag in \`totk/port/android/setup/patcher/fetch_capstone.sh\`.
- \`upstream/noto-sans-cjk/\`: the font \`totk/port/android/tools/fetch_font.sh\` fetches and cuts down.
- \`upstream/libadrenotools-*/\`, \`upstream/liblinkernsbypass-*/\`: the commits in
  \`totk/port/android/tools/build_gpu_driver.py\`.

To build, put the upstream trees where the scripts expect them (\`MacNativeGames/\`, \`work/turnip-build/mesa/\`,
\`port/android/deps/src/<name>/\`, \`work/android/gpu-driver-src/<name>/\`), then follow \`totk/BUILDING.md\`. The
toolchains are the published versions pinned in \`totk/port/android/tools/env.sh\` (Android NDK and SDK) and
\`totk/port/android/tools/release.sh\` (.NET SDK).
EOF
tar -C "$S" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - "$N" | xz -T0 -6 > "$ROOT/dist/$N.tar.xz"
rm -rf "$S"
(cd "$ROOT/dist" && sha256sum "totk-$TAG.apk" > "totk-$TAG.apk.sha256" && sha256sum "$N.tar.xz" > "$N.tar.xz.sha256")
step "dist/totk-$TAG.apk ($(du -h "$ROOT/dist/totk-$TAG.apk" | cut -f1)), source $(du -h "$ROOT/dist/totk-$TAG-source.tar.xz" | cut -f1), signed by:"
"$BUILD_TOOLS/apksigner" verify --print-certs "$APK" | grep -E "certificate (DN|SHA-256)"
