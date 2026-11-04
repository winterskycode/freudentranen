#!/usr/bin/env python3.14
"""Linux build driver: game data, verified rewrites and Android rewrites, with independent work run side by side.

    python3.14 port/build_all.py <base.nsp|xci> -k <prod.keys> [--update <update.nsp|nsz>] [--firmware <dir>] [--jobs N]

Uses MacNativeGames' build_app.py for extraction and checks (no macOS steps). Concurrency:
  data lane:  base RomFS + fonts (step_data) -> update RomFS merge (hactool) -> all RomFS overlays at once
  code lane:  Apple rewrites (pinned-hash anchor) and Android rewrites, all modules at once
              -> structck for all modules at once -> equiv per module across all cores
The Apple rewrite is kept because its hashes are pinned upstream: a match proves the ExeFS extraction is exact, and
build_app.step_code then finds the images in place and only writes its report.
"""
import argparse, concurrent.futures as cf, json, os, shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'MacNativeGames'))
import build_app as B

REWRITER = ROOT / 'port/rewriter/d2r'
NDK_LLVM = Path('/opt/android-sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin')
MODULES = ('main', 'sdk', 'subsdk0')   # main first: it is the longest job


def timed(name, fn, *a, **k):
    t = time.time()
    r = fn(*a, **k)
    B.say(f'   {name}: {time.time() - t:.0f} s')
    return r


def run_all(jobs, workers):
    """jobs: [(label, cmd, log, cwd, env)]; runs up to `workers` at once, raises the first failure after all finish."""
    errors = []
    with cf.ThreadPoolExecutor(workers) as pool:
        futures = {pool.submit(B.run, cmd, log, cwd, env, False): label for label, cmd, log, cwd, env in jobs}
        for f in cf.as_completed(futures):
            try:
                f.result()
            except B.BuildError as e:
                errors.append(f'{futures[f]}: {e}')
    if errors:
        raise B.BuildError('\n'.join(errors))


def build_hactool_linux(tree, logs, workers):
    """build_app.build_hactool patches hactool with a macOS funopen() view of the base NCA range; on glibc the same
    view is fopencookie(). Let build_app apply its patches (its make fails on Linux), convert the hook, build again."""
    try:
        return B.build_hactool(tree, logs)
    except B.BuildError:
        pass
    source = tree.parent / 'src/hactool'
    main = source / 'main.c'
    text = main.read_text()
    if 'fopencookie' not in text:
        swaps = [
            ('#define _DARWIN_C_SOURCE 1\n', '#define _DARWIN_C_SOURCE 1\n#define _GNU_SOURCE 1\n'),
            ('static int mng_base_read(void *cookie, char *buffer, int count) {\n',
             'static ssize_t mng_base_read(void *cookie, char *buffer, size_t wanted) {\n'
             '    int count = wanted > 0x7fffffff ? 0x7fffffff : (int)wanted;\n'),
            ('static fpos_t mng_base_seek(void *cookie, fpos_t offset, int whence) {\n'
             '    mng_base_range *range = cookie;\n'
             '    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;\n'
             '    if (base < 0 || offset < -base || offset > range->size - base) return (fpos_t)-1;\n'
             '    return (fpos_t)(range->position = base + offset);\n',
             'static int mng_base_seek(void *cookie, off64_t *where, int whence) {\n'
             '    mng_base_range *range = cookie; off_t offset = *where;\n'
             '    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;\n'
             '    if (base < 0 || offset < -base || offset > range->size - base) return -1;\n'
             '    *where = range->position = base + offset; return 0;\n'),
            ('FILE *view = funopen(range, mng_base_read, NULL, mng_base_seek, mng_base_close);',
             'FILE *view = fopencookie(range, "rb", (cookie_io_functions_t){mng_base_read, NULL, mng_base_seek, mng_base_close});'),
        ]
        for old, new in swaps:
            if text.count(old) != 1:
                raise B.BuildError(f'hactool funopen hook changed; cannot convert: {old.splitlines()[0]}')
            text = text.replace(old, new)
        main.write_text(text)
    B.run(['make', f'-j{workers}', 'CC=clang'], logs / 'hactool-build.log', cwd=source)
    return source / 'hactool'


