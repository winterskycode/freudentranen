#!/usr/bin/env python3.14
"""Checks a PlayStation icon mod made by IconMod.java (run.sh) against the game and the PC tool. Prints counts, no game data.

    python3.14 compare.py <mod folder> [--root <repository with MacNativeGames and work/>] [--font NotoSansCJKsc-Bold.otf]

Three references:
- the game's own files: what the mod did not draw must be byte for byte the same;
- MacNativeGames' make_button_icons.py run here with Noto in place of the game's text fonts: the same drawings, so every glyph
  and mask must match exactly;
- the mod the PC tool made (work/android/v143/mods): only the letters on keys may differ.

Needs Python 3.14 (compression.zstd) and fontTools.
"""
import argparse, hashlib, io, json, struct, sys
from pathlib import Path
import compression.zstd as zstd
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.ttLib import TTFont

FONTS = ['Font/Font.Nin_NX_NVN.bfarc.zs', 'Font/Font_CNzh.Nin_NX_NVN.bfarc.zs', 'Font/Font_KRko.Nin_NX_NVN.bfarc.zs', 'Font/Font_TWzh.Nin_NX_NVN.bfarc.zs']
LAYOUT = 'UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs'
LETTERS = {0xE044, 0xE045, 0xE046, 0xE047, 0xE052, 0xE05E}   # the icons with letters on them
LETTER_MASKS = {'Nt_KeyTexL_00', 'Nt_KeyTexZL_00', 'Nt_KeyTexLSt_00', 'Nt_KeyTexLSt_01'}
problems = []


def check(ok, what):
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        problems.append(what)


def tables(otf):
    """tag -> (directory place, checksum, offset, length), in directory order."""
    return {otf[12 + 16 * i:16 + 16 * i].decode(): (12 + 16 * i, *struct.unpack_from('>III', otf, 16 + 16 * i)) for i in range(struct.unpack_from('>H', otf, 4)[0])}


def body(otf, entries, tag):
    return otf[entries[tag][2]:entries[tag][2] + entries[tag][3]]


def cff_index(cff, at):
    """(data start - 1, offsets, end) of a CFF INDEX."""
    n = struct.unpack_from('>H', cff, at)[0]
    if not n:
        return at + 2, [], at + 2
    size = cff[at + 2]
    offsets = [int.from_bytes(cff[at + 3 + i * size:at + 3 + (i + 1) * size], 'big') for i in range(n + 1)]
    base = at + 3 + (n + 1) * size - 1
    return base, offsets, base + offsets[-1]


def charstrings_at(cff):
    """Where the CharStrings INDEX starts and ends, from the fontTools-independent layout."""
    font_top = cff_index(cff, cff_index(cff, cff[2])[2])
    d = cff[font_top[0] + font_top[1][0]:font_top[0] + font_top[1][1]]
    i, stack = 0, []
    while i < len(d):
        b = d[i]
        if b == 28: stack.append(struct.unpack_from('>h', d, i + 1)[0]); i += 3
        elif b == 29: stack.append(struct.unpack_from('>i', d, i + 1)[0]); i += 5
        elif 32 <= b <= 246: stack.append(b - 139); i += 1
        elif 247 <= b <= 250: stack.append((b - 247) * 256 + d[i + 1] + 108); i += 2
        elif 251 <= b <= 254: stack.append(-(b - 251) * 256 - d[i + 1] - 108); i += 2
        else:
            op = b; i += 1
            if b == 12: i += 1; op = 1200 + d[i - 1]
            if op == 17: return stack[-1]
            stack = []
    raise ValueError('no CharStrings')


def sarc_nodes(d):
    start = struct.unpack_from('<I', d, 12)[0]; count = struct.unpack_from('<H', d, 0x1a)[0]
    return start, [struct.unpack_from('<4I', d, 0x20 + 16 * i) for i in range(count)]


def compare_sarc(m, new, old, changed):
    """The archive's frame around the one changed member; the members by name. True if all holds."""
    start, nodes = sarc_nodes(new); ostart, onodes = sarc_nodes(old)
    head = bytearray(new[:start]); ohead = bytearray(old[:ostart])
    for h, n in ((head, len(nodes)), (ohead, len(onodes))):
        h[8:12] = b'\0' * 4
        for i in range(n):
            h[0x20 + 16 * i + 8:0x20 + 16 * i + 16] = b'\0' * 8
    same = start == ostart and bytes(head) == bytes(ohead)
    a, b = m.sarc_members(new), m.sarc_members(old)
    same = same and list(a) == list(b) and struct.unpack_from('<I', new, 8)[0] == len(new)
    for k in b:
        if k != changed:
            same = same and new[slice(*a[k])] == old[slice(*b[k])]
    def low(o):
        return o & -o if o else 1 << 30
    for x, y in zip(nodes, onodes):   # each member at least as aligned as the game has it, up to the PC tool's 8 KiB limit
        same = same and x[2] % min(low(y[2]), 0x2000) == 0
    used = sorted(a.values()); gaps = zip([(0, start)] + used, used)
    same = same and all(not any(new[p[1]:q[0]]) for p, q in gaps if p[1] > start)
    return same


