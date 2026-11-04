#!/usr/bin/env python3
"""Breath of the Wild: convert a Wii U save folder (big-endian, e.g. from Cemu) into the Switch layout (little-endian)
that the Android app (dev.tok.botw) reads.

The .sav files (game_data.sav per slot, caption.sav per slot, option.sav) are 32-bit words: a three-word header (version,
0xffffffff, 1), then (flag hash, value) pairs, then a 0xffffffff end word. Each word is byte-swapped, except the values of
string flags: those are four raw characters each and stay as they are. Which flags are strings comes from the game's own
flag definitions in its RomFS (Pack/Bootup.pack -> GameData/gamedata.ssarc, the *_data bgdata tables), so the RomFS
image the app uses is an input. A string hash that the tables do not define fails the conversion instead of guessing.
JPEG files (caption.jpg, album/, pict_book/) are copied as they are. The Hero's Path tracker blocks (tracker/) have a
different layout on the Switch and are left out: the game starts a new path.

A Wii U 1.5.0 save loads in Switch 1.6.0 (the game upgrades older saves). Nothing here reads or writes the device.
usage: botw_wiiu_save.py <Wii U user folder, e.g. .../user/80000001> <romfs.bin> <new output folder>
then push the output's contents to the app's files/profile/save/<account>/ (tools/README or the session notes).
"""
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'MacNativeGames/games/totk/scripts'))
from extract_romfs import index_romfs


def fail(msg):
    sys.exit(f'botw_wiiu_save: {msg}')


