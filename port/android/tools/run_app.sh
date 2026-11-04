#!/bin/sh
# Launch the game through the launcher on the Odin 3, let it run, and collect its log.
#   port/android/tools/run_app.sh [seconds] [grep pattern]   -> work/android/v143/logs/app-<time>.log (the app's totk.log)
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); SECS=${1:-45}; PAT=${2:-'FATAL|signal=|translated [0-9]+:|【红】'}
adb shell am force-stop dev.tok.totk
adb shell run-as dev.tok.totk rm -f files/profile/totk.log   # never mistake an older run's log for this one
adb logcat -c
adb shell am start -W --display 0 -n dev.tok.totk/.LauncherActivity > /dev/null
top() { adb shell dumpsys activity activities | grep -q "topResumedActivity=.*dev.tok.totk/.LauncherActivity"; }
for i in $(seq 1 40); do top && break; sleep 0.5; done
sleep 3   # the launcher needs a moment after resuming before it takes input
# never press blind: with another app in front the key would go to it
if ! top; then echo "launcher is not in front; not tapping" >&2; exit 1; fi
adb shell input -d 0 keyevent KEYCODE_DPAD_CENTER   # the main screen (the Thor has two): "Start game" (focused on the startup screen)
for i in $(seq 1 20); do adb shell pidof dev.tok.totk:game > /dev/null && break; sleep 0.5; done
adb shell pidof dev.tok.totk:game > /dev/null || { echo "the game did not start after the tap" >&2; exit 1; }
sleep "$SECS"
PID=$(adb shell pidof dev.tok.totk:game)
mkdir -p "$ROOT/work/android/v143/logs"; LOG="$ROOT/work/android/v143/logs/app-$(date +%H%M%S).log"
adb shell run-as dev.tok.totk cat files/profile/totk.log > "$LOG" 2>/dev/null
echo "game process: ${PID:-not running}   log: $LOG ($(wc -l < "$LOG") lines)"
grep -E "$PAT" "$LOG" | tail -20 | cut -c1-400
