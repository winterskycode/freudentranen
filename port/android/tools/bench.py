#!/usr/bin/env python3
"""bench.py: a repeatable benchmark on the device: the camera orbits a set of places, every game frame is timed.

    python3 port/android/tools/bench.py [--save <slot folder>] [--scenes Kakariko,Depths] [--label name]
                                        [--env VAR=value ...] [--laps N]
    python3 port/android/tools/bench.py --compare <run folder> <run folder>

Built on MacNativeGames' survey session (runtime/world_survey.c, the benchmark camera the 1.4.3 patches install with
TOTK_SURVEY=1; scripts/bench_run.py flies UltraCam's route the same way). Our route is our own: for each scene the camera
circles a point of the game's own map (LocationMarker actors, read from the player's RomFS by MacNativeGames'
bench_locations.py into work/android/v143/bench/locations.json), one lap in ORBIT_S seconds at a fixed height and
distance, so it never flies into a hill and streams the world in every direction. Link is dropped at the point first and
the route starts where he lands (map markers are not at ground height); the camera waits until he is still and the
world is in, then flies; the frames of the flight are the scene's.

A run: waits until the device is cool, builds a test profile from the save (default: the 100% save; the player's own
profile is only read, for its account and options), starts the game with the survey and the frame log on, presses
through the title, flies every scene, and reports per scene the average FPS, the 1%
and 0.1% lows and the worst frame, with temperatures and the fan and performance modes (frame times only compare in
the same mode: profile.sh). The scenes' hours are not set (--clock), so time of day and weather follow the save and the
run's pace: compare runs made from the same cool start. Results go to work/android/v143/bench/<time>-<label>/ (report.json, frames.txt, totk.log).
"""
import argparse, json, math, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BENCH = ROOT/'work/android/v143/bench'
APP = 'dev.tok.totk'
FILES = f'/data/user/0/{APP}/files'
DEFAULT_SAVE = BENCH/'save-100/slot_00'   # the 100% save, unpacked by --save-zip (switch_files/totk_save_files_*.zip)
ORBIT_S, RADIUS, HEIGHT, FOV = 30.0, 60.0, 35.0, 0.85
# (scene, map point: a LocationMarker name, or "save" for where the save puts Link, hour of the day)
SCENES = [('Great Sky Island', 'save', 12), ('Lookout Landing', 'ShopGeneral_BaseCamp', 11),
          ('Kakariko', 'ShopInn_Kakariko', 12), ('Korok Forest', 'ShopInn_Korok', 10), ('Zora Domain', 'ShopInn_Zora', 15),
          ('Depths', 'MinusField_Rito', 12),
          # --scenes all adds these
          ('Goron City', 'ShopInn_Goron', 20), ('Hateno', 'ShopInn_Hateno', 9), ('Gerudo Town', 'ShopInn_Gerudo', 14)]
DEFAULT_SCENES = 6   # about 6 minutes of flying: the Odin heats quickly (two laps of one scene took the CPU from 59 to 97 C)
COOL_C = 60.0        # a run starts once thermal status is 0 and the hottest CPU sensor is below this (idle: 54-58 C, the
                     # vendor service keeps CPU clock floors up)


def adb(*args, check=True, input=None, timeout=120):
    r = subprocess.run(['adb', *args], capture_output=True, text=input is None or isinstance(input, str), input=input, timeout=timeout)
    if check and r.returncode:
        raise SystemExit(f'adb {" ".join(args)}: {r.stderr or r.stdout}')
    return r.stdout


def shell(cmd, **kw):
    return adb('shell', cmd, **kw)


def as_app(cmd, **kw):
    return adb('exec-out', 'run-as', APP, 'sh', '-c', cmd, **kw)   # exec-out passes each argument as it is


def put(path, text):
    """Write files/<path> of the app: adb push to /data/local/tmp, then a copy as the app (adb's stdin to run-as is not
    reliable: a cat through exec-out never ends, one through exec-in got 0 bytes)."""
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(text)
    adb('push', f.name, '/data/local/tmp/bench-xfer.txt')
    Path(f.name).unlink()
    shell('chmod a+r /data/local/tmp/bench-xfer.txt')
    as_app(f'cp /data/local/tmp/bench-xfer.txt {path}')