def contours(font_glyphs, name):
    rec = RecordingPen(); font_glyphs[name].draw(rec); out, cur = [], []
    for op, args in rec.value:
        cur.append((op, args))
        if op in ('closePath', 'endPath'):
            out.append(tuple(cur)); cur = []
    return out


def bounds_of(contour):
    pts = [p for _, args in contour for p in args]
    return min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)


def compare_font(m, otf, original, reference, pc):
    new, old = tables(otf), tables(original)
    check(list(new) == list(old), 'font: same tables in the same directory order')
    check(sorted(new, key=lambda t: new[t][2]) == sorted(old, key=lambda t: old[t][2]), 'font: same tables in the same file order')
    check(all(body(otf, new, t) == body(original, old, t) for t in old if t not in ('CFF ', 'hmtx', 'head')), 'font: every table but CFF, hmtx and head byte-identical')
    check(all(new[t][1] == old[t][1] for t in old if t not in ('CFF ', 'hmtx', 'head')), 'font: their directory checksums too')
    check(otf[:12] == original[:12], 'font: the sfnt header identical')
    h, oh = bytearray(body(otf, new, 'head')), bytearray(body(original, old, 'head'))
    h[8:12] = oh[8:12] = b'\0' * 4
    check(h == oh, 'font: head differs only in checkSumAdjustment')
    total = sum(struct.unpack(f'>{len(otf) // 4}I', otf)) & 0xffffffff
    check(total == 0xb1b0afba and len(otf) % 4 == 0, 'font: the whole-file checksum holds')
    for t, (_, sum_, at, size) in new.items():
        padded = otf[at:at + size] + b'\0' * (-size % 4)
        if t == 'head': padded = padded[:8] + b'\0' * 4 + padded[12:]
        if sum(struct.unpack(f'>{len(padded) // 4}I', padded)) & 0xffffffff != sum_:
            check(False, f'font: checksum of {t}')
    check(all(new[t][2] % 4 == 0 for t in new), 'font: tables 4-byte aligned')

    f = TTFont(io.BytesIO(otf), checkChecksums=2); ref = TTFont(io.BytesIO(reference)); old_font = TTFont(io.BytesIO(original)); pcf = TTFont(io.BytesIO(pc))
    order = f.getGlyphOrder(); cmap = f.getBestCmap(); names = {v: k for k, v in cmap.items()}
    check(order == old_font.getGlyphOrder() and cmap == old_font.getBestCmap(), 'font: fontTools opens it (checksums on); glyph order and cmap the game\'s')
    def code(font, g):
        cs = font['CFF '].cff.topDictIndex[0].CharStrings[g]; cs.decompile(); return cs.program
    def codes(font):
        return {g: code(font, g) for g in order}
    mine, theirs, game, pcs = codes(f), codes(ref), codes(old_font), codes(pcf)
    drawn = {names[g] for g in order if mine[g] != game[g]}
    pc_drawn = {names[g] for g in order if pcs[g] != game[g]}
    check(drawn == pc_drawn, f'font: {len(drawn)} code points redrawn, the same as the PC tool\'s')
    check(mine == theirs, f'font: all {len(order)} glyphs equal the PC tool with Noto letters (decompiled programs)')
    check(all(f['hmtx'][g] == ref['hmtx'][g] for g in order), 'font: hmtx equals the PC tool with Noto letters')
    differ = {names[g] for g in order if mine[g] != pcs[g]}
    check(differ == LETTERS, f'font: against the PC mod {len(drawn) - len(differ)} icons identical; those that differ are the {len(differ)} with letters')
    check(all(f['hmtx'][g][0] == pcf['hmtx'][g][0] for g in order), 'font: advances equal the PC mod\'s')
    print('      left side bearings different from the PC mod:', [hex(names[g]) for g in order if f['hmtx'][g] != pcf['hmtx'][g]])
    gs, ps = f.getGlyphSet(), pcf.getGlyphSet()
    for cp in sorted(LETTERS):
        a, b = contours(gs, cmap[cp]), contours(ps, cmap[cp])
        kept = [c for c in a if c in b]; only = [c for c in a + b if not (c in a and c in b)]
        outer = bounds_of(a[0]); inside = all(outer[0] < bounds_of(c)[0] and outer[1] < bounds_of(c)[1] and bounds_of(c)[2] < outer[2] and bounds_of(c)[3] < outer[3] for c in only)
        check(a[0] == b[0] and inside, f'font: U+{cp:04X}: key shape identical ({len(kept)} contours), only {len(only)} letter contours differ, all inside it')

    # the bounds that are not updated must still hold
    box = [1e9, 1e9, -1e9, -1e9]; lsb = 1e9; rsb = 1e9; wide = 0
    for g in order:
        b = BoundsPen(gs); gs[g].draw(b); w, l = f['hmtx'][g]; wide = max(wide, w)
        if b.bounds:
            box = [min(box[0], b.bounds[0]), min(box[1], b.bounds[1]), max(box[2], b.bounds[2]), max(box[3], b.bounds[3])]
            lsb = min(lsb, l); rsb = min(rsb, w - b.bounds[2])
    head, hhea = f['head'], f['hhea']; top = f['CFF '].cff.topDictIndex[0]
    check(head.xMin <= box[0] and head.yMin <= box[1] and head.xMax >= box[2] and head.yMax >= box[3], f'font: all outlines {box} inside head\'s box')
    check(list(top.FontBBox) == [head.xMin, head.yMin, head.xMax, head.yMax], 'font: FontBBox as the game has it')
    check(hhea.advanceWidthMax >= wide and hhea.minLeftSideBearing <= lsb and hhea.minRightSideBearing <= rsb and hhea.xMaxExtent >= box[2], 'font: hhea\'s limits still hold')
    print(f'      the PC mod\'s head xMin {pcf["head"].xMin}, this font\'s {head.xMin}: fontTools recalculates it, the game\'s value stays here')

    # CFF: outside the CharStrings only the Private offset
    a, b = body(otf, new, 'CFF '), body(original, old, 'CFF ')
    at = charstrings_at(b); check(at == charstrings_at(a), 'CFF: CharStrings where the game has them')
    ob, oo, oe = cff_index(b, at); nb, no, ne = cff_index(a, at)
    check(a[ne:] == b[oe:], f'CFF: everything after CharStrings (Private DICT, Subrs) identical, {len(b) - oe} bytes')
    pre = [i for i in range(at) if a[i] != b[i]]
    top_index = cff_index(b, cff_index(b, b[2])[2]); top_at = top_index[0] + top_index[1][0]; top_end = top_index[0] + top_index[1][1]
    check(len(pre) <= 2 and all(top_at <= i < top_end for i in pre), f'CFF: before CharStrings {len(pre)} bytes differ, the Private offset in the Top DICT')
    print(f'      CharStrings {oe - at} -> {ne - at} bytes')
    same = sum(a[nb + no[i]:nb + no[i + 1]] == b[ob + oo[i]:ob + oo[i + 1]] for i in range(len(oo) - 1))
    check(same == len(order) - len(drawn), f'CFF: {same} of {len(order)} charstrings byte-identical, the rest are the drawn ones')

    # hmtx: the drawn rows only
    x, y = body(otf, new, 'hmtx'), body(original, old, 'hmtx')
    rows = {i // 4 for i in range(len(y)) if x[i] != y[i]}
    want = {f.getGlyphID(cmap[cp]) for cp in drawn}
    check(len(x) == len(y) and rows <= want, f'hmtx: {len(rows)} rows differ, all of drawn glyphs')


def textures(m, bntx):
    """name -> (data offset, width, height, block height): the eight textures."""
    out = {}; pos = 0
    while (i := bntx.find(b'BRTI', pos)) >= 0:
        pos = i + 4
        at = struct.unpack_from('<Q', bntx, i + 0x60)[0]; n = struct.unpack_from('<H', bntx, at)[0]
        name = bntx[at + 2:at + 2 + n].decode().split('^')[0]
        if name in m.HUD_TEXTURES:
            w, h = struct.unpack_from('<II', bntx, i + 0x24); layout = struct.unpack_from('<I', bntx, i + 0x34)[0]
            out[name] = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, i + 0x70)[0])[0], w, h, 1 << (layout & 7)
    return out