def data_lane(tree, game, update, version, keys, logs, firmware, workers):
    timed('base RomFS and fonts', B.step_data, tree, game, keys, logs, firmware=firmware)
    if version == 'v0':
        return
    expected = B.VERSIONS[version]
    folder = tree / f'extracted/{version}/romfs'
    index = folder / 'index.json'
    valid = False
    if index.is_file() and (folder / 'romfs.bin').is_file():
        try:
            report = json.loads(index.read_text())
            valid = (report.get('status') == 'pass' and report['romfs_sha256'] == expected['romfs']['sha256']
                     and (folder / 'romfs.bin').stat().st_size == expected['romfs']['size'])
        except (ValueError, KeyError):
            pass
    if not valid:
        hactool = build_hactool_linux(tree, logs, workers)
        if folder.exists():
            shutil.rmtree(folder)
        B.say(f'data: merging base + {expected["display_version"]} and verifying all IVFC levels')
        t = time.time()
        B.run([tree / '.venv/bin/python', 'scripts/extract_update_romfs.py', game, update, '--keys', keys,
               '--hactool', hactool, '--output', folder], logs / 'update-romfs.log', cwd=tree)
        report = json.loads(index.read_text())
        B.check(f'{expected["display_version"]} RomFS', report['romfs_sha256'], expected['romfs']['sha256'])
        B.check(f'{expected["display_version"]} RomFS size', report['romfs_size'], expected['romfs']['size'])
        B.say(f'   update RomFS merge: {time.time() - t:.0f} s')
    # The same overlay steps as build_app.step_update_data, all at once (each writes its own output folder).
    steps = [('make_romfs_patches.py', []), ('make_output_res_overlay.py', ['2560x1440'])]
    if version == 'v143':
        steps += [('make_output_res_overlay.py', [canvas, '--scene', scene])
                  for canvas in ('1920x1080', '2560x1440') for scene in ('1920x1080', '2560x1440')]
    jobs = [(f'overlay {i}', [tree / '.venv/bin/python', 'scripts/' + script, *arguments, '--version', version],
             logs / (version + '-' + script + (f'-{i}' if i > 1 else '') + '.log'), tree, None)
            for i, (script, arguments) in enumerate(steps)]
    timed('RomFS overlays', run_all, jobs, workers)


