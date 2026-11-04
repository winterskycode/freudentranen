#!/usr/bin/env python3
"""Checks the cut-down shared font (tools/fetch_font.sh) against the game, on a PC. Prints code points and counts, no game text.

The shared fonts the game can ask for are the names in its complex fonts (fcpx/*.bfcpx in Font/*.bfarc.zs). Each lists fonts in
order, each with the character ranges it serves; a font with no ranges takes the rest. For every language this finds which
characters of its messages (Mals/*.sarc.zs) a list sends to a shared font, and whether SharedFont.otf has them.

    python3.14 port/android/tools/check_shared_font.py port/android/deps/out/fonts/SharedFont.otf [--extracted work/totk/extracted/v143]

Needs Python 3.14 (compression.zstd) and fontTools.
"""
import argparse, io, json, re, struct, sys
from pathlib import Path
import compression.zstd as zstd
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[3]
FONT_ARCHIVES = {'CNzh': 'Font/Font_CNzh.Nin_NX_NVN.bfarc.zs', 'KRko': 'Font/Font_KRko.Nin_NX_NVN.bfarc.zs', 'TWzh': 'Font/Font_TWzh.Nin_NX_NVN.bfarc.zs'}
BASE_ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'
SHARED = re.compile(r'nintendo_(udsg|udjxh|ext)')   # the six names main+0x105dc60 maps to slots


def sarc(d):
    assert d[:4] == b'SARC'
    head, _, start = struct.unpack_from('<HxxII', d, 4)
    count = struct.unpack_from('<H', d, head + 6)[0]; names = head + 12 + count*16 + 8
    out = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', d, head + 12 + i*16)
        at = names + (attr & 0xffffff)*4
        out[d[at:d.index(0, at)].decode()] = d[start + lo:start + hi]
    return out


def fcpx(b):
    """[(font name, [(first, last)...])] in order: entries of 0x28 bytes from 0x3c, offsets from 0x18."""
    out, at, base = [], 0x3c, 0x18
    while at + 0x28 <= len(b):
        _, _, name, _, _, _, _, count, ranges, _ = struct.unpack_from('<fIIIfIIIII', b, at)
        if not name or base + name >= len(b): break
        text = b[base + name:b.index(0, base + name)].decode('ascii', 'replace')
        if not re.fullmatch(r'[\w.\-]+\.(bfttf|bfotf)', text): break
        out.append((text, [struct.unpack_from('<II', b, base + ranges + 8*i) for i in range(count)]))
        at += 0x28
    return out


def msbt_chars(b):
    out = set()
    if b[:8] != b'MsgStdBn': return out
    e = '>' if b[8:10] == b'\xfe\xff' else '<'
    p = 0x20
    for _ in range(struct.unpack_from(e + 'H', b, 0xe)[0]):
        size = struct.unpack_from(e + 'I', b, p + 4)[0]; body = b[p + 0x10:p + 0x10 + size]
        if b[p:p + 4] == b'TXT2':
            n = struct.unpack_from(e + 'I', body, 0)[0]
            offs = list(struct.unpack_from(f'{e}{n}I', body, 4)) + [size]
            for i in range(n):
                t = body[offs[i]:offs[i + 1]]; q = 0
                while q + 2 <= len(t):
                    c = struct.unpack_from(e + 'H', t, q)[0]; q += 2
                    if c == 0x0e: q += 4; q += 2 + struct.unpack_from(e + 'H', t, q)[0]   # a tag and its data
                    elif c == 0x0f: q += 4
                    elif c: out.add(c)
        p = (p + 0x10 + size + 15) & ~15
    return out


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('font', type=Path)
    a.add_argument('--extracted', type=Path, default=ROOT/'work/totk/extracted/v143')
    args = a.parse_args()
    files = {f['path']: f for f in json.loads((args.extracted/'romfs/index.json').read_text())['files']}
    image = (args.extracted/'romfs/romfs.bin').open('rb')
    def read(p): f = files[p]; image.seek(f['offset']); return image.read(f['size'])
    dicts = {}
    for b in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
        z = zstd.ZstdDict(b); dicts[z.dict_id] = z
    def unzs(raw):
        i = zstd.get_frame_info(raw); return zstd.decompress(raw, zstd_dict=dicts[i.dictionary_id]) if i.dictionary_id else zstd.decompress(raw)
    have = TTFont(args.font, lazy=True).getBestCmap()
    bad = 0
    base = [n for n, b in sarc(unzs(read(BASE_ARCHIVE))).items() if n.endswith('.bfcpx') and SHARED.search(b.decode('latin-1'))]
    print(f'{BASE_ARCHIVE}: {len(base)} complex fonts name a shared font')
    bad += len(base)
    for lang, archive in FONT_ARCHIVES.items():
        lists = [fcpx(b) for n, b in sarc(unzs(read(archive))).items() if n.endswith('.bfcpx')]
        names = {f for l in lists for f, _ in l if SHARED.match(f)}
        chars = set()
        for p, f in files.items():
            if p.startswith(f'Mals/{lang}.'):
                for n, b in sarc(unzs(read(p))).items():
                    if n.endswith('.msbt'): chars |= msbt_chars(b)
        reached = set()
        for l in lists:
            for c in chars:
                for font, ranges in l:
                    if not ranges or any(lo <= c <= hi for lo, hi in ranges):
                        if SHARED.match(font): reached.add(c)
                        break
        lacks = sorted(c for c in reached if c not in have)
        bad += len(lacks)
        print(f'{lang}: shared fonts named {sorted(names)}; {len(chars)} characters in its messages, {len(reached)} reach them, the font lacks {len(lacks)}', [f'{c:x}' for c in lacks])
        print('   ', ' '.join(f'{c:x}' for c in sorted(reached)))
    print('PASS' if not bad else 'FAIL')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
