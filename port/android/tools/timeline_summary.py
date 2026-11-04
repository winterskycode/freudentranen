#!/usr/bin/env python3
"""Medians of the frame chain's pieces from a TOTK_TIMELINE file: the submit call, main-thread wake -> next submit, and
the translation replay (ms).   port/android/tools/timeline_summary.py <timeline.tsv>"""
import statistics, sys
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1]) if l[:1].isdigit()]
if not rows: print('no timeline'); sys.exit()
ev = sorted((int(r[0]), r[1]) for r in rows)
def spans(a, b):
    out, start = [], None
    for t, k in ev:
        if k == a: start = t
        elif k == b and start is not None: out.append(t - start); start = None
    return out
med = lambda v: statistics.median(v) / 1e6 if v else float('nan')
print('sub %.2f  wake->sub %.2f  replay %.2f' % (med(spans('sub_in', 'sub_out')), med(spans('wait_out', 'sub_in')), med(spans('rep_begin', 'rep_end'))))