def code_lane(tree, game, update, version, keys, env, logs, android_out, workers):
    expected = B.VERSIONS[version]
    py = tree / '.venv/bin/python'
    source = update if version != 'v0' else game
    exefs = tree / f'extracted/{version}/exefs'
    if version == 'v0' and not B.exefs_matches(exefs, expected['exefs_sha256'], expected['title_id']):
        # step_code extracts the base ExeFS; the update ExeFS was extracted by identify_update
        B.run([py, 'scripts/extract_exefs.py', source, '--keys', keys, '--output', exefs], logs / 'extract_exefs-v0.log', cwd=tree)
    if not (tree / f'extracted/{version}/elf/main-inventory.json').exists():
        B.run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / f'extracted/{version}/elf'],
              logs / f'prepare_modules-{version}.log', cwd=tree)
    apple_out = tree / f'rewritten/{version}'
    apple_out.mkdir(parents=True, exist_ok=True)
    android_out.mkdir(parents=True, exist_ok=True)
    apple_env = env
    android_env = dict(apple_env, DM_TARGET='android')
    jobs = []
    for m in MODULES:
        image = apple_out / f'{m}.patched.bin'
        if not (image.exists() and B.sha256(image) == expected['rewritten_sha256'][f'{m}.patched.bin']):
            jobs.append((f'apple {m}', [py, 'third_party/d2r/patch_nso.py', exefs / m, apple_out, m],
                         logs / f'rewrite_{version}_{m}.log', tree, apple_env))
        jobs.append((f'android {m}', [py, REWRITER / 'patch_nso.py', exefs / m, android_out, m],
                     logs / f'android_rewrite_{version}_{m}.log', REWRITER, android_env))
    B.say(f'code: {len(jobs)} rewrites at once')
    timed('rewrites', run_all, jobs, workers)
    # build_app's own checks (ExeFS hashes, npdm signature, rewritten hashes) and its validation report
    B.step_code(tree, source, keys, env, logs, version=version)
    B.say('code: Apple rewrite matches the pinned hashes (extraction verified)')
    venv_py = tree / '.venv/bin/python'
    check_env = dict(env, LLVM_BIN=str(NDK_LLVM))
    jobs = [(f'structck {m}', [venv_py, REWRITER / 'structck.py', android_out, m, exefs / m, '--target', 'android'],
             android_out / f'{m}-structure.log', REWRITER, check_env) for m in MODULES]
    timed('structure checks', run_all, jobs, workers)
    for m in MODULES:   # equiv already uses every core through its own worker pool
        mutate = '300' if m == 'main' else '150'
        timed(f'equivalence {m}', B.run, [venv_py, REWRITER / 'equiv.py', android_out, m, exefs / m, '--target', 'android',
              '--jobs', str(workers), '--min-eff', '2', '--max-extra', '16', '--mutate', mutate,
              '--tsv', android_out / f'{m}-equivalence.tsv'], android_out / f'{m}-equivalence.log', REWRITER, check_env, False)
    for m in MODULES:
        s = next((l for l in (android_out / f'{m}-structure.log').read_text().splitlines() if l.startswith('STRUCT')), '')
        e = next((l for l in (android_out / f'{m}-equivalence.log').read_text().splitlines() if l.startswith('EQUIV')), '')
        if ' OK ' not in s or ' OK ' not in e or '漏抓=0' not in e:
            raise B.BuildError(f'android {m}: checks did not pass\n{s}\n{e}')
        B.say(f'code: android {m}: {s.split(" ", 3)[3][:60]}... | {e.split(" ", 3)[3][:60]}...')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', type=Path)
    ap.add_argument('-k', '--keys', type=Path, required=True)
    ap.add_argument('--update', type=Path)
    ap.add_argument('--firmware', type=Path)
    ap.add_argument('--work', type=Path, default=ROOT / 'work')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    t0 = time.time()
    game, keys = a.game.resolve(), a.keys.resolve()
    work = a.work.resolve(); logs = work / 'logs'; logs.mkdir(parents=True, exist_ok=True)
    if B.input_kind(game) not in ('nsp', 'xci'):
        raise B.BuildError('base game must be an NSP or XCI')
    tree = B.prepare_work(work, Path(sys.executable), logs)
    env = dict(os.environ)
    version, update = 'v0', None
    if a.update:
        version, update = B.identify_update(tree, a.update.resolve(), keys, logs)
    B.say(f'building {B.VERSIONS[version]["display_version"]} with {a.jobs} workers')
    android_out = work / f'android/{version}/rewritten'
    with cf.ThreadPoolExecutor(2) as lanes:
        data = lanes.submit(data_lane, tree, game, update, version, keys, logs, a.firmware.resolve() if a.firmware else None, a.jobs)
        code = lanes.submit(code_lane, tree, game, update, version, keys, env, logs, android_out, a.jobs)
        for f in cf.as_completed([data, code]):
            f.result()
    B.say(f'done in {time.time() - t0:.0f} s: data {tree}/extracted/{version}, android code {android_out}')


if __name__ == '__main__':
    try:
        main()
    except B.BuildError as e:
        sys.exit(f'error: {e}')
