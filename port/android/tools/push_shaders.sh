#!/bin/sh
# Put a shader cache made ahead of time (tools/aot_shaders.py) on the device as the app's Turnip shader cache (its
# bindless cache when the request profile asks for bindless translations).
#   port/android/tools/push_shaders.sh work/android/v143/shader-aot/<digest>
# Streamed into files/shader-cache-turnip.new, then swapped in with the app stopped; the cache it replaces is kept as
# files/shader-cache-turnip.old (one generation: an older .old is removed, unless there is no cache to keep).
# Translations the device makes later are added to the new cache as before.
set -e
DIR=${1:?usage: push_shaders.sh <shader-aot folder>}
[ -f "$DIR/request-profile.json" ] || { echo "$DIR: not an aot_shaders.py output (no request-profile.json)"; exit 1; }
APP=dev.tok.totk; C=files/shader-cache-turnip
# a cache made for TOTK_VK_BINDLESS (vk/nvn_vulkan_bindless.inc) goes to the app's separate bindless cache
grep -q '"bindless": true' "$DIR/request-profile.json" && C=$C-bindless
adb shell am force-stop $APP
adb shell "run-as $APP sh -c 'rm -rf $C.new && mkdir -p $C.new'"
echo "sending $(du -sh "$DIR" | cut -f1) ..."
tar -C "$DIR" --exclude='*.code' --exclude=failures.txt -cf - . | adb exec-in run-as $APP sh -c "tar -C $C.new -xf -"
adb shell "run-as $APP sh -c 'test -f $C.new/request-profile.json && { test ! -d $C || { rm -rf $C.old && mv $C $C.old; }; } && mv $C.new $C && ls $C | wc -l'"
