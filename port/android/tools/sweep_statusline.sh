#!/bin/bash
# Claude Code status line: the progress of a running ab_sweep.sh, e.g.
#   A/B opidx ■■■□□□ 3/6 on2 · recording 9/15 s · ETA ~12 min
# Shown while a sweep runs and for 15 minutes after it ends; empty otherwise. stdin (the session JSON) is ignored.
cat > /dev/null
P=$(cd "$(dirname "$0")/../../.." && pwd)/work/android/v143/profile
[ -f "$P/.sweep-status" ] || exit 0
declare -A s; while IFS='=' read -r k v; do s[$k]=$v; done < "$P/.sweep-status"
now=$(date +%s); age=$(( now - $(stat -c %Y "$P/.sweep-status") ))
i=${s[i]}; n=${s[n]}; t0=${s[t0]}
if [ "${s[state]}" != running ]; then
    [ "$age" -gt 900 ] && exit 0
    printf 'A/B %s %s (%d/%d runs, %d min)\n' "${s[tag]}" "${s[state]}" "$((i))" "$n" "$(( (now - t0) / 60 ))"; exit 0
fi
done_runs=$((i - 1)); bar=""
for k in $(seq 1 "$n"); do if [ "$k" -le "$done_runs" ]; then bar+="■"; elif [ "$k" -eq "$i" ]; then bar+="▣"; else bar+="□"; fi; done
phase=""
if [ -f "$P/.sweep-phase" ]; then
    IFS=$'\t' read -r what since len < "$P/.sweep-phase"; el=$(( now - since ))
    if [ "${len:-0}" -gt 0 ]; then phase=" · $what $el/${len} s"; else phase=" · $what $(( el / 60 )):$(printf %02d $(( el % 60 )))"; fi
fi
eta=""
if [ "$done_runs" -gt 0 ]; then per=$(( (now - t0) / done_runs )); left=$(( per * (n - done_runs) - (now - t0 - per * done_runs) )); [ "$left" -lt 60 ] && left=60
    eta=" · ETA ~$(( (left + 59) / 60 )) min"; fi
printf 'A/B %s %s %d/%d %s%s%s\n' "${s[tag]}" "$bar" "$i" "$n" "${s[name]}" "$phase" "$eta"
