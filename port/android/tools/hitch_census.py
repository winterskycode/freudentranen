#!/usr/bin/env python3
"""Hitch census of a bench route run (tools/bench.py --env TOTK_VK_FRAMEPROF=1): for each scene, the frames whose present interval exceeds 33 ms, classified by
the frame profile line: the game's submit gap, our queue wait and translation, the GPU time completed in the window,
pipeline creations, uploads.  hitch_census.py <bench run dir>"""
import re, sys, json, collections
D = sys.argv[1]
r = json.load(open(D + '/report.json')); scenes = [(s['scene'], s['begin'], s['end']) for s in r['scenes']]
fp = re.compile(r'\[frameprof\] frame=(\d+) wall=([\d.]+) cpu=([\d.]+) ms draws=(\d+) submits=(\d+) dep=(\d+)/([\d.]+) ms snap=([\d.]+) MB pipelines=(\d+) images=(\d+) uploads=(\d+)(?: gap=([\d.]+) qwait=([\d.]+) gpu=([\d.]+))?(?: upload_mb=([\d.]+) upload_ms=([\d.]+))?')
rows = {}
for l in open(D + '/totk.log', errors='replace'):
    m = fp.search(l)
    if m:
        g = lambda i, f=float: f(m.group(i)) if m.group(i) is not None else 0
        rows[int(m.group(1))] = dict(wall=g(2), cpu=g(3), draws=g(4, int), dep=g(7), pipes=g(9, int), images=g(10, int), uploads=g(11, int), gap=g(12), qwait=g(13), gpu=g(14), upmb=g(15), upms=g(16))
pres = {}
for l in open(D + '/frames.txt'):
    p = l.split()
    if len(p) == 2 and p[0].isdigit(): pres[int(p[0])] = int(p[1])
def classify(w, dt):
    if w['pipes']: return 'pipeline'
    if w['upmb'] > 2 or w['upms'] > 3: return 'texture uploads'
    if w['uploads'] > 20 or w['images'] > 5: return 'uploads/images'
    if w['cpu'] > 20: return 'translation cpu'
    if w['dep'] > 3: return 'dependency wait'
    if w['gap'] and w['gap'] > 0.6 * dt: return 'game gap'
    if w['gpu'] and w['gpu'] > 0.6 * dt: return 'gpu'
    if w['qwait'] > 5: return 'queue wait'
    return 'other'
total = collections.Counter()
for name, a, b in scenes:
    long = [(f, (pres[f] - pres[f - 1]) / 1e6) for f in range(a + 1, b + 1) if f in pres and f - 1 in pres and (pres[f] - pres[f - 1]) / 1e6 > 33]
    cats = collections.Counter(); worst = []
    for f, dt in long:
        w = rows.get(f) or rows.get(f - 1)
        if not w: cats['no profile'] += 1; continue
        c = classify(w, dt); cats[c] += 1; total[c] += 1
        worst.append((dt, f, c, w))
    worst.sort(reverse=True)
    print(f"{name:18} frames {b - a + 1:5} >33ms {len(long):3} first3s {sum(1 for f, _ in long if f < a + 180):3} : {dict(cats)}")
    for dt, f, c, w in worst[:3]:
        print(f"    f{f} {dt:.0f} ms [{c}] gap {w['gap']:.1f} qwait {w['qwait']:.1f} xlate wall {w['wall']:.1f} cpu {w['cpu']:.1f} gpu {w['gpu']:.1f} pipes {w['pipes']} uploads {w['uploads']} ({w['upmb']:.1f} MB, {w['upms']:.1f} ms) images {w['images']} dep {w['dep']:.1f}")
    ups = [w for f, w in rows.items() if a <= f <= b]
    if ups: print(f"    uploads per frame: mean {sum(w['upmb'] for w in ups)/len(ups):.2f} MB / {sum(w['upms'] for w in ups)/len(ups):.2f} ms, frames with > 2 MB: {sum(1 for w in ups if w['upmb'] > 2)}")
print("all scenes:", dict(total))