def pixels(m, b, at, w, h, block):
    return bytes(b[at + m.gob_offset(x, y, (w + 63) // 64, block)] for y in range(h) for x in range(w))


def read_asset(path):
    """The drawings: (outlines, masks by name)."""
    f = path.read_bytes(); glyphs, masks = struct.unpack_from('<HH', f, 8); at = 12; outlines, out = [], {}
    for _ in range(glyphs):
        n = struct.unpack_from('<H', f, at + 6)[0]; at += 8; outlines.append(f[at:at + n]); at += n
    for _ in range(masks):
        size, n = struct.unpack_from('<HB', f, at); at += 3; name = f[at:at + n].decode(); at += n; out[name] = f[at:at + size * size]; at += size * size
    return outlines, out


def compare_asset(m, path, game_fonts, game_masks):
    """Nothing of the game in the drawings: no run of 12 bytes of an outline occurs in the game's fonts, no mask is the game's."""
    outlines, masks = read_asset(path); run = 12
    windows = {o[i:i + run] for o in outlines for i in range(len(o) - run + 1)}
    found = sum(f[i:i + run] in windows for f in game_fonts for i in range(len(f) - run + 1))
    check(not found, f'asset: {path.stat().st_size} bytes, none of its {len(windows)} outline runs in the game\'s icon and text fonts ({sum(map(len, game_fonts))} bytes)')
    check(set(masks) == set(game_masks) and all(masks[k] != game_masks[k] for k in masks), 'asset: its masks are not the game\'s')


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('mod', type=Path)
    a.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    a.add_argument('--font', type=Path, help='default: port/android/deps/src/noto-sans-cjk/NotoSansCJKsc-Bold.otf')
    a.add_argument('--asset', type=Path, help='default: port/android/deps/out/icons/playstation.bin')
    args = a.parse_args()
    root = args.root
    sys.path.insert(0, str(root/'MacNativeGames/games/totk/scripts'))
    import make_button_icons as m
    from make_mod_manifest import read_index
    noto = TTFont(args.font or root/'port/android/deps/src/noto-sans-cjk/NotoSansCJKsc-Bold.otf')
    romfs = root/'work/totk/extracted/v143/romfs'; pc_dir = root/'work/android/v143/mods/_buttons-playstation'
    index = read_index(romfs/'runtime-index.bin'); image = (romfs/'romfs.bin').open('rb')
    def read(p):
        o, s = index[p]; image.seek(o); return image.read(s)
    pack = zstd.decompress(read('Pack/ZsDic.pack.zs')); dicts = {}; dict_names = {}
    for n, (lo, hi) in m.sarc_members(pack).items():
        z = zstd.ZstdDict(pack[lo:hi]); dicts[z.dict_id] = z; dict_names[z.dict_id] = n
    def unzs(b):
        i = zstd.get_frame_info(b); return zstd.decompress(b, zstd_dict=dicts[i.dictionary_id])

    rows = [l.split('\t') for l in (args.mod/'files.tsv').read_text().splitlines()]
    pc_rows = [l.split('\t') for l in (pc_dir/'files.tsv').read_text().splitlines()]
    check([r[0] for r in rows] == [r[0] for r in pc_rows] == FONTS + [LAYOUT], 'files.tsv: the PC tool\'s five paths, in its order')
    check(all(len(r) == 6 and r[4] == 'copy' for r in rows), 'files.tsv: six columns, kind copy')
    mod_json, pc_json = json.loads((args.mod/'mod.json').read_text()), json.loads((pc_dir/'mod.json').read_text())
    check(set(mod_json) == set(pc_json), 'mod.json: the PC tool\'s fields')
    check(all(mod_json[k] == pc_json[k] for k in ('name', 'description', 'source', 'made', 'replaced', 'added', 'skipped', 'contents', 'warnings')), 'mod.json: the same values but author, version, time and size')
    check(mod_json['bytes'] == sum(int(r[1]) for r in rows), 'mod.json: bytes')
    check(sorted(str(p.relative_to(args.mod)) for p in args.mod.rglob('*') if p.is_file()) == sorted(['files.tsv', 'mod.json'] + ['romfs/' + r[0] for r in rows]), 'folder: nothing else in it')

    fonts, game_fonts, game_masks = {}, [], {}
    for r, pr in zip(rows, pc_rows):
        path, size, sha, orig, _, hint = r
        raw = read(path); data = (args.mod/'romfs'/path).read_bytes()
        info, oinfo = zstd.get_frame_info(data), zstd.get_frame_info(raw)
        check(int(size) == len(data) and sha == hashlib.sha256(data).hexdigest() and orig == hashlib.sha256(raw).hexdigest(), f'{path}: size and hashes')
        check(info.dictionary_id == oinfo.dictionary_id == 1 and info.decompressed_size is not None, f'{path}: dictionary {dict_names[info.dictionary_id]}, content size in the frame')
        new, old = unzs(data), unzs(raw)
        pc = unzs((pc_dir/'romfs'/path).read_bytes())
        check(int(hint) - int(pr[5]) == len(new) - len(pc), f'{path}: size-table room {hint} (PC {pr[5]}), the difference is the archives\' size difference')
        if path == LAYOUT:
            check(compare_sarc(m, new, old, m.BNTX), f'{path}: frame and all members but the textures byte-identical to the game\'s')
            check(len(new) == len(old), f'{path}: same size')
            check(orig != pr[3], f'{path}: its original is the game\'s own file; the PC mod names the PC build\'s overlay')
            nm, om, pm = m.sarc_members(new), m.sarc_members(old), m.sarc_members(pc)
            differ = [k for k in om if new[slice(*nm[k])] != pc[slice(*pm[k])]]
            check(sorted(differ) == sorted([m.BNTX, 'anim/ShortCutBG_00_In.bflan', 'anim/AutoBuilder_00_In.bflan']), f'{path}: against the PC mod only the textures and the capture rule\'s two animations differ (prompts_romfs.c edits those at open)')
            bntx, obntx, pbntx = new[slice(*nm[m.BNTX])], old[slice(*om[m.BNTX])], pc[slice(*pm[m.BNTX])]
            info_t = textures(m, obntx); check(len(info_t) == 8 and textures(m, bntx) == info_t, 'BNTX: the eight textures where the game has them')
            icons = m.STYLES['playstation'](m.Glyphs([noto])); patched = bytearray(obntx)
            for name, (at, w, h, block) in info_t.items():
                mine, pcm = pixels(m, bntx, at, w, h, block), pixels(m, pbntx, at, w, h, block); ref = m.rasterize(icons[m.HUD_TEXTURES[name]][1], w)
                game_masks[name] = pixels(m, obntx, at, w, h, block)
                for y in range(h):
                    for x in range(w):
                        o = at + m.gob_offset(x, y, (w + 63) // 64, block); patched[o] = mine[y * w + x]
                check(mine == ref, f'BNTX: {name} equals the PC tool with Noto letters')
                if name in LETTER_MASKS:
                    d = [(i % w, i // w) for i in range(w * h) if mine[i] != pcm[i]]
                    box = min(p[0] for p in d), min(p[1] for p in d), max(p[0] for p in d), max(p[1] for p in d)
                    edge = 6   # the key's outline is the same: no difference at the border
                    check(box[0] > edge and box[1] > edge and box[2] < w - edge and box[3] < h - edge, f'BNTX: {name}: {len(d)} of {w * h} pixels differ from the PC mod, in the box {box}: the letters')
                else:
                    check(mine == pcm, f'BNTX: {name} byte-identical to the PC mod')
            check(bytes(patched) == bntx, 'BNTX: every byte outside those eight textures identical to the game\'s')
        else:
            check(compare_sarc(m, new, old, m.FONT), f'{path}: frame and all members but the font byte-identical to the game\'s')
            pm = m.sarc_members(pc); nm = m.sarc_members(new); om = m.sarc_members(old)
            blob, oblob, pblob = new[slice(*nm[m.FONT])], old[slice(*om[m.FONT])], pc[slice(*pm[m.FONT])]
            otf, key = m.bfotf_decode(blob); ootf, okey = m.bfotf_decode(oblob); potf, _ = m.bfotf_decode(pblob)
            check(key == okey and m.bfotf_encode(otf, key) == blob, f'{path}: the key kept, the font encodes back to the same bytes')
            fonts[otf] = (ootf, potf)
            if not game_fonts:
                game_fonts = [ootf] + [m.bfotf_decode(old[slice(*om[name])])[0] for name in m.TEXT_FONTS]
    check(len(fonts) == 1, 'the four archives hold one and the same font')
    otf, (original, pc_otf) = next(iter(fonts.items()))
    reference, count = m.redraw(original, [noto], 'playstation')
    print(f'      the PC tool with Noto letters: {count} icons')
    compare_font(m, otf, original, reference, pc_otf)
    compare_asset(m, args.asset or root/'port/android/deps/out/icons/playstation.bin', game_fonts, game_masks)
    print('PASS' if not problems else f'{len(problems)} FAILED')
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