def send(line):
    as_app(f"echo '{line}' >> files/input.now")


def log_grep(pattern):
    return as_app(f"grep -a -E '{pattern}' files/profile/totk.log 2>/dev/null", check=False)


def wait(pattern, timeout, poll=1.0):
    end = time.time() + timeout
    while time.time() < end:
        out = log_grep(pattern).strip().splitlines()
        if out:
            return out[-1]
        if log_grep(r'signal=0x|FATAL|host abort').strip():
            raise SystemExit('the game crashed (see totk.log)')
        time.sleep(poll)
    return None


def thermal():
    m = re.search(r'Thermal Status: (\d+)', shell('dumpsys thermalservice'))
    return int(m[1]) if m else -1


def device_state():
    t = shell('dumpsys thermalservice')
    hal = t.split('Current temperatures from HAL')[1].split('Current cooling devices')[0] if 'Current temperatures from HAL' in t else ''
    def top(kind):
        vals = [float(v) for v, k in re.findall(r'mValue=([\d.]+), mType=(\d+)', hal) if int(k) == kind]
        return max(vals) if vals else None
    skin = re.search(r'mValue=([\d.]+), mType=\d+, mName=skin', hal)
    fan = shell('f=/sys/class/gpio5_pwm2; if [ -e $f/duty ]; then echo "fan $(cat $f/state) $(( $(cat $f/duty) * 100 / $(cat $f/period) ))% '
                '$(cat $f/speed)rpm mode $(settings get system fan_mode) perf $(settings get system performance_mode)"; else echo "no fan"; fi').strip()
    return dict(thermal=thermal(), cpu_c=top(0), gpu_c=top(1), skin_c=float(skin[1]) if skin else None, fan=fan)


def orbit(center, t0=0.0):
    """Keyframes of one lap around center: 12 points, the camera HEIGHT m up and RADIUS m out, looking at center + 5 m."""
    cx, cy, cz = center
    keys = []
    for i in range(13):
        a = 2 * math.pi * i / 12
        pos = (cx + RADIUS * math.cos(a), cy + HEIGHT, cz + RADIUS * math.sin(a))
        f = (cx - pos[0], cy + 5 - pos[1], cz - pos[2]); n = math.sqrt(sum(c * c for c in f)); f = tuple(c / n for c in f)
        r = (f[2], 0.0, -f[0]); n = math.sqrt(r[0] ** 2 + r[2] ** 2); r = (r[0] / n, 0.0, r[2] / n)   # right = fwd x up(0,1,0)
        up = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
        if up[1] < 0: up = tuple(-c for c in up)
        keys.append((t0 + ORBIT_S * i / 12, pos, f, up))
    return keys


def route_text(start, keys, hour):
    rows = [f'start {start[0]:.3f} {start[1]:.3f} {start[2]:.3f}']
    if hour is not None:
        rows.append(f'hour {hour}')
    for t, p, f, u in keys:
        rows.append('k {:.3f} {:.3f} {:.3f} {:.3f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.3f}'.format(t, *p, *f, *u, FOV))
    return '\n'.join(rows) + '\n'


def locations():
    path = BENCH/'locations.json'
    if not path.exists():
        BENCH.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(ROOT/'work/venv/bin/python'), str(ROOT/'MacNativeGames/games/totk/scripts/bench_locations.py'),
                        str(ROOT/'work/totk/extracted/v143/romfs'), '--out', str(path)], check=True)
    return {x['name']: (x['x'], x['y'], x['z']) for x in json.loads(path.read_text())}


def xyz_of(line):
    return tuple(float(v) for v in re.search(r'xyz=([-\d.]+),([-\d.]+),([-\d.]+)', line).groups())


def land(tag, point):
    """Where Link stands at a map point: markers are not at ground height (Lookout Landing's is 5 m below it), and the
    route waits until Link is within 2 m above its start, so he is dropped 15 m above the marker first, lands, and the
    route starts where he stands."""
    x, y, z = point
    send(f'survey move {tag}_drop {x:.3f} {y + 15:.3f} {z:.3f} 30')
    done = wait(rf'\[survey\] tag={tag}_drop status=(move_done|busy|disabled|no_player)', 120)
    if not done or 'move_done' not in done:
        raise SystemExit(f'{tag}: could not move Link to the point: {done}')
    last = None
    for i in range(30):   # until he stops falling (the world streams in around him meanwhile)
        time.sleep(1)
        send(f'survey pos {tag}_g{i}')
        line = wait(rf'\[survey\] tag={tag}_g{i} status=(position|no_player)', 10)
        if line and 'status=position' in line:
            at = xyz_of(line)
            if last and abs(at[1] - last[1]) < 0.05 and i >= 4:
                return at
            last = at
    raise SystemExit(f'{tag}: Link did not come to rest at the point (last {last})')


