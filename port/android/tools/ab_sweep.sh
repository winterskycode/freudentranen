#!/bin/bash
# A/B sweep in the still scene: one 15 s profile.sh run (NOPERF, ROUTE=still) per variant, with the frame timeline, and one
# summary line each. Interleave the variants (base, a, b, base2, a2, ...): the device drifts with heat.
#   port/android/tools/ab_sweep.sh <tag> <name>=<"ENV=v ENV2=v"> ...     (empty env = the build's defaults)
#   APK=<path> in a variant's list installs that APK before its run (build-vs-build A/B)
#   -> work/android/v143/profile/<tag>-<name>/
# Progress for the status line (tools/sweep_statusline.sh): work/android/v143/profile/.sweep-status (which run) and
# .sweep-phase (what profile.sh is doing in it).
cd "$(dirname "$0")/../../.." || exit 1
tag=$1; shift
P=work/android/v143/profile; export SWEEP_PHASE=$PWD/$P/.sweep-phase
n=$#; i=0; t0=$(date +%s); names="$(for s in "$@"; do printf '%s ' "${s%%=*}"; done)"
status() { printf 'tag=%s\ni=%s\nn=%s\nname=%s\nt0=%s\nstate=%s\nnames=%s\n' "$tag" "$i" "$n" "$1" "$t0" "$2" "$names" > "$P/.sweep-status"; }
trap 'status "" stopped' INT TERM
for spec in "$@"; do
    name=${spec%%=*}; env=${spec#*=}; [ "$env" = "$spec" ] && env=""
    i=$((i + 1)); status "$name" running
    # APK=<path> in a variant's env: install that build first (A/B between builds; the token is not a debug.env line)
    apk=""; rest=""; gfx=""; for e in $env; do case "$e" in APK=*) apk=${e#APK=};; GFX=*) gfx="$gfx${gfx:+;}$(echo "${e#GFX=}" | tr '_' ' ')";; *) rest="$rest $e";; esac; done
    # GFX=gfxpoke_<addr>_u_3: a gfx* command (underscores for spaces) sent after the save loads (profile.sh GFX_CMD)
    env=${rest# }
    if [ -n "$apk" ]; then adb install -r "$apk" >/dev/null 2>&1 || echo "install of $apk failed" >&2; fi
    adb shell run-as dev.tok.totk rm -f files/timeline.tsv
    # $env is split on purpose: each ENV=value becomes one debug.env line
    GFX_CMD="$gfx" NOPERF=1 ROUTE=still port/android/tools/profile.sh "$tag-$name" 15 $env TOTK_TIMELINE=/data/data/dev.tok.totk/files/timeline.tsv:5600:300 >/dev/null 2>&1
    out=work/android/v143/profile/$tag-$name
    adb exec-out run-as dev.tok.totk cat files/timeline.tsv > "$out/timeline.tsv" 2>/dev/null
    r=$(python3 port/android/tools/profile_report.py "$out" --no-perf 2>&1)
    draws=$(echo "$r" | grep 'draws  ' | awk '{printf "%d", $3}')
    # the still scene draws ~3,770 a frame; far off means the save loaded into another view: not comparable
    flag=""; [ -n "$draws" ] && { [ "$draws" -lt 3500 ] || [ "$draws" -gt 4050 ]; } && flag=" OTHER-VIEW"
    printf '%-10s %-44s p50 %5s p90 %5s p99 %5s >25ms %4s  draws %4s%s  GPU %6s  xlate %6s  %s\n' "$name" "$env" \
        "$(echo "$r" | grep -o 'p50 [0-9.]*' | head -1 | cut -d' ' -f2)" "$(echo "$r" | grep -o 'p90 [0-9.]*' | head -1 | cut -d' ' -f2)" \
        "$(echo "$r" | grep -o 'p99 [0-9.]*' | head -1 | cut -d' ' -f2)" "$(echo "$r" | grep -o 'over 25 ms: [0-9]*' | head -1 | awk '{print $4}')" "$draws" "$flag" \
        "$(echo "$r" | grep -o 'median [0-9.]*' | head -1 | cut -d' ' -f2)" "$(echo "$r" | grep 'translation-thread' | awk '{print $5}')" \
        "$(python3 port/android/tools/timeline_summary.py "$out/timeline.tsv" 2>&1 | tail -1)  $(tail -1 "$out/device.txt" 2>/dev/null)"
done
status "" done; rm -f "$SWEEP_PHASE"
