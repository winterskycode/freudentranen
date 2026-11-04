#!/bin/sh
# A new player's walk: a copy of the save, an empty pipeline cache, load, then 54 s of scripted sticks; the game is stopped
# at the end. Usage: cold_walk.sh <output dir> [keep] [extra debug.env lines, \n separated]
#   keep 0: empty pipeline and Mesa caches; 2 (default): Mesa cache copied from files/mesa-cache (prepared), empty pipeline
#   cache; 1: both caches kept from the previous walk (every pipeline built).
#   REC=1 records the screen to <output dir>/rec.mp4. Output: totk.log, end.png.
O=$1; KEEP=${2:-2}; EXTRA=$3; DIR=$(dirname "$0"); [ -n "$O" ] || { echo "usage: $0 <output dir> [keep] [env]" >&2; exit 2; }
mkdir -p "$O"; F=/data/data/dev.tok.totk/files
adb shell am force-stop dev.tok.totk
adb shell "run-as dev.tok.totk sh -c 'rm -rf files/walk-profile; [ $KEEP = 1 ] || rm -rf files/walk-pc files/walk-mesa; [ $KEEP = 2 ] && cp -a files/mesa-cache files/walk-mesa; mkdir -p files/walk-pc files/walk-mesa files/walk-profile && cp -a files/profile/save files/profile/account.uid files/walk-profile/; printf \"TOTK_PROFILE_DIR=$F/walk-profile\nDM_VK_PIPELINE_CACHE_DIR=$F/walk-pc\nMESA_SHADER_CACHE_DIR=$F/walk-mesa\n$EXTRA\n\" > files/debug.env'"
finish() { adb shell am force-stop dev.tok.totk; adb shell 'run-as dev.tok.totk sh -c ": > files/debug.env"'; }
"$DIR/to_cave.sh" 15 > "$O/start.log" 2>&1 || { echo "start failed"; tail -3 "$O/start.log"; finish; exit 1; }
p() { adb shell run-as dev.tok.totk sh -c "'echo $1 > files/input.now'"; }
[ -n "$REC" ] && { adb shell screenrecord --time-limit 60 --bit-rate 8000000 /sdcard/walk_rec.mp4 & }
for i in 1 2 3 4 5 6; do p "ly 32000 4000"; sleep 2; p "rx 25000 2000"; sleep 3; p "lx 30000 3000"; sleep 4; done
adb exec-out run-as dev.tok.totk cat files/profile/totk.log > "$O/totk.log"
adb exec-out screencap -p > "$O/end.png"
adb shell pidof dev.tok.totk:game > /dev/null && echo alive || echo DIED
finish
[ -n "$REC" ] && { while adb shell pidof screenrecord > /dev/null; do sleep 1; done; adb pull -q /sdcard/walk_rec.mp4 "$O/rec.mp4"; adb shell rm -f /sdcard/walk_rec.mp4; }
echo "done: $(grep -a -c '\[hitch\]' "$O/totk.log") hitches"
