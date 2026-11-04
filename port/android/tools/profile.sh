#!/bin/sh
# Frame-time profile of the latest save (to_cave.sh) while a scripted route runs and turns the camera: one
# [frameprof] line per frame (translation-thread CPU, draws, dependency waits, pipeline/image/texture creations),
# every thread's CPU time over the run (/proc), and a simpleperf sample of the whole game process (call graphs).
# Waits for the device to cool first (thermal status <= 1). Uncapped, no frame generation, HUD off.
#   port/android/tools/profile.sh <name> [seconds, default 30] [extra debug.env lines...]
#   NOPERF=1: no simpleperf (GPU / frame-time A/Bs; pulling perf.data over Wi-Fi takes minutes)
#   ROUTE=still: no input (Link stands where the save puts him): the same scene every run, for A/B comparisons
#   -> work/android/v143/profile/<name>/ (totk.log, threads-start/-end.txt, perf.data); tools/profile_report.py reads it
set -e
DIR=$(dirname "$0"); ROOT=$(cd "$DIR/../../.." && pwd); NAME=${1:?name}; SECS=${2:-30}; shift; [ $# -gt 0 ] && shift
OUT=$ROOT/work/android/v143/profile/$NAME; rm -rf "$OUT"; mkdir -p "$OUT"
thermal() { adb shell dumpsys thermalservice | grep -m1 "Thermal Status" | grep -o "[0-9]*"; }
# SWEEP_PHASE=<file> (ab_sweep.sh sets it): "<phase>\t<start epoch>\t<expected seconds>" for the status line
phase() { [ -n "$SWEEP_PHASE" ] && printf '%s\t%s\t%s\n' "$1" "$(date +%s)" "${2:-0}" > "$SWEEP_PHASE"; return 0; }
# a pause between runs (the device is free: the game is stopped): touch work/android/v143/profile/.sweep-pause, rm to resume
while [ -f "$ROOT/work/android/v143/profile/.sweep-pause" ]; do phase "paused (rm .sweep-pause to resume)"; sleep 5; done
adb shell am force-stop dev.tok.totk
for i in $(seq 1 90); do t=$(thermal); [ "$t" -le 1 ] && break; [ "$i" = 1 ] && phase "cooling down (thermal $t)"; sleep 5; done
echo "thermal before: $(thermal)"
# the device's fan and performance modes change frame times by 10-15%: record them with every run
# (the fan driver, /sys/class/gpio5_pwm2: state 1 = spinning, duty out of period = power, speed ~ rpm)
fan() { adb shell 'f=/sys/class/gpio5_pwm2; echo "fan $(cat $f/state) $(( $(cat $f/duty) * 100 / $(cat $f/period) ))% $(cat $f/speed)rpm mode $(settings get system fan_mode) perf $(settings get system performance_mode)"' | tr -d '\r'; }
fan > "$OUT/device.txt"
case "$(cat "$OUT/device.txt")" in "fan 0"*) echo "WARNING: the fan is off ($(cat "$OUT/device.txt")): frame times will not compare" >&2;; esac
# the benchmark fan mode (settings fan_mode; 4 = the mode the 2026-10 baselines ran in): another mode, set on the device
# or by its own UI, makes frame times and thermals incomparable (a run in mode 1 ended at thermal 5 with the fan at 10%)
case "$(cat "$OUT/device.txt")" in *"mode ${BENCH_FAN_MODE:-4} "*) ;; *) echo "WARNING: fan mode is not ${BENCH_FAN_MODE:-4} ($(cat "$OUT/device.txt")): frame times will not compare (BENCH_FAN_MODE=<n> to accept another)" >&2;; esac
# The benchmark save: files/bench-save.orig (a copy of the profile whose newest slot is the still-scene save, made
# 2026-10-23 from slot_03 when the user's own play added a newer save) is restored to files/bench-profile before every
# run and used through TOTK_PROFILE_DIR, so "Continue" always loads the same spot and the user's saves are not touched.
# BENCH_PROFILE=0: the user's profile (the latest save) instead.
BENCH=""
if [ "${BENCH_PROFILE:-1}" != 0 ] && adb shell run-as dev.tok.totk test -d files/bench-save.orig; then
    # copied over in place (no recursive delete); if a run left files the original lacks (other than the game's
    # empty temp/), a fresh directory instead
    B=bench-profile
    extra=$(adb shell run-as dev.tok.totk sh -c "'cd files/bench-profile 2>/dev/null && find . ! -path ./temp | while read f; do [ -e ../bench-save.orig/\$f ] || echo \$f; done'" | tr -d '\r')
    [ -n "$extra" ] && B=bench-profile-$(date +%s) && echo "bench-profile has extra files; using files/$B (delete old ones by hand)" >&2
    adb shell run-as dev.tok.totk sh -c "'mkdir -p files/$B && cp -a files/bench-save.orig/. files/$B/'"
    BENCH="TOTK_PROFILE_DIR=/data/data/dev.tok.totk/files/$B"
