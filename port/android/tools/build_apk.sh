#!/bin/sh
# Build the app: libtotk.so (ninja), the Java launcher/activity (javac + d8), manifest (aapt2), signed with the debug key.
#   port/android/tools/build_apk.sh [--install]   -> work/android/v143/apk/totk.apk
# A release (tools/release.sh) sets TOTK_VERSION_NAME / TOTK_VERSION_CODE and the release key: TOTK_KEYSTORE (a .jks),
# TOTK_KEYSTORE_PASSWORD and TOTK_KEY_ALIAS. Without them: version "dev", code = the minutes since 2025-01-01 now,
# counted as a release's is (a PC build made after a release installs over it), signed with this PC's debug key.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); APP=$ROOT/port/android/app
. "$(dirname "$0")/env.sh"; BT=$BUILD_TOOLS; JAR=$ANDROID_JAR; TC=$NDK_BIN
# The game: TOTK_VERSION (v143 default; botw160 for Breath of the Wild, tools/build_botw_apk.sh) picks the configured build
# folder and the data tables; APK_PACKAGE / APK_LABEL / APK_NAME make a second app (its own package, label and file) so
# both games install side by side.
# a release of Tears of the Kingdom is com.winterskycode.totk; PC builds stay dev.tok.totk, so test devices keep their data
V=${TOTK_VERSION:-v143}; DEFAULT_PKG=dev.tok.totk; if [ -n "$TOTK_VERSION_NAME" ] && [ "$V" = v143 ]; then DEFAULT_PKG=com.winterskycode.totk; fi
PKG=${APK_PACKAGE:-$DEFAULT_PKG}; LABEL=${APK_LABEL:-Tears of the Kingdom}; NAME=${APK_NAME:-totk}
B=${TOTK_BUILD_DIR:-$ROOT/work/android/$V/build-host}; OUT=$ROOT/work/android/$V/apk
SETUP_LIB=totksetup; case $V in botw*) SETUP_LIB= ;; esac   # the Game files page: Tears of the Kingdom only (CMakeLists.txt)
ninja -C $B -j$(nproc) totk totkmods totklsfg $SETUP_LIB
rm -rf $OUT/stage && mkdir -p $OUT/stage/classes $OUT/stage/lib/arm64-v8a
# the launcher's game (Game.java KEY: data folder, title, RomFS size, Mods page) is stamped from TOTK_VERSION
mkdir -p $OUT/stage/java && cp -r $APP/java/. $OUT/stage/java/
sed -i "s|static final String KEY = \"v143\";|static final String KEY = \"$V\";|" $OUT/stage/java/dev/tok/totk/Game.java
grep -q "KEY = \"$V\";" $OUT/stage/java/dev/tok/totk/Game.java || { echo "Game.java KEY not stamped" >&2; exit 1; }
javac -Xlint:all -Xlint:-options -source 11 -target 11 -classpath $JAR -d $OUT/stage/classes $(find $OUT/stage/java -name '*.java')
$BT/d8 --min-api 33 --lib $JAR --output $OUT/stage $(find $OUT/stage/classes -name '*.class')
$TC/llvm-strip --strip-debug -o $OUT/stage/lib/arm64-v8a/libtotk.so $B/libtotk.so   # unstripped copy stays in the build folder
$TC/llvm-strip --strip-debug -o $OUT/stage/lib/arm64-v8a/libtotkmods.so $B/libtotkmods.so   # zstd for the Mods page
$TC/llvm-strip --strip-debug -o $OUT/stage/lib/arm64-v8a/libtotklsfg.so $B/libtotklsfg.so   # the shader translator of Tools > Lossless Scaling
[ -z "$SETUP_LIB" ] || $TC/llvm-strip --strip-debug -o $OUT/stage/lib/arm64-v8a/libtotksetup.so $B/libtotksetup.so   # the reader of the Game files page
# the shader translator (NativeAOT, port/xlate/publish_android.sh) ships as a "library" so the installer extracts it as an executable file
cp $ROOT/port/xlate/linux-bionic-arm64/totk_xlate $OUT/stage/lib/arm64-v8a/libtotk_xlate.so
# adrenotools + hooks (load a custom GPU driver such as Turnip; built by d2r-android/dm/android/tools/build_gpu_driver.py)
for l in adrenotools hook_impl main_hook file_redirect_hook gsl_alloc_hook; do [ -f $ROOT/work/android/gpu-driver/lib$l.so ] && cp $ROOT/work/android/gpu-driver/lib$l.so $OUT/stage/lib/arm64-v8a/; done
# Turnip for Adreno 8xx (work/android/turnip, not committed: tools/build_turnip.sh builds it with our a8xx fix); the app uses it when present
[ -f $ROOT/work/android/turnip/libvulkan_freedreno.so ] && cp $ROOT/work/android/turnip/libvulkan_freedreno.so $OUT/stage/lib/arm64-v8a/
# TOTK_VK_VALIDATION=1: bundle the Khronos validation layer (work/tools/vvl); enable it with tools/gpu_debug.sh on
if [ "${TOTK_RENDERDOC:-0}" = 1 ]; then cp $ROOT/work/android/renderdoc/libVkLayer_GLES_RenderDoc.so $OUT/stage/lib/arm64-v8a/; fi   # RenderDoc capture layer (official 1.46 Android build)
if [ "${TOTK_VK_VALIDATION:-0}" = 1 ]; then cp "$(find $ROOT/work/tools/vvl -path '*arm64-v8a*' -name libVkLayer_khronos_validation.so | head -1)" $OUT/stage/lib/arm64-v8a/; fi
# assets/build.txt (generated, not committed): the commit this APK was built from, for bug reports (host/report.c)
rm -rf $OUT/assets-gen && mkdir -p $OUT/assets-gen
{ git -C $ROOT describe --always --dirty 2>/dev/null || echo unknown; date '+built %Y-%m-%d %H:%M'; } > $OUT/assets-gen/build.txt
cp -r $APP/assets/. $OUT/assets-gen/
# the stand-in for the firmware's shared fonts (host/player_files.c) and the PlayStation icons' drawings (IconMod.java): made
# from a fetched font, never committed
if [ "$NAME" = totk ]; then
  sh $ROOT/port/android/tools/fetch_font.sh && cp $ROOT/port/android/deps/out/fonts/SharedFont.otf $OUT/assets-gen/fonts/
  mkdir -p $OUT/assets-gen/icons && cp $ROOT/port/android/deps/out/icons/playstation.bin $OUT/assets-gen/icons/
