#!/usr/bin/env python3
"""Import and export the game's save on the device, with backups.

    python3 port/android/tools/saves.py export <new folder>     the device's save -> an unpacked save folder
    python3 port/android/tools/saves.py import <save folder>    an unpacked save folder -> the device (backed up first)
    python3 port/android/tools/saves.py backups                 the backups on the device
    python3 port/android/tools/saves.py restore <backup name>   put a backup back (the save it replaces is backed up too)
    [--profile <name>]: a profile other than the player's (files/<name>, default profile)

A save folder is what Ryujinx, Yuzu or Eden open with "Open Save Data Location" (slot_00 ... slot_05, option.sav, album,
...), or one exported here; MacNativeGames' save_manager.py checks it (validate_save) and this tool copies it unchanged.
On the device the save is files/<profile>/save/<account>/ (the account folder named from users.txt, or account.uid in an
older profile, as the game names it). The game is stopped first. Before an import or restore the device's save is copied to files/save-backups/<time>/ on
the device and to work/android/save-backups/<time>.tar on the PC; every file is then checked by SHA-256 on both sides.
"""
import argparse, hashlib, io, struct, subprocess, sys, tarfile, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'MacNativeGames/games/totk/scripts'))
from save_manager import validate_save   # noqa: E402

APP = 'dev.tok.totk'
PC_BACKUPS = ROOT/'work/android/save-backups'


def adb(*args, check=True, binary=False):
    r = subprocess.run(['adb', *args], capture_output=True, text=not binary, timeout=600)
    if check and r.returncode:
        raise SystemExit(f'adb {" ".join(args)}: {r.stderr or r.stdout}')
    return r.stdout


def as_app(cmd, **kw):
    # exec-out passes the command as it is
    return adb('exec-out', 'run-as', APP, 'sh', '-c', cmd, **kw)


def account(profile):
    # users.txt (runtime/account.c): "[*]<folder>\t<name>" a line, * on the user in force; account.uid in older profiles
    users = adb('exec-out', 'run-as', APP, 'cat', f'files/{profile}/users.txt', check=False, binary=True).decode('utf8', 'replace').splitlines()
    users = [l for l in users if '\t' in l and 'No such file' not in l]
    if users:
        return next((l for l in users if l.startswith('*')), users[0]).lstrip('*').split('\t')[0]
    raw = adb('exec-out', 'run-as', APP, 'cat', f'files/{profile}/account.uid', binary=True)
    if len(raw) != 16 or not any(raw):
        raise SystemExit(f'files/{profile}/account.uid missing or invalid: start the game once with this profile')
    lo, hi = struct.unpack('<QQ', raw)
    # as the game and save_manager.py name the folder
    return f'{hi:016x}{lo:016x}'


def device_sums(path):
    """SHA-256 of every file under files/<path> on the device, by relative path."""
    out = as_app(f'cd files/{path} && find . -type f -exec sha256sum {{}} +', check=False)
    return {line.split(None, 1)[1][2:]: line.split()[0] for line in out.splitlines() if '  ./' in line}


def local_sums(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file()}


def stop():
    adb('shell', f'am force-stop {APP}')


def backup(profile, acct):
    """files/save-backups/<time>/ on the device and work/android/save-backups/<time>.tar here; returns the name."""
    name = time.strftime('%Y%m%d-%H%M%S') + f'-{profile}'
    src = f'files/{profile}/save/{acct}'
    if not as_app(f'test -d {src} && echo yes', check=False).strip():
        return None
    as_app(f'mkdir -p files/save-backups && cp -a {src} files/save-backups/{name}')
    if device_sums(f'save-backups/{name}') != device_sums(f'{profile}/save/{acct}'):
        raise SystemExit('the backup on the device does not match the save: nothing was changed')
    PC_BACKUPS.mkdir(parents=True, exist_ok=True)
    tar = adb('exec-out', 'run-as', APP, 'tar', '-C', f'files/save-backups/{name}', '-cf', '-', '.', binary=True)
    (PC_BACKUPS/f'{name}.tar').write_bytes(tar)
    with tarfile.open(PC_BACKUPS/f'{name}.tar') as t:
        sums = {m.name[2:]: hashlib.sha256(t.extractfile(m).read()).hexdigest() for m in t.getmembers() if m.isfile()}
    if sums != device_sums(f'save-backups/{name}'):
        raise SystemExit('the backup copied to the PC does not match: nothing was changed')
    print(f'backed up: files/save-backups/{name} on the device, {PC_BACKUPS/name}.tar')
    return name