def stats(frames, a, b):
    """FPS figures of the game frames a..b (frame numbers, the survey's), from the "<frame> <ns>" log."""
    ts = [frames[f] for f in range(a, b + 1) if f in frames]
    dt = sorted((y - x) / 1e6 for x, y in zip(ts, ts[1:]))
    if len(dt) < 10:
        return None
    total = sum(dt)
    def low(p):   # the FPS of the slowest p of the frames, as NX Optimizer reports its lows
        k = max(1, int(len(dt) * p)); return 1000 * k / sum(dt[-k:])
    return dict(frames=len(dt), seconds=round(total / 1000, 2), avg_fps=round(1000 * len(dt) / total, 1),
                low1_fps=round(low(0.01), 1), low01_fps=round(low(0.001), 1), worst_ms=round(dt[-1], 1),
                over33=sum(d > 33.4 for d in dt), p50_ms=round(dt[len(dt) // 2], 2))


ACCOUNT = BENCH/'account'   # account.uid + save/<account>/option.sav, for a device without a player profile


def profile(save):
    """files/bench-profile: the player's account and options (read) and the save's slot as slot_00. A device where
    nobody has played (no files/profile/account.uid: the game would start a new game) gets the account kept in
    work/android/v143/bench/account instead (a copy of another device's account.uid and option.sav)."""
    shell('rm -rf /data/local/tmp/bench-save /data/local/tmp/bench-account')
    adb('push', str(save), '/data/local/tmp/bench-save/slot_00', timeout=300)
    src = 'files/profile'
    if as_app('test -f files/profile/account.uid && echo yes', check=False).strip() != 'yes':
        if not (ACCOUNT/'account.uid').is_file():
            raise SystemExit(f'no player profile on the device and no {ACCOUNT}/account.uid to use instead')
        adb('push', str(ACCOUNT), '/data/local/tmp/bench-account')
        src = '/data/local/tmp/bench-account'
        print(f'no player profile on the device: using the account in {ACCOUNT}', flush=True)
    shell('chmod -R a+rX /data/local/tmp/bench-save /data/local/tmp/bench-account 2>/dev/null', check=False)
    as_app('rm -rf files/bench-profile && mkdir -p files/bench-profile/cache files/bench-profile/temp && '
           f'cp {src}/account.uid files/bench-profile/ && for d in {src}/save/*; do h=${{d##*/}}; '
           'mkdir -p files/bench-profile/save/$h && cp $d/option.sav files/bench-profile/save/$h/ && '
           'cp -r /data/local/tmp/bench-save/slot_00 files/bench-profile/save/$h/; done')
    shell('rm -rf /data/local/tmp/bench-account', check=False)


def run(args):
    places = locations()
    scenes = SCENES[:DEFAULT_SCENES]
    if args.scenes == 'all':
        scenes = SCENES
    elif args.scenes:
        want = [w.strip().lower() for w in args.scenes.split(',')]
        scenes = [s for s in SCENES if any(w in s[0].lower() for w in want)]
    save = Path(args.save) if args.save else DEFAULT_SAVE
    if not (save/'progress.sav').is_file():
        raise SystemExit(f'no save at {save} (a slot folder with progress.sav; --save-zip unpacks the 100% save)')
    out = BENCH/(time.strftime('%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else ''))
    out.mkdir(parents=True)
    shell('input keyevent KEYCODE_WAKEUP'); shell('wm dismiss-keyguard', check=False)
    shell(f'am force-stop {APP}')
    for _ in range(120):   # the device cannot cool while the previous run plays; runs compare only from the same start
        st = device_state()
        if st['thermal'] == 0 and (st['cpu_c'] or 0) < COOL_C: break
        time.sleep(5)
    else:
        print(f'WARNING: still warm after 10 minutes: {st}', flush=True)
    before = device_state()
    print('before:', before, flush=True)
    if before['fan'] != 'no fan' and (not before['fan'].startswith('fan 1') or ' mode 4 ' not in before['fan'] + ' '):
        print('WARNING: fan off or not in mode 4: frame times will not compare with other runs (profile.sh)', flush=True)
    profile(save)
    # what the device renders: no frame rate cap and no frame generation (a cap hides any gain in a scene that reaches
    # it: at 45 + LSFG x2 the Depths read 45.0 in every run), unless --as-played keeps the player's settings
    uncapped = [] if args.as_played else ['TOTK_UNCAPPED_HZ=240', 'TOTK_FRAMEGEN=0']
    env = [f'TOTK_PROFILE_DIR={FILES}/bench-profile', 'TOTK_SURVEY=1', 'TOTK_SLOT_OWNER=claude',
           f'TOTK_BENCH_FRAMES={FILES}/bench-frames.txt', 'TOTK_VK_FRAMEPROF=1', *uncapped, *args.env]
    if args.timeline:   # one TOTK_TIMELINE window in each scene's flight, started by files/timeline.tsv.go (totk_timeline.c)
        env.append(f'TOTK_TIMELINE={FILES}/timeline.tsv:go:{args.timeline}')
    if args.gputrace:   # Turnip's GPU trace (render passes, compute, copies, command buffers on the GPU clock), kept per scene
        env += ['MESA_GPU_TRACES=print_csv', f'MESA_GPU_TRACEFILE={FILES}/gputrace.csv']
    trace_size = lambda: int((as_app('stat -c %s files/gputrace.csv', check=False).strip() or '0'))
    as_app('rm -f files/profile/totk.log files/input.now files/bench-frames.txt files/bench-route-* files/timeline.tsv* files/gputrace.csv')
    put('files/debug.env', '\n'.join(env) + '\n')
    shell(f'am start -W --display 0 -n {APP}/.LauncherActivity')
    time.sleep(3)
    for _ in range(6):   # the launcher misses a press while it is still getting ready (a first start on the Xperia)
        shell('input -d 0 keyevent KEYCODE_DPAD_CENTER'); time.sleep(4)
        if shell(f'pidof {APP}:game', check=False).strip(): break
    try:
        if not wait(r'benchmark camera: look-at', 120):
            raise SystemExit('no benchmark camera (TOTK_SURVEY=1 session not set up: see totk.log)')
        # through the title: A every 2 s until the world draws (the frame profile's draw count)
        start = time.time()
        while time.time() - start < 240:
            line = (log_grep(r'\[frameprof\] frame=').strip().splitlines() or [''])[-1]
            m = re.search(r'draws=(\d+)', line)
            if m and int(m[1]) > 1500: break
            if line: send('a')
            time.sleep(2)
        else:
            raise SystemExit('the save never loaded')
        here = None
        for i in range(60):   # the player appears once the world is in (at 0, -0.15, 0 while the save still loads)
            send(f'survey pos p{i}')
            line = wait(rf'\[survey\] tag=p{i} status=(position|no_player)', 10)
            if line and 'status=position' in line and 'xyz=0.000000,-0.150390,0.000000' not in line:
                here = xyz_of(line); break
            time.sleep(1)
        if not here:
            raise SystemExit('the player never appeared in the world')
        time.sleep(3)
        clock_ok = False
        if args.clock:   # scene hours: world_survey.c's clock search; on Android it picks other timers of the same rate
            send('survey clockfind')
            clock = wait(r'\[survey\] time of day: ', 150)
            clock_ok = bool(clock) and bool(log_grep(r'\[survey\] clock value at').strip())
            print('clock:', clock.split('] ', 1)[1] if clock else 'not found', flush=True)
        segments = []
        for name, point, hour in scenes:
            for lap in range(args.laps):
                tag = re.sub(r'[^A-Za-z0-9]', '_', name) + (f'_{lap}' if args.laps > 1 else '')
                center = here if point == 'save' else land(tag, places[point])
                path = f'files/bench-route-{tag}.txt'
                put(path, route_text(center, orbit(center), hour if clock_ok else None))
                send(f'survey route {tag} {FILES}/bench-route-{tag}.txt')
                begin = wait(rf'\[survey\] tag={tag} status=(route_begin|busy|disabled|no_camera|bad_route|no_player|invalid_rotation)', 30)
                if not begin or 'route_begin' not in begin:
                    raise SystemExit(f'{name}: route not accepted: {begin}')
                measure = wait(rf'\[survey\] tag={tag} status=(route_measure|route_done|no_player|invalid_rotation)', 180)
                if not measure or 'route_measure' not in measure:
                    shell(f'screencap -p /data/local/tmp/bench-{tag}.png', check=False)
                    adb('pull', f'/data/local/tmp/bench-{tag}.png', str(out), check=False)
                    raise SystemExit(f'{name}: the camera never started (did Link land at the point? {out}/bench-{tag}.png): {measure}')
                if args.timeline:
                    time.sleep(3)
                    put('files/timeline.tsv.go', tag)
                if args.gputrace:   # 12 s of the flight, from 3 s in; only the trace's pieces that are timed on the GPU
                    if not args.timeline: time.sleep(3)
                    b0 = trace_size(); time.sleep(12); b1 = trace_size()
                    (out/f'gputrace-{tag}.csv').write_bytes(subprocess.run(['adb', 'exec-out', 'run-as', APP, 'sh', '-c',
                        f'tail -c +{b0 + 1} files/gputrace.csv | head -c {b1 - b0} | grep -a -E ",(start|end)_[a-z_]+,"'], capture_output=True).stdout)
                length = ORBIT_S * 11 / 12 + 1.5 * ORBIT_S / 12   # T[n-2] + 1.5 (T[n-1] - T[n-2]), world_survey.c route_length
                end = wait(rf'\[survey\] tag={tag} status=(route_done|no_player|invalid_rotation)', length + 60)
                if not end or 'route_done' not in end:
                    raise SystemExit(f'{name}: the flight did not finish: {end}')
                f = lambda l: int(re.search(r'frame=(\d+)', l)[1])
                segments.append(dict(scene=name, tag=tag, begin=f(measure), end=f(end)))
                print(f'  {name}: frames {f(measure)}..{f(end)}', flush=True)
                shell(f'screencap -p /data/local/tmp/bench-{tag}.png', check=False)
    finally:
        after = device_state()
        (out/'totk.log').write_bytes(subprocess.run(['adb', 'exec-out', 'run-as', APP, 'cat', 'files/profile/totk.log'], capture_output=True).stdout)
        (out/'frames.txt').write_bytes(subprocess.run(['adb', 'exec-out', 'run-as', APP, 'cat', 'files/bench-frames.txt'], capture_output=True).stdout)
        if args.timeline:
            time.sleep(5)   # the last window is written ~(frames + 30) frames after its start
            for tl in as_app('ls files', check=False).split():
                if tl.startswith('timeline.tsv-'):
                    (out/tl).write_bytes(subprocess.run(['adb', 'exec-out', 'run-as', APP, 'cat', f'files/{tl}'], capture_output=True).stdout)
        shell(f'am force-stop {APP}')
        as_app(': > files/debug.env; rm -rf files/bench-profile files/bench-route-* files/bench-frames.txt files/input.now files/timeline.tsv* files/gputrace.csv')
    frames = {}
    for line in (out/'frames.txt').read_text().splitlines():
        p = line.split()
        if len(p) == 2: frames[int(p[0])] = int(p[1])
    report = dict(label=args.label, env=args.env, uncapped=not args.as_played, save=str(save), before=before, after=after, clock=clock_ok,
                  scenes=[dict(s, **(stats(frames, s['begin'], s['end']) or {})) for s in segments])
    allf = [s for s in report['scenes'] if 'frames' in s]
    report['total'] = dict(frames=sum(s['frames'] for s in allf), avg_fps=round(sum(s['frames'] for s in allf) / max(sum(s['seconds'] for s in allf), 1e-9), 1))
    for s in segments:
        subprocess.run(['adb', 'pull', f'/data/local/tmp/bench-{s["tag"]}.png', str(out)], capture_output=True)
    (out/'report.json').write_text(json.dumps(report, indent=1) + '\n')
    print_report(report, out)


def print_report(r, out=None):
    print(f"\n{r.get('label') or ''} {' '.join(r.get('env') or [])}".strip())
    print(f"{'scene':18} {'avg':>7} {'1% low':>7} {'0.1%':>7} {'worst':>8} {'>33ms':>6} {'frames':>7}")
    for s in r['scenes']:
        if 'frames' in s:
            print(f"{s['scene']:18} {s['avg_fps']:7.1f} {s['low1_fps']:7.1f} {s['low01_fps']:7.1f} {s['worst_ms']:6.1f}ms {s['over33']:6d} {s['frames']:7d}")
    print(f"{'all':18} {r['total']['avg_fps']:7.1f}{'':>40}{r['total']['frames']:7d}")
    print(f"before: {r['before']}\nafter:  {r['after']}" + (f'\n{out}' if out else ''))


def compare(a, b):
    ra, rb = (json.loads((Path(p)/'report.json').read_text()) for p in (a, b))
    print(f"A: {ra.get('label')} {' '.join(ra.get('env') or [])}\nB: {rb.get('label')} {' '.join(rb.get('env') or [])}")
    print(f"{'scene':18} {'avg A':>7} {'avg B':>7} {'diff':>7} {'1%A':>7} {'1%B':>7} {'0.1%A':>7} {'0.1%B':>7}")
    sb = {s['tag']: s for s in rb['scenes'] if 'frames' in s}
    for s in ra['scenes']:
        t = sb.get(s['tag'])
        if 'frames' in s and t:
            print(f"{s['scene']:18} {s['avg_fps']:7.1f} {t['avg_fps']:7.1f} {100 * (t['avg_fps'] / s['avg_fps'] - 1):+6.1f}% "
                  f"{s['low1_fps']:7.1f} {t['low1_fps']:7.1f} {s['low01_fps']:7.1f} {t['low01_fps']:7.1f}")
    print(f"{'all':18} {ra['total']['avg_fps']:7.1f} {rb['total']['avg_fps']:7.1f} {100 * (rb['total']['avg_fps'] / ra['total']['avg_fps'] - 1):+6.1f}%")
    print(f"fan A: {ra['before']['fan']}\nfan B: {rb['before']['fan']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--save', help=f'slot folder with progress.sav (default {DEFAULT_SAVE})')
    ap.add_argument('--save-zip', help='only unpack the 100%% save from this zip (TOTK Save Files/100%% Save/slot_00) as the default save')
    ap.add_argument('--scenes', help=f'comma-separated parts of scene names, or "all" (default: the first {DEFAULT_SCENES})')
    ap.add_argument('--label', default='')
    ap.add_argument('--env', action='append', default=[], help='an extra debug.env line, e.g. TOTK_ASYNC_PREEMPT=1 (repeatable)')
    ap.add_argument('--as-played', action='store_true', help="keep the player's frame rate cap and frame generation "
                    '(default: uncapped, no frame generation)')
    ap.add_argument('--laps', type=int, default=1, help='laps per scene (a second lap shows the place without first-visit loading)')
    ap.add_argument('--clock', action='store_true', help='set each scene\'s hour (experimental: on Android the clock search '
                    'finds another timer of the same rate, 2026-10-28, and would write the hour into it)')
    ap.add_argument('--timeline', type=int, default=0, metavar='FRAMES', help='a TOTK_TIMELINE window of FRAMES frames 3 s into '
                    'each scene\'s flight, saved as timeline.tsv-<scene> (scratch analysis: GPU busy / idle by cause)')
    ap.add_argument('--gputrace', action='store_true', help='Turnip\'s GPU trace for 12 s of each flight, saved as gputrace-<scene>.csv')
    ap.add_argument('--compare', nargs=2, metavar='RUN')
    args = ap.parse_args()
    if args.compare:
        return compare(*args.compare)
    if args.save_zip:   # unpack only
        import zipfile
        DEFAULT_SAVE.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.save_zip) as z:
            for n in z.namelist():
                if '/100% Save/slot_00/' in n and not n.endswith('/'):
                    (DEFAULT_SAVE/Path(n).name).write_bytes(z.read(n))
        return print('100% save unpacked to', DEFAULT_SAVE)
    run(args)


if __name__ == '__main__':
    sys.exit(main())
