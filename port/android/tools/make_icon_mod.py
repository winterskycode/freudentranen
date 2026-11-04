#!/usr/bin/env python3.14
"""The PlayStation button icons as a built-in mod for the startup screen's merge (app Mods.java, built-in id
_buttons-playstation), made from the player's own RomFS. This is for prepared data under /sdcard/tok on a dev device: from the
player's picked files the app makes the mod itself (IconMod.java, tools/make_icon_asset.py) and does not use this folder.

    work/venv/bin/python port/android/tools/make_icon_mod.py   -> work/android/v143/mods/_buttons-playstation/
        (push_data.sh puts it in /sdcard/tok/v143/mods/)

MacNativeGames' make_button_icons.py --style playstation redraws the face, shoulder, stick and d-pad icons of the
game's icon font (four font archives) and the eight button-guide textures of the common layout archive, and raises
their entries in the resource size table. Its output becomes a mod folder like the ones the Mods page installs:
romfs/ with the files, files.tsv (path, size, SHA-256, the file it replaces' SHA-256, kind, size-table entry) and
mod.json. The size table is not shipped: the merge writes the one table every mod shares, from the entries in
files.tsv. The icons are drawn by the Switch's button positions (east Circle, south Cross, north Triangle, west
Square); the app shows them with the Switch letter positions (host/app.c, TOTK_BUTTON_STYLE=playstation). Game content:
the folder stays in work/ and on the device, never in the repository.
"""
import json, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT/'MacNativeGames/games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
RSTB = 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'
ID = '_buttons-playstation'


def main():
    raw = ROOT/'work/android/v143/button-icons/playstation'
    out = ROOT/f'work/android/v143/mods/{ID}'
    if not (raw/'manifest.tsv').exists():
        subprocess.run([str(ROOT/'work/venv/bin/python'), str(SCRIPTS/'make_button_icons.py'), str(raw), '--romfs',
                        str(ROOT/'work/totk/extracted/v143/romfs'), '--version', 'v143', '--style', 'playstation'],
                       cwd=SCRIPTS, check=True)
    import compression.zstd, zlib
    from port_mod import restbl_read, rstb_key   # the size table as the PC merge tool reads it
    _, _, by_crc, by_name = restbl_read(compression.zstd.decompress((raw/RSTB).read_bytes()))
    shutil.rmtree(out, ignore_errors=True)
    rows, total = [], 0
    for line in (raw/'manifest.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        path, size, sha, orig = line.split('\t')[:4]
        if path == RSTB:
            continue
        (out/'romfs'/path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(raw/path, out/'romfs'/path)
        key = rstb_key(path)
        hint = by_name.get(key, by_crc.get(zlib.crc32(key.encode()), 0))
        rows.append(f'{path}\t{size}\t{sha}\t{orig}\tcopy\t{hint}')
        total += int(size)
    (out/'files.tsv').write_text(''.join(r + '\n' for r in rows))
    (out/'mod.json').write_text(json.dumps(dict(
        name='PlayStation button icons', author='MacNativeGames make_button_icons.py', version='',
        description='Built in: on while Settings > Controls > Button letters is PlayStation.', source='built in',
        made='1.4.3', installed=int(time.time()), bytes=total, replaced=len(rows), added=0, skipped=0,
        contents=['Font archives', 'Button guide textures'], warnings=[]), indent=1) + '\n')
    print(f'{out}: {len(rows)} files, {total >> 20} MB')


if __name__ == '__main__':
    main()
