#!/usr/bin/env python3
"""Report for a tools/profile.sh run: frame-time distribution, hitches with what happened in them, CPU per thread,
GPU time, and the simpleperf hotspots by thread and function.
  port/android/tools/profile_report.py work/android/v143/profile/<name> [--no-perf]"""
import collections, os, re, statistics, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
NDK_SP = '/opt/android-sdk/ndk/29.0.14206865/simpleperf'
D = sys.argv[1]
lines = open(os.path.join(D, 'run.log'), errors='replace').read().splitlines()

FP = re.compile(r'\[frameprof\] frame=(\d+) wall=([\d.]+) cpu=([\d.]+) ms draws=(\d+) submits=(\d+) dep=(\d+)/([\d.]+) ms '
                r'snap=([\d.]+) MB pipelines=(\d+) images=(\d+) uploads=(\d+)')
frames, between, pending = [], {}, []
for l in lines:
    m = FP.search(l)
    if m:
        f = dict(zip(['frame', 'wall', 'cpu', 'draws', 'submits', 'depn', 'depms', 'snap', 'pipes', 'images', 'uploads'],
                     [float(x) for x in m.groups()]))
        frames.append(f); between[int(f['frame'])] = pending; pending = []
    elif '[frameprof]' not in l:
        pending.append(l)
# game frames (not loading screens)
frames = [f for f in frames if f['draws'] > 50]
if not frames: sys.exit('no game frames in run.log')

def pct(v, p): v = sorted(v); return v[min(len(v) - 1, int(len(v) * p))]
walls = [f['wall'] for f in frames]
med = statistics.median(walls)
print(f'== {os.path.basename(D)}: {len(frames)} frames, {len(frames) / (sum(walls) / 1000):.1f} fps mean')
print('frame interval ms: p50 %.1f  p90 %.1f  p99 %.1f  max %.1f   over 25 ms: %d  over 33 ms: %d  over 50 ms: %d' % (
    med, pct(walls, .9), pct(walls, .99), max(walls), sum(w > 25 for w in walls), sum(w > 33.4 for w in walls), sum(w > 50 for w in walls)))
for k, name in [('cpu', 'translation-thread CPU ms'), ('draws', 'draws'), ('submits', 'submits'), ('depn', 'dependency waits'),
                ('depms', 'dependency wait ms'), ('snap', 'snapshot MB')]:
    v = [f[k] for f in frames]; print('  %-27s p50 %7.2f  p90 %7.2f  max %7.2f' % (name, statistics.median(v), pct(v, .9), max(v)))
print('  frames creating pipelines: %d (%d pipelines), images: %d, with texture uploads: %d' % (
    sum(f['pipes'] > 0 for f in frames), sum(f['pipes'] for f in frames), sum(f['images'] > 0 for f in frames), sum(f['uploads'] > 0 for f in frames)))

# hitches: what each slow frame spent its time on, against a normal frame
limit = max(25.0, 1.6 * med)
hitch = [f for f in frames if f['wall'] > limit]
cause = collections.Counter()
print(f'\nhitches (> {limit:.1f} ms): {len(hitch)}')
for f in sorted(hitch, key=lambda f: -f['wall'])[:25]:
    why = []
    if f['pipes']: why.append('%d new pipelines' % f['pipes'])
    if f['images']: why.append('%d new images' % f['images'])
    if f['depms'] > 3: why.append('waited %.1f ms on GPU results' % f['depms'])
    if f['snap'] > 8: why.append('%.1f MB snapshots' % f['snap'])
    if f['cpu'] > 1.6 * statistics.median([g['cpu'] for g in frames]): why.append('translation CPU %.1f ms' % f['cpu'])
    if not why: why.append('translation idle: game side or present')
    for w in why: cause[re.sub(r'[\d.]+', 'N', w)] += 1
    notes = [re.sub(r'^\[dm\] ', '', l)[:110] for l in between.get(int(f['frame']), []) if '[vulkan-perf]' not in l and '[vulkan-time]' not in l][:3]
    print('  frame %5d %6.1f ms  cpu %5.1f  draws %4d  %s' % (f['frame'], f['wall'], f['cpu'], f['draws'], '; '.join(why)))
    for n in notes: print('        | ' + n)
print('  causes over all hitches:', dict(cause.most_common()))

# GPU time per frame (the backend's own measurement, 60-frame windows)
gpu = [float(m.group(1)) for l in lines for m in [re.search(r'\[vulkan-time\] frame=\d+ GPU=([\d.]+) ms/frame fence_wait=([\d.]+)', l)] if m]
fw = [float(m.group(2)) for l in lines for m in [re.search(r'\[vulkan-time\] frame=\d+ GPU=([\d.]+) ms/frame fence_wait=([\d.]+)', l)] if m]
if gpu: print('\nGPU ms/frame: median %.2f  max %.2f   fence wait ms/frame: median %.2f' % (statistics.median(gpu), max(gpu), statistics.median(fw)))

# CPU per thread over the run (clock ticks of 10 ms)
def threads(name):
    t = {}
    for l in open(os.path.join(D, name)):
        p = l.split()
        if len(p) == 4: t[p[0]] = (p[1], int(p[2]) + int(p[3]))
    return t
a, b = threads('threads-start.txt'), threads('threads-end.txt')
by = collections.Counter()
for tid, (name, ticks) in b.items():
    by[re.sub(r'[-_:]?\d+$', '', name)] += ticks - a.get(tid, (name, 0))[1]
n = len([l for l in lines if FP.search(l)])
total = sum(by.values())
print(f'\nCPU per thread over the run ({n} frames; ms per frame, % of the process):')
for name, ticks in by.most_common(14):
    if ticks: print('  %-24s %6.2f ms/frame  %5.1f%%' % (name, ticks * 10 / max(n, 1), 100 * ticks / max(total, 1)))

# simpleperf: by thread, then by function inside the busiest threads
# the cache builder runs inside D
perf = os.path.abspath(os.path.join(D, 'perf.data'))
if '--no-perf' not in sys.argv and os.path.exists(perf):
    cache = os.path.join(D, 'binary_cache')
    if not os.path.isdir(cache):
        subprocess.run([sys.executable, os.path.join(NDK_SP, 'binary_cache_builder.py'), '-i', perf, '-lib',
                        os.path.join(ROOT, 'work/android/v143/build-host'), os.path.join(ROOT, 'work/turnip-build/build-release/src/freedreno/vulkan'),
                        '--disable_adb_root'], cwd=D, capture_output=True)
    sp = os.path.join(NDK_SP, 'bin/linux/x86_64/simpleperf')
    def report(*args):
        r = subprocess.run([sp, 'report', '-i', perf, '--symfs', cache, '--percent-limit', '0.8', *args], capture_output=True, text=True)
        return [l for l in r.stdout.splitlines() if re.match(r'\s*[\d.]+%', l)]
    print('\nsimpleperf, self time by thread and library:')
    for l in report('--sort', 'comm,dso')[:20]: print('  ' + l.strip())
    print('\nsimpleperf, self time by function (all threads):')
    for l in report('--sort', 'comm,symbol')[:40]: print('  ' + l.strip()[:150])