fi
# assets/licenses: the licence texts of the third-party software in this APK (app/licenses.tsv; Tools > Licences), copied
# from the sources themselves; a header file gives its leading licence comment only
mkdir -p $OUT/assets-gen/licenses; : > $OUT/assets-gen/licenses/index.tsv
grep -v '^#' $APP/licenses.tsv | { n=0; while IFS='	' read -r name lic what files when; do
  [ -z "$name" ] && continue
  [ "$when" != "-" ] && [ ! -f "$ROOT/$when" ] && continue   # not in this APK
  n=$((n + 1)); out=""
  for f in $(echo "$files" | tr ',' ' '); do
    [ -f "$ROOT/$f" ] || { echo "licence file missing for $name: $f" >&2; exit 1; }
    t=$(printf '%02d-%s.txt' $n "$(basename "$f")")
    case $f in *.h) awk '{print} /\*\//{exit}' "$ROOT/$f" ;; *) cat "$ROOT/$f" ;; esac > $OUT/assets-gen/licenses/$t
    out="$out${out:+,}$t"
  done
  printf '%s\t%s\t%s\t%s\n' "$name" "$lic" "$what" "$out" >> $OUT/assets-gen/licenses/index.tsv
done; }
# resources: the launcher icon only (res/, vector drawables); a second app's own icon (res-$NAME, e.g. res-botw) replaces
# the files of the same name
rm -rf $OUT/res-gen && cp -r $APP/res $OUT/res-gen
[ -d "$APP/res-$NAME" ] && cp -r "$APP/res-$NAME/." $OUT/res-gen/
$BT/aapt2 compile --dir $OUT/res-gen -o $OUT/res.zip
# the manifest for this app: package, label and the report provider's authority (the Java package stays dev.tok.totk)
sed -e "s|package=\"dev.tok.totk\"|package=\"$PKG\"|" -e "s|android:label=\"Tears of the Kingdom\"|android:label=\"$LABEL\"|" \
    -e "s|android:authorities=\"dev.tok.totk.reports\"|android:authorities=\"$PKG.reports\"|" \
    -e 's|android:name="\.|android:name="dev.tok.totk.|g' $APP/AndroidManifest.xml > $OUT/AndroidManifest.xml
# a release is not debuggable: run-as would read the app's private files, the player's keys among them. It has no all-files
# access either: its game files come through the picker. Breath of the Wild has no setup yet and keeps it.
if [ -n "$TOTK_VERSION_NAME" ]; then
  sed -i 's/android:debuggable="true"/android:debuggable="false"/' $OUT/AndroidManifest.xml
  case $V in botw*) ;; *) sed -i '/MANAGE_EXTERNAL_STORAGE/d' $OUT/AndroidManifest.xml ;; esac
fi
$BT/aapt2 link -o $OUT/base.apk --manifest $OUT/AndroidManifest.xml -A $OUT/assets-gen -I $JAR --min-sdk-version 33 --target-sdk-version 35 \
  --version-name "${TOTK_VERSION_NAME:-dev}" --version-code "${TOTK_VERSION_CODE:-$(( ($(date +%s) - 1735689600) / 60 ))}" $OUT/res.zip
(cd $OUT/stage && zip -q -r ../base.apk classes.dex lib)
$BT/zipalign -f -p 16 $OUT/base.apk $OUT/aligned.apk
if [ -n "$TOTK_KEYSTORE" ]; then   # the release key (passwords from the environment, never on the command line)
  $BT/apksigner sign --ks "$TOTK_KEYSTORE" --ks-key-alias "${TOTK_KEY_ALIAS:-totk}" --ks-pass env:TOTK_KEYSTORE_PASSWORD --key-pass env:TOTK_KEYSTORE_PASSWORD \
    --out $OUT/$NAME.apk $OUT/aligned.apk
else
  KS=$HOME/.android/debug.keystore
  [ -f $KS ] || { mkdir -p $HOME/.android; keytool -genkeypair -keystore $KS -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1; }   # as Android's tools make it
  $BT/apksigner sign --ks $KS --ks-pass pass:android --key-pass pass:android --out $OUT/$NAME.apk $OUT/aligned.apk
fi
echo "built $OUT/$NAME.apk ($(du -h $OUT/$NAME.apk | cut -f1))"
if [ "$1" = "--install" ]; then
  adb install -r $OUT/$NAME.apk
  adb shell appops set --uid $PKG MANAGE_EXTERNAL_STORAGE allow   # all-files access for /sdcard/tok (personal build)
  # the device setting that loads the validation layer aborts the app at start when the APK does not carry the layer
  [ "$TOTK_VK_VALIDATION" = 1 ] || [ "$TOTK_RENDERDOC" = 1 ] || "$(dirname "$0")/gpu_debug.sh" off > /dev/null
fi