def yaz0(data):
    if data[:4] != b'Yaz0':
        return data
    size = struct.unpack_from('>I', data, 4)[0]
    out = bytearray(size)
    src, dst = 16, 0
    while dst < size:
        code = data[src]; src += 1
        for bit in range(7, -1, -1):
            if dst >= size:
                break
            if code >> bit & 1:
                out[dst] = data[src]; src += 1; dst += 1
            else:
                b1, b2 = data[src], data[src + 1]; src += 2
                back = ((b1 & 15) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12; src += 1
                else:
                    n += 2
                for _ in range(n):
                    out[dst] = out[dst - back]; dst += 1
    return bytes(out)


def sarc(data):
    if data[:4] != b'SARC':
        fail('not a SARC archive')
    e = '<' if data[6:8] == b'\xff\xfe' else '>'
    data_off = struct.unpack_from(e + 'I', data, 0xc)[0]
    sfat = struct.unpack_from(e + 'H', data, 4)[0]
    if data[sfat:sfat + 4] != b'SFAT':
        fail('SARC without SFAT')
    count = struct.unpack_from(e + 'H', data, sfat + 6)[0]
    names_at = sfat + 12 + 16 * count
    if data[names_at:names_at + 4] != b'SFNT':
        fail('SARC without SFNT')
    names_at += 8
    files = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from(e + '4I', data, sfat + 12 + 16 * i)
        if not attr >> 24:
            continue
        at = names_at + (attr & 0xffffff) * 4
        name = data[at:data.index(b'\0', at)].decode()
        files[name] = data[data_off + start:data_off + end]
    return files


def byml(data):
    """A BYML document (version 2+, either byte order) as Python values."""
    e = '<' if data[:2] == b'YB' else '>' if data[:2] == b'BY' else fail('not BYML')
    u32 = lambda o: struct.unpack_from(e + 'I', data, o)[0]
    keys_at, strings_at, root_at = u32(4), u32(8), u32(12)

    def head(at):
        word = u32(at)
        return (word & 0xff, word >> 8) if e == '<' else (word >> 24, word & 0xffffff)

    def table(at):
        if not at:
            return []
        n = head(at)[1]
        offs = [u32(at + 4 + 4 * i) for i in range(n + 1)]
        return [data[at + offs[i]:data.index(b'\0', at + offs[i])].decode('utf-8') for i in range(n)]
    keys, strings = table(keys_at), table(strings_at)

    def value(kind, raw):
        if kind == 0xa0: return strings[raw]
        if kind in (0xc0, 0xc1): return node(raw)
        if kind == 0xd0: return bool(raw)
        if kind == 0xd1: return struct.unpack(e + 'i', struct.pack(e + 'I', raw))[0]
        if kind == 0xd2: return struct.unpack(e + 'f', struct.pack(e + 'I', raw))[0]
        if kind == 0xd3: return raw
        if kind == 0xd4: return struct.unpack_from(e + 'q', data, raw)[0]
        if kind == 0xd5: return struct.unpack_from(e + 'Q', data, raw)[0]
        if kind == 0xd6: return struct.unpack_from(e + 'd', data, raw)[0]
        if kind == 0xff: return None
        fail(f'BYML node type {kind:#x}')

    def node(at):
        kind, n = head(at)
        if kind == 0xc0:
            types = data[at + 4:at + 4 + n]
            base = at + 4 + ((n + 3) & ~3)
            return [value(types[i], u32(base + 4 * i)) for i in range(n)]
        if kind == 0xc1:
            out = {}
            for i in range(n):
                word, raw = u32(at + 4 + 8 * i), u32(at + 8 + 8 * i)
                k, t = (word & 0xffffff, word >> 24) if e == '<' else (word >> 8, word & 0xff)
                out[keys[k]] = value(t, raw)
            return out
        fail(f'BYML container type {kind:#x}')
    return node(root_at)


def string_hashes(romfs):
    files = {f['path']: f for f in index_romfs(romfs)}
    boot = files.get('Pack/Bootup.pack') or fail('Pack/Bootup.pack is not in the RomFS')
    with open(romfs, 'rb') as f:
        f.seek(boot['offset']); pack = sarc(f.read(boot['size']))
    gamedata = sarc(yaz0(pack.get('GameData/gamedata.ssarc') or fail('GameData/gamedata.ssarc is not in Bootup.pack')))
    strings, every = set(), set()
    for blob in gamedata.values():
        for kind, flags in byml(blob).items():
            for flag in flags:
                h = flag['HashValue'] & 0xffffffff
                every.add(h)
                if 'string' in kind:
                    strings.add(h)
    if not strings:
        fail('no string flags in the game-data tables')
    return strings, every


def convert_sav(blob, strings, every, label):
    if len(blob) % 4 or len(blob) < 16:
        fail(f'{label}: size {len(blob)} is not a save file')
    words = list(struct.unpack(f'>{len(blob) // 4}I', blob))
    if words[1] != 0xffffffff or words[-1] != 0xffffffff or (len(words) - 4) % 2:
        fail(f'{label}: not a big-endian save (header {words[0]:#x} {words[1]:#x}; is it already a Switch save?)')
    out = bytearray(blob)
    struct.pack_into('<3I', out, 0, *words[:3])
    raw = unknown = 0
    for i in range(3, len(words) - 1, 2):
        h = words[i]
        struct.pack_into('<I', out, 4 * i, h)
        if h in strings:
            raw += 1   # four characters: stays
        else:
            struct.pack_into('<I', out, 4 * i + 4, words[i + 1])
            unknown += h not in every
    struct.pack_into('<I', out, len(out) - 4, words[-1])
    return bytes(out), raw, unknown


def main():
    if len(sys.argv) != 4:
        fail(__doc__.split('usage: ')[1].split('\n')[0])
    src, romfs, dst = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    if dst.exists():
        fail(f'{dst} exists; the output must be a new folder')
    strings, every = string_hashes(romfs)
    print(f'game-data tables: {len(every)} flags, {len(strings)} of them strings')
    dst.mkdir(parents=True)
    for path in sorted(src.rglob('*')):
        rel = path.relative_to(src)
        if path.is_dir() or rel.parts[0] == 'tracker':
            continue
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix == '.sav':
            blob, raw, unknown = convert_sav(path.read_bytes(), strings, every, str(rel))
            target.write_bytes(blob)
            print(f'{rel}: version {struct.unpack_from("<I", blob)[0]:#x}, {len(blob)} bytes, {raw} string words kept'
                  + (f', {unknown} values of flags the tables do not define (swapped as numbers)' if unknown else ''))
        else:
            shutil.copyfile(path, target)
    print(f'written {dst}')


if __name__ == '__main__':
    main()
