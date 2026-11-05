#!/usr/bin/env python3
"""Input-to-screen latency from a latency_probe.sh timeline: for each stick pulse, input.now set the stick -> the game's
first read of it -> the first presented picture whose probe rows moved (far above the scene's own change) -> on screen.
   port/android/tools/latency_report.py <timeline.tsv> [-v: one line per pulse, with the game presents in between]"""
import bisect, collections, statistics, sys
ev = collections.defaultdict(list)
for l in open(sys.argv[1]):
    if l[0] == '#': continue
    t, k, tid, f, a, b = l.split(); t, a, b = int(t), int(a), int(b)
    if b >= 1 << 63: b -= 1 << 64
    ev[k].append((t, a, b, int(tid)))
for k in ev: ev[k].sort()
inj = [(t, b) for t, a, b, _ in ev['input'] if a >> 40]
reads = [(t, b) for t, a, b, _ in ev['input'] if not a >> 40]
# present id -> (on screen, our present)
shown = {a: (t, b) for t, a, b, _ in ev['shown']}
# by our present time
pix = sorted((shown[a][1], a, b) for t, a, b, _ in ev['pixels'] if a in shown)
# the game's presents
pres = [t for t, *_ in ev['pres_in']]
ms = lambda x: x / 1e6
rows = []
for t0, v in inj:
    if v == 0: continue
    r = next((t for t, x in reads if t > t0 and x == v), None)
    base = sorted(d for p, i, d in pix if t0 - 1e9 < p < t0 - 1e8)
    if r is None or len(base) < 10: continue
    thr = max(base[int(len(base) * .95)] * 2, base[len(base) // 2] * 4)
    eff = next(((p, i) for p, i, d in pix if p > r and d > thr), None)
    if not eff: continue
    p, i = eff; s = shown[i][0]; k, g = bisect.bisect_left(pres, r), bisect.bisect_left(pres, p) - 1
    rows.append({'stick -> game read': ms(r - t0), 'read -> game present': ms(pres[g] - r), 'game -> our present': ms(p - pres[g]),
                 'our present -> screen': ms(s - p), 'stick -> screen': ms(s - t0), 'game presents': g - k + 1})
    if '-v' in sys.argv:
        print(f"stick {v:+6d}: read +{ms(r - t0):4.1f} | game presents " + " ".join(f"{ms(pres[j] - r):5.1f}" for j in range(k, g + 1)) +
              f" | our present {ms(p - r):5.1f} | screen {ms(s - r):5.1f} ms after the read")
if not rows: sys.exit('no pulses found (window, TOTK_PACING=1, TOTK_LATENCY_PROBE=1?)')
for key in rows[0]:
    v = sorted(x[key] for x in rows)
    print(f"{key:22s} p50 {statistics.median(v):6.1f}  min {v[0]:6.1f}  max {v[-1]:6.1f}")
span = (pres[-1] - pres[0]) / 1e9 if len(pres) > 1 else 0
st = sorted(t for t, *_ in ev['shown']); sspan = (st[-1] - st[0]) / 1e9 if len(st) > 1 else 0
print(f"({len(rows)} pulses; game {len(pres) / span if span else 0:.1f} presents/s, screen {len(st) / sspan if sspan else 0:.1f} pictures/s)")