fi
{ printf 'TOTK_VK_FRAMEPROF=1\nTOTK_UNCAPPED_HZ=120\nTOTK_DISPLAY_HZ=120\nTOTK_FRAMEGEN=0\nTOTK_LSFG=0\nTOTK_HUD=off\n'; for e in "$@"; do echo "$e"; done; [ -n "$BENCH" ] && echo "$BENCH"; } |
    adb shell run-as dev.tok.totk sh -c "'cat > files/debug.env'"
phase "loading the save" 50
"$DIR/to_cave.sh" 10 > /dev/null
# GFX_CMD="<line>[;<line>...]": input.now lines sent once the save has loaded (game_patches_v143.c gfx* commands: poke
# the game's graphics settings for an A/B), each given 2 s to apply before the recording starts
if [ -n "$GFX_CMD" ]; then echo "$GFX_CMD" | tr ';' '\n' | while read -r c; do
    adb shell run-as dev.tok.totk sh -c "'echo \"$c\" > files/input.now'"; sleep 2; done; fi
PID=$(adb shell pidof dev.tok.totk:game)
# per thread: tid, name, utime, stime (clock ticks; the name in stat may hold spaces, so fields are counted after its ')')
threads() { adb shell "for t in /proc/$PID/task/*; do echo \${t##*/} \$(cat \$t/comm 2>/dev/null | tr ' ' _) \$(sed 's/.*) //' \$t/stat 2>/dev/null | cut -d' ' -f12,13); done"; }
START=$(adb shell run-as dev.tok.totk sh -c "'grep -c . files/profile/totk.log'")
threads > "$OUT/threads-start.txt"
adb shell run-as dev.tok.totk rm -f files/perf.data
phase "recording" "$SECS"
# as the app: the shell user may not attach to the app's process
if [ -n "$NOPERF" ]; then sleep "$SECS" & REC=$!
else adb shell run-as dev.tok.totk simpleperf record -p "$PID" --call-graph fp -f 2000 --duration "$SECS" -o files/perf.data > "$OUT/simpleperf.txt" 2>&1 &
REC=$!; fi
# the route: run forward while turning, then sideways the other way; repeated until the recording ends
end=$(( $(date +%s) + SECS )); [ "${ROUTE:-walk}" = still ] && end=0
while [ "$(date +%s)" -lt "$end" ]; do
    adb shell run-as dev.tok.totk sh -c "'printf \"ly 32000 3000\nrx 14000 3000\n\" > files/input.now'"; sleep 3
    adb shell run-as dev.tok.totk sh -c "'printf \"lx -30000 2500\nrx -16000 2500\nb 2500\n\" > files/input.now'"; sleep 3
done
wait $REC || true
phase "collecting results" 10
fan >> "$OUT/device.txt"   # and at the end of the recording
threads > "$OUT/threads-end.txt"
adb shell run-as dev.tok.totk cat files/profile/totk.log > "$OUT/totk.log"
tail -n +"$START" "$OUT/totk.log" > "$OUT/run.log"
[ -z "$NOPERF" ] && { adb exec-out run-as dev.tok.totk cat files/perf.data > "$OUT/perf.data"; adb shell run-as dev.tok.totk rm -f files/perf.data; }
echo "thermal after: $(thermal)   display: $(adb shell dumpsys display | grep -m1 -o "mActiveRenderFrameRate=[0-9]*")   $(tail -1 "$OUT/device.txt")"
printf '' | adb shell run-as dev.tok.totk sh -c "'cat > files/debug.env'"
adb shell am force-stop dev.tok.totk
echo "profile in $OUT ($(grep -c '^\[dm\] \[frameprof\] frame=' "$OUT/run.log") frames)"
