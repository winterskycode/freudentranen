#!/bin/bash
# Input-to-screen latency by cause and effect: the still scene (profile.sh), right-stick pulses that turn the camera every
# 1.5 s, and a timeline with the picture probe (TOTK_LATENCY_PROBE, display timing; every picture shown, generated ones too).
# latency_report.py finds, for each pulse, the game's first read of the stick and the first shown picture that moved.
#   port/android/tools/latency_probe.sh <name> <game frame rate> [debug.env lines...]
#   The rate sizes the window (35 s of game frames, opened by a trigger once the save has loaded): the run must reach at
#   least 60% of it for the window to close. Settings not given come from launch.env: pin what the comparison needs.
#   The game is stopped and debug.env cleared at the end. -> work/android/v143/profile/lp-<name>/timeline.tsv
cd "$(dirname "$0")/../../.." || exit 1
name=$1 rate=$2; shift 2
F=/data/data/dev.tok.totk/files; P=work/android/v143/profile; export SWEEP_PHASE=$PWD/$P/.latency-phase; rm -f "$SWEEP_PHASE"
adb shell run-as dev.tok.totk sh -c "'rm -f files/timeline.tsv*'"
NOPERF=1 ROUTE=still port/android/tools/profile.sh "lp-$name" 60 TOTK_LATENCY_PROBE=1 TOTK_TIMELINE_LEAN=1 \
    "TOTK_TIMELINE=$F/timeline.tsv:go:$((rate * 35))" "$@" > /dev/null 2>&1 &
run=$!
until grep -q recording "$SWEEP_PHASE" 2>/dev/null; do sleep 0.5; kill -0 $run 2>/dev/null || break; done
sleep 2
adb shell run-as dev.tok.totk sh -c "'echo lp > files/timeline.tsv.go'"; sleep 1
for i in $(seq 1 15); do
    adb shell run-as dev.tok.totk sh -c "'echo \"rx 32000 300\" > files/input.now'"; sleep 1.5
    adb shell run-as dev.tok.totk sh -c "'echo \"rx -32000 300\" > files/input.now'"; sleep 1.5
done
wait $run
adb exec-out run-as dev.tok.totk cat files/timeline.tsv-lp > "$P/lp-$name/timeline.tsv"
adb shell am force-stop dev.tok.totk
adb shell run-as dev.tok.totk sh -c "'rm -f files/timeline.tsv* files/input.now; : > files/debug.env'"; rm -f "$SWEEP_PHASE"
python3 port/android/tools/latency_report.py "$P/lp-$name/timeline.tsv"
