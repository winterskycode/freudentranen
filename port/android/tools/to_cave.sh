#!/bin/sh
# Start the game and load the most recent save (the cave autosave) with scripted presses, no hands needed:
# title "Continue" (A), then "Load" (A). Usage: port/android/tools/to_cave.sh [seconds to wait in game after loading]
DIR=$(dirname "$0"); SECS=${1:-20}
press() { adb shell run-as dev.tok.totk sh -c "'echo $1 > files/input.now'"; }
"$DIR/run_app.sh" 25 'FATAL|signal=' || exit 1
press a; sleep 1; press a; sleep 2; press a   # title (2x A: wake + Continue) -> "Load save data?" -> Load
sleep 25                       # loading
[ "$SECS" -gt 0 ] && sleep "$SECS"
adb shell pidof dev.tok.totk:game > /dev/null || { echo "game died" >&2; exit 1; }
echo "in game (loaded the latest save)"