def put(source, profile, acct):
    """Replace files/<profile>/save/<acct> with the local folder source, checked file by file."""
    want = local_sums(source)
    tmp = '/data/local/tmp/save-import'
    adb('shell', f'rm -rf {tmp}')
    adb('push', str(source), tmp)
    adb('shell', f'chmod -R a+rX {tmp}')
    dst = f'files/{profile}/save/{acct}'
    as_app(f'rm -rf {dst}.new && mkdir -p {dst}.new && cp -r {tmp}/. {dst}.new/')
    adb('shell', f'rm -rf {tmp}')
    if device_sums(f'{profile}/save/{acct}.new') != want:
        as_app(f'rm -rf {dst}.new')
        raise SystemExit('the copy on the device does not match the folder: the save was not changed')
    as_app(f'rm -rf {dst} && mv {dst}.new {dst}')
    print(f'{source} -> files/{profile}/save/{acct}: {len(want)} files, checked')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('action', choices=('import', 'export', 'backups', 'restore'))
    ap.add_argument('target', nargs='?', help='the save folder (import, export) or the backup name (restore)')
    ap.add_argument('--profile', default='profile', help='files/<profile> (default: the player\'s, files/profile)')
    a = ap.parse_args()
    if a.action == 'backups':
        print(as_app('ls -1 files/save-backups 2>/dev/null', check=False) or 'no backups')
        return
    if not a.target:
        ap.error(f'{a.action} needs a folder or a backup name')
    acct = account(a.profile)
    if a.action == 'export':
        dst = Path(a.target).expanduser()
        if dst.exists():
            raise SystemExit(f'{dst}: the export goes to a new folder')
        stop()
        tar = adb('exec-out', 'run-as', APP, 'tar', '-C', f'files/{a.profile}/save/{acct}', '-cf', '-', '.', binary=True)
        with tempfile.TemporaryDirectory(dir=dst.parent if dst.parent.exists() else None) as tmp:
            with tarfile.open(fileobj=io.BytesIO(tar)) as t:
                t.extractall(tmp, filter='data')
            if local_sums(Path(tmp)) != device_sums(f'{a.profile}/save/{acct}'):
                raise SystemExit('the export does not match the save on the device')
            validate_save(tmp)
            Path(tmp).rename(dst)
        print(f'files/{a.profile}/save/{acct} -> {dst}: checked')
        return
    stop()
    if a.action == 'import':
        source, slots = validate_save(Path(a.target).expanduser())
        print(f'importing {source}: {", ".join(slots)}')
        backup(a.profile, acct)
        put(source, a.profile, acct)
    else:
        name = a.target
        if not as_app(f'test -d files/save-backups/{name} && echo yes', check=False).strip():
            raise SystemExit(f'no backup {name} (saves.py backups lists them)')
        with tempfile.TemporaryDirectory() as tmp:
            tar = adb('exec-out', 'run-as', APP, 'tar', '-C', f'files/save-backups/{name}', '-cf', '-', '.', binary=True)
            with tarfile.open(fileobj=io.BytesIO(tar)) as t:
                t.extractall(tmp, filter='data')
            if local_sums(Path(tmp)) != device_sums(f'save-backups/{name}'):
                raise SystemExit('the backup could not be read back intact: nothing was changed')
            backup(a.profile, acct)
            put(Path(tmp), a.profile, acct)


if __name__ == '__main__':
    main()
