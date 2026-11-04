#!/bin/bash
# Freeze-frame debugging on the device (see nvn_vulkan_freeze.inc and hle_nvn.c "freeze-frame"); TOTK_APP picks the app.
#   frz.sh on                 freeze the current frame (it replays identically until "off")
#   frz.sh off                resume the game
#   frz.sh shot <file.png>    screenshot of the frozen frame
#   frz.sh probe [keys] [dir] probe the next replay (pipeline.tsv, images; after-draw dumps for the comma list of
#                             fragment shader keys) and pull it to dir (default: work/android/probe/<n>)
#   frz.sh magenta [keys|n:k] fragment shaders drawn solid magenta (no argument: none)
#   frz.sh override <key> <file.spv>   replace a shader's SPIR-V live;  frz.sh override <key>  restore it
#   frz.sh spv <key> <out.spv>         the shader's current translation from the device's shader cache
#   frz.sh px <fx> <fy> [dir]          pixel history: that relative position of every target after every draw (pixels.tsv)
#   frz.sh live [words]                live experiment words (files/exp.live, nvn_vulkan_forward.inc), e.g. "restart@0-2999";
#                                      no words: none. Each replay uses them, so a frozen frame can be compared with and without.
set -e
APP=${TOTK_APP:-dev.tok.totk}   # the BotW app: TOTK_APP=dev.tok.botw
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
run() { adb shell run-as $APP sh -c "'$1'"; }
case "$1" in
on) run "touch files/freeze"
    for i in $(seq 1 50); do adb logcat -d | grep -q "\[freeze\] frozen" && break; sleep 0.2; done
    adb logcat -d | grep "\[freeze\]" | tail -4 | sed 's/.*\[freeze\]/[freeze]/' ;;
off) run "rm -f files/freeze"; echo resumed ;;
shot) adb exec-out screencap -p > "${2:-frozen.png}"; echo "${2:-frozen.png}" ;;
live) if [ -n "$2" ]; then printf '%s\n' "$2" | adb shell run-as $APP sh -c "'cat > files/exp.live'"; else run "rm -f files/exp.live"; fi; sleep 1.5; echo "live: ${2:-none}" ;;
magenta) printf '%s' "$2" | adb shell run-as $APP sh -c "'cat > files/magenta.txt'"; sleep 4; echo "magenta: ${2:-off}" ;;
probe)
    run "rm -rf files/liveprobe/*"
    printf '%s\n' "$2" | adb shell run-as $APP sh -c "'cat > files/probe.trigger'"
    n=; for i in $(seq 1 150); do n=$(run "ls files/liveprobe/*/done 2>/dev/null" | head -1 | sed 's|files/liveprobe/||; s|/done||'); [ -n "$n" ] && break; sleep 0.2; done
    [ -n "$n" ] || { echo "probe did not finish"; exit 1; }
    out=${3:-$ROOT/work/android/probe/$n}; rm -rf "$out"; mkdir -p "$out"
    adb exec-out run-as $APP sh -c "cd files/liveprobe/$n && tar cf - ." 2>/dev/null | tar xf - -C "$out" 2>/dev/null
    echo "$out ($(ls "$out" | grep -c ^after) after-dumps)" ;;
px) exec "$0" probe "px:$2,$3" "$4" ;;
override)
    run "mkdir -p files/override"
    if [ -n "$3" ]; then adb push "$3" /data/local/tmp/ovr.spv >/dev/null; run "cp /data/local/tmp/ovr.spv files/override/$2.spv"; else run "rm -f files/override/$2.spv"; fi
    sleep 1.5; adb logcat -d | grep "\[override\] $2" | tail -1 | sed 's/.*\[override\]/[override]/' ;;
spv) c=$(adb shell run-as $APP sh -c "'ls files/shader-cache-turnip/$2.spv files/shader-cache/$2.spv 2>/dev/null'" | head -1)
    adb exec-out run-as $APP cat "$c" > "$3"; echo "$3 ($(stat -c %s "$3") bytes, from $c)" ;;
*) sed -n 2,12p "$0"; exit 1 ;;
esac
