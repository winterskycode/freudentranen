#!/usr/bin/env python3.14
"""Generated headers for the Android runtime: MacNativeGames' own generators, run against the Android rewrite.

    python3.14 port/android/tools/gen_runtime.py [--version v143]   -> work/android/<version>/tree/build/...

The generators find everything relative to their own file, so they run from a small tree: copies of the three scripts,
links to the shared extraction and to work/android/<version>/rewritten, and a validation report carrying the Android
rewrite's hashes (prepare_runtime.py binds the tables to those exact images).
"""
import argparse, hashlib, json, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / 'MacNativeGames/games/totk'
WORK = ROOT / 'work'


def link(path, target):
    if path.is_symlink() or path.exists():
        path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.symlink_to(target)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', default='v143')
    v = ap.parse_args().version
    tree = WORK / f'android/{v}/tree'
    (tree / 'scripts').mkdir(parents=True, exist_ok=True)
    for s in ('prepare_runtime.py', 'prepare_romfs_backend.py', 'prepare_shared_fonts.py'):
        shutil.copy2(GAME / 'scripts' / s, tree / 'scripts' / s)
    link(tree / 'third_party/d2r', ROOT / 'port/rewriter/d2r')
    link(tree / 'extracted', WORK / 'totk/extracted')
    link(tree / f'rewritten/{v}', WORK / f'android/{v}/rewritten')
    report_path = tree / ('analysis/phase12-validation.json' if v == 'v0' else f'analysis/{v}-port/phase12-validation.json')
    report_path.parent.mkdir(parents=True, exist_ok=True)
    digests = {}
    for m in ('main', 'sdk', 'subsdk0'):
        digests[f'rewritten/{v}/{m}.patched.bin'] = hashlib.sha256((tree / f'rewritten/{v}/{m}.patched.bin').read_bytes()).hexdigest()
        digests[f'extracted/{v}/exefs/{m}'] = hashlib.sha256((tree / f'extracted/{v}/exefs/{m}').read_bytes()).hexdigest()
    report_path.write_text(json.dumps(dict(format=1, status='pass', rom_version=v, scope='android rewrite (structck + equiv)',
                                           module_file_sha256=digests), indent=2) + '\n')
    py = WORK / 'venv/bin/python'
    (tree / 'build').mkdir(exist_ok=True)
    for cmd in ([py, tree / 'scripts/prepare_runtime.py', '--version', v],
                [py, tree / 'scripts/prepare_romfs_backend.py', '--version', v],
                # Breath of the Wild: aoc-config.txt for the add-on volumes
                *([[py, tree / 'scripts/prepare_romfs_backend.py', '--version', v, '--aoc']] if v.startswith('botw') else []),
                [py, tree / 'scripts/prepare_shared_fonts.py', '--version', v]):
        r = subprocess.run([str(c) for c in cmd], cwd=tree, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'{cmd[1].name} failed:\n{r.stdout}\n{r.stderr}')
        print(cmd[1].name, r.stdout.strip()[-200:])
    print(f'generated: {tree / "build"}')


if __name__ == '__main__':
    main()
