#!/usr/bin/env python3
"""Cases for comparing tables_cli with prepare_runtime.py: each a small tree the Python tool can run in, holding three made-up
modules in the shape it reads, or the real ones with one damaged.

    gen_synth.py <prepare_runtime.py> <d2r dir> made-up <out dir> <count> <seed>
    gen_synth.py <prepare_runtime.py> <d2r dir> mutate <rewritten dir> <exefs dir> <out dir> <count> <seed>

Every field a module can vary in is drawn from the seed, and a share of the cases carry one aimed fault. The tool's check of
the real main module's hash is taken out of each tree's copy: it would refuse everything here. Nothing from the game is in a
made-up case, apart from the one symbol name the tool looks for.
"""
import hashlib, json, random, shutil, struct, sys
from pathlib import Path

PROBE = '_ZN2nn2os13GetSystemTickEv'
NAMES = ('main', 'subsdk0', 'sdk')
HASH_CHECK = "    assert report['module_file_sha256']['extracted/v143/exefs/main'] == "


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def adrp(pc, target, reg):
    pages = ((target & ~4095) - (pc & ~4095)) >> 12
    pages &= (1 << 21) - 1
    return 0x90000000 | (pages & 3) << 29 | (pages >> 2) << 5 | reg


def ldr64(rt, rn, off):
    return 0xF9400000 | (off // 8) << 10 | rn << 5 | rt


def ldr32(rt, rn, off):
    return 0xB9400000 | (off // 4) << 10 | rn << 5 | rt


def symbol_name(r, i):
    kind = r.randrange(12)
    if kind == 0:
        return 'café_' + str(i)     # two-byte UTF-8
    if kind == 1:
        return '漢字' + str(i)   # three bytes
    if kind == 2:
        return '\U0001F600' + str(i)     # four bytes
    return ''.join(r.choice('abcdefgh_ZN0123') for _ in range(r.randrange(1, 40))) + str(i)


class Module:
    """One module's image and side files, with where its parts went for aiming faults."""

    def __init__(self, r, name):
        self.name = name
        main = name == 'main'
        nsym = 1 + r.randrange(40)
        names = [''] + [symbol_name(r, i) for i in range(1, nsym)]
        if main:
            names.append(PROBE)
            if r.randrange(3) == 0:
                names.append(PROBE)     # two symbols of that name: the first slot found wins
        nsym = len(names)
        self.text_end = align((0x12000 if main else 0x3000) + r.randrange(0x2000), 4)
        ro = align(self.text_end, 0x1000)

        # read-only part: names, symbols, tables
        strtab = ro + 0x40
        blob, offsets = bytearray(), []
        for n in names:
            offsets.append(len(blob))
            blob += n.encode() + b'\0'
        symtab = align(strtab + len(blob), 8)
        self.use_hash = main or r.randrange(2) == 0
        hash_at = symtab + 24 * nsym
        rela_at = align(hash_at + 16, 8)
        nrela = r.randrange(0, 24)
        nplt = 2 + r.randrange(8)
        jmprel = rela_at + 24 * nrela
        relr_at = jmprel + 24 * nplt
        relr_words = 1 + r.randrange(12)
        use_relr = main or r.randrange(4) > 0
        ro_end = align(relr_at + 8 * relr_words + 0x20, 0x1000)

        # data: dynamic table first, then what relocations touch
        data = ro_end
        dynamic_room = 0x200
        slots = data + dynamic_room
        data_size = align(0x2000 + r.randrange(0x3000), 8)
        end = data + data_size + r.randrange(0x2000)      # bss after the file data
        key = align(end, 0x4000)
        patch = align(end + 0x4000, 0x10000)
        patch_size = r.choice((0, 0x40, 0x400)) if name == 'subsdk0' else 0x400
        img = bytearray(patch + patch_size)

        mod0 = 0x20
        struct.pack_into('<I', img, 4, mod0)
        img[mod0:mod0 + 4] = b'MOD0'
        struct.pack_into('<i', img, mod0 + 4, data - mod0)
        img[strtab:strtab + len(blob)] = blob

        for i in range(nsym):
            struct.pack_into('<IBBHQQ', img, symtab + 24 * i, offsets[i], r.randrange(256), r.randrange(256),
                             r.choice((0, 0, 1, 0xFFF1)), r.getrandbits(r.choice((8, 32, 64))), r.getrandbits(r.choice((8, 32, 64))))
        if self.use_hash:
            struct.pack_into('<II', img, hash_at, 1, nsym)

        # relocated places: every word distinct
        free = list(range(slots, data + data_size - 8, 8))
        r.shuffle(free)
        take = free.pop
        probe_index = names.index(PROBE) if main else None

        for i in range(nrela):
            kind = r.choice((257, 1025, 1026, 1027))
            addend = r.randrange(key) if kind == 1027 else r.getrandbits(32) - (1 << 31)
            symbol = r.randrange(nsym)
            # RELA needs no alignment, but a stub can only address an aligned slot
            where = take() + (0 if main and names[symbol] == PROBE else r.choice((0, 0, 0, 1, 4)))
            struct.pack_into('<QQq', img, rela_at + 24 * i, where, kind | symbol << 32, addend)
        for i in range(nplt):
            where = take()
            symbol = probe_index if main and i == 0 else r.randrange(nsym)
            struct.pack_into('<QQq', img, jmprel + 24 * i, where, 1026 | symbol << 32, 0)

        # RELR: an address, then bitmaps of the words after it
        words = []
        if use_relr:
            start = (slots + 0x800 + data_size) // 2 // 8 * 8
            start = max(start, slots + 0x400)
            start = min(start, data + data_size - 0x400)
            used = set()
            cursor = None
            for _ in range(relr_words):
                if cursor is None or r.randrange(3) == 0:
                    addr = start + 8 * r.randrange(4) if cursor is None else align(cursor + 8, 8)
                    if addr + 512 >= data + data_size:
                        break
                    words.append(addr)
                    used.add(addr)
                    cursor = addr + 8
                else:
                    bits = r.getrandbits(63) << 1 | 1
                    if cursor + 63 * 8 >= data + data_size:
                        break
                    words.append(bits)
                    for bit in range(1, 64):
                        if bits >> bit & 1:
                            used.add(cursor + (bit - 1) * 8)
                    cursor += 63 * 8
            # keep RELA off RELR's words: the tool refuses a shared address
            for off, count in ((rela_at, nrela), (jmprel, nplt)):
                for i in range(count):
                    where = struct.unpack_from('<Q', img, off + 24 * i)[0]
                    while (where & ~7) in used or ((where + 7) & ~7) in used:
                        where = take()
                    struct.pack_into('<Q', img, off + 24 * i, where)
            for w, a in enumerate(words):
                struct.pack_into('<Q', img, relr_at + 8 * w, a)
            for a in used:
                struct.pack_into('<Q', img, a, r.randrange(key))
        self.relr_words = len(words)

        # DT_INIT: adrp x19 at +16, ldr w8, [x19, #n] at +20
        init = align(0x100 + r.randrange(0x400), 4)
        flag = align(take(), 8)
        page = flag & ~4095
        struct.pack_into('<II', img, init + 16, adrp(init + 16, page, 19), ldr32(8, 19, (flag - page) // 4 * 4))
        flag_low = flag - page
        assert flag_low % 4 == 0

        if main:   # the stub of the first jump slot of that name, somewhere in the last 64 KB of the text, and another beside it
            self.probe_slot = None
            for off, count in ((rela_at, nrela), (jmprel, nplt)):
                for i in range(count):
                    where, info, _ = struct.unpack_from('<QQq', img, off + 24 * i)
                    if self.probe_slot is None and info & 0xFFFFFFFF == 1026 and names[info >> 32] == PROBE:
                        self.probe_slot = where
            at = self.text_end - 0x40 - 16 * r.randrange(32)
            self.plt = at
            page = self.probe_slot & ~4095
            struct.pack_into('<IIII', img, at, adrp(at, page, 16), ldr64(17, 16, self.probe_slot - page), 0x91000210, 0xD61F0220)
            other = at - 16
            page2 = (self.probe_slot + 8) & ~4095
            struct.pack_into('<IIII', img, other, adrp(other, page2, 16), ldr64(17, 16, self.probe_slot + 8 - page2), 0x91000210, 0xD61F0220)

        tags = [(5, strtab), (6, symtab), (7, rela_at), (8, 24 * nrela), (9, 24), (10, len(blob)), (11, 24), (12, init), (20, 7), (23, jmprel), (2, 24 * nplt)]
        if self.use_hash:
            tags.append((4, hash_at))
        if use_relr:
            tags += [(35, 8 * len(words)), (36, relr_at), (37, 8)]
        if r.randrange(2):
            tags += [(25, data + 0x100), (27, 8 * r.randrange(16))]
        tags += [(r.choice((1, 3, 13, 14, 26, 28, 30, 0x6FFFFEF5, -2)), r.getrandbits(32)) for _ in range(r.randrange(4))]
        r.shuffle(tags)
        if r.randrange(4) == 0:   # a tag given twice: the last one counts
            tags.insert(0, (10, 1))
        for i, (t, v) in enumerate(tags):
            struct.pack_into('<qQ', img, data + 16 * i, t, v)
        assert 16 * (len(tags) + 1) <= dynamic_room

        self.image = img
        self.layout = dict(PATCH_OFF=patch, KEY_PAGE_OFF=key, KEY_PAGE_SIZE=0x4000, TEXT_END=self.text_end, RO_OFF=ro, DATA_OFF=data)
        self.places = dict(mod0=mod0, dynamic=data, ntags=len(tags), strtab=strtab, strsz=len(blob), symtab=symtab, nsym=nsym, rela=rela_at, nrela=nrela,
                           jmprel=jmprel, nplt=nplt, relr=relr_at, nrelr=len(words), init=init, hash=hash_at if self.use_hash else None)

        nso = bytearray(0x100)
        nso[0:4] = b'NSO0'
        struct.pack_into('<II', nso, 0x98, symtab - ro, 24 * nsym)
        self.nso = nso

        gates = []
        if name == 'sdk':
            rets = r.sample(range(patch, patch + 0x400, 4), r.randrange(0, 24))
            for ret in rets:
                gates.append((ret, r.choice(('svc', 'cntpct', 'cntvct')), r.randrange(31), r.randrange(self.text_end)))
        self.gates = gates


def fault(r, modules):
    """One aimed fault, the kinds of thing a damaged or foreign module would show. Returns its name."""
    m = r.choice(modules)
    p, img = m.places, m.image
    w64 = lambda at, v: struct.pack_into('<Q', img, at, v & (2**64 - 1))
    w32 = lambda at, v: struct.pack_into('<I', img, at, v & (2**32 - 1))
    odd = lambda: r.choice((0, 1, 4, 8, 24, 0xFFFFFFFF, len(img), len(img) - 1, len(img) - 8, len(img) + 8, 2**63, 2**64 - 8, r.getrandbits(32), r.getrandbits(64)))
    kind = r.randrange(26)
    if kind == 0: img[p['mod0']] ^= 1
    elif kind == 1: w32(4, odd())
    elif kind == 2: w32(p['mod0'] + 4, odd())
    elif kind == 3: w64(p['dynamic'] + 16 * r.randrange(p['ntags']) + 8, odd())
    elif kind == 4: w64(p['dynamic'] + 16 * r.randrange(p['ntags']), r.choice((0, 0, 4, 9, 11, 20, 35, 36, 37)))
    elif kind == 5:   # no terminator in the dynamic table's room
        for i in range(0x1000):
            if p['dynamic'] + 16 * i + 16 > len(img): break
            w64(p['dynamic'] + 16 * i, 1)
    elif kind == 6: w32(p['symtab'] + 24 * r.randrange(p['nsym']), r.choice((p['strsz'], p['strsz'] - 1, p['strsz'] + 10, 2**32 - 1, r.getrandbits(32))))
    elif kind == 7:   # names without their last terminator
        img[p['strtab'] + p['strsz'] - 1] = 0x41
    elif kind == 8:   # bad UTF-8
        img[p['strtab'] + r.randrange(p['strsz'])] = r.choice((0x80, 0xC0, 0xC1, 0xE0, 0xED, 0xF0, 0xF4, 0xF5, 0xFF))
    elif kind == 9 and p['nrela']: w64(p['rela'] + 24 * r.randrange(p['nrela']), odd())
    elif kind == 10 and p['nrela']: w64(p['rela'] + 24 * r.randrange(p['nrela']) + 8, r.choice((0, 1024, 1028, 1027 | 2**40, 257 | (p['nsym'] << 32), r.getrandbits(64))))
    elif kind == 11 and p['nrela']:   # a relative addend out of range
        e = p['rela'] + 24 * r.randrange(p['nrela'])
        w64(e + 8, 1027); w64(e + 16, r.choice((m.layout['KEY_PAGE_OFF'], m.layout['KEY_PAGE_OFF'] - 1, 2**64 - 1)))
    elif kind == 12:  # one address relocated twice
        a = p['jmprel']
        b = p['jmprel'] + 24 * (p['nplt'] - 1)
        struct.pack_into('<Q', img, a, struct.unpack_from('<Q', img, b)[0])
    elif kind == 13 and p['nrelr']: w64(p['relr'] + 8 * r.randrange(p['nrelr']), odd())
    elif kind == 14 and p['nrelr']: w64(p['relr'], r.getrandbits(62) << 1 | 1)                 # a bitmap first
    elif kind == 15 and p['nrelr'] > 1: w64(p['relr'] + 8, struct.unpack_from('<Q', img, p['relr'])[0])   # the same address twice
    elif kind == 16: w32(p['init'] + 16, r.choice((0, r.getrandbits(32), 0x90000013, 0x90FFFFF3)))
    elif kind == 17: w32(p['init'] + 20, r.choice((0, r.getrandbits(32), 0xB9400268, 0xB97FFE68)))
    elif kind == 18 and m.name == 'main':    # the stub broken
        w32(m.plt + 12, 0)
    elif kind == 19 and m.name == 'main':    # a second stub of the slot
        at = m.plt - 32
        page = m.probe_slot & ~4095
        struct.pack_into('<IIII', img, at, adrp(at, page, 16), ldr64(17, 16, m.probe_slot - page), 0, 0xD61F0220)
    elif kind == 20 and p['hash'] is not None: w32(p['hash'] + 4, odd())
    elif kind == 21 and m.gates:             # two gates on one return address
        m.gates[0] = (m.gates[-1][0],) + m.gates[0][1:]
    elif kind == 22:                         # the header's dynsym extent
        struct.pack_into('<II', m.nso, 0x98, r.getrandbits(16), r.choice((0, 1, 24, 2**31, r.getrandbits(32))))
    elif kind == 23: m.nso = m.nso[:r.randrange(0xA0)]
    elif kind == 24:
        for _ in range(r.randrange(1, 40)):
            img[r.randrange(p['symtab'], p['relr'] + 8)] = r.getrandbits(8)
    else: return fault(r, modules)
    return f'{m.name} {kind}'


def write_tree(out, tool, d2r):
    (out / 'scripts').mkdir(parents=True)
    (out / 'third_party').mkdir()
    (out / 'analysis/v143-port').mkdir(parents=True)
    (out / 'extracted/v143/elf').mkdir(parents=True)
    (out / 'extracted/v143/exefs').mkdir()
    (out / 'rewritten/v143').mkdir(parents=True)
    (out / 'third_party/d2r').symlink_to(d2r)
    lines = tool.read_text().splitlines(True)
    edited = ['    pass\n' if line.startswith(HASH_CHECK) else line for line in lines]
    assert edited != lines
    (out / 'scripts/prepare_runtime.py').write_text(''.join(edited))
    for n in NAMES:
        (out / f'extracted/v143/elf/{n}-inventory.json').write_text('{}')


def finish_tree(out):
    digests = {}
    for n in NAMES:
        digests[f'rewritten/v143/{n}.patched.bin'] = hashlib.sha256((out / f'rewritten/v143/{n}.patched.bin').read_bytes()).hexdigest()
    (out / 'analysis/v143-port/phase12-validation.json').write_text(json.dumps(dict(status='pass', rom_version='v143', module_file_sha256=digests)))
    (out / 'rewritten/v143/build_src.sha256').write_text('0' * 64 + '  patch_nso.py\n')


def made_up(tool, d2r, out, count, seed):
    for i in range(count):
        r = random.Random(seed * 1000003 + i)
        case = out / f'case_{i:05d}'
        write_tree(case, tool, d2r)
        modules = [Module(r, n) for n in NAMES]
        note = fault(r, modules) if i % 2 else 'none'
        for m in modules:
            (case / f'rewritten/v143/{m.name}.patched.bin').write_bytes(m.image)
            (case / f'extracted/v143/exefs/{m.name}').write_bytes(m.nso)
            with open(case / f'rewritten/v143/{m.name}.fixups.tsv', 'w') as f:
                for k, v in m.layout.items():
                    f.write(f'# {k}\t{v:#x}\n')
            with open(case / f'rewritten/v143/{m.name}.gates.tsv', 'w') as f:
                for ret, kind, arg, original in m.gates:
                    f.write(f'{ret:#x}\t{kind}\t{arg}\t{original:#x}\n')
        finish_tree(case)
        (case / 'fault.txt').write_text(note + '\n')


def real_places(image):
    mod0 = struct.unpack_from('<I', image, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags, n = {}, 0
    while True:
        t, v = struct.unpack_from('<qQ', image, dyn + 16 * n)
        if t == 0: break
        tags[t] = v
        n += 1
    return mod0, dyn, n, tags


def mutate(tool, d2r, rewritten, exefs, out, count, seed):
    """The real modules with a fault in one: links for what stays the same, a copy for the one that changes."""
    images = {n: (rewritten / f'{n}.patched.bin').read_bytes() for n in NAMES}
    for i in range(count):
        r = random.Random(seed * 1000003 + i)
        case = out / f'case_{i:05d}'
        write_tree(case, tool, d2r)
        name = r.choice(NAMES)
        image = bytearray(images[name])
        _, dyn, n, tags = real_places(image)
        odd = lambda: r.choice((0, 1, 8, 24, len(image), len(image) - 8, 2**63, 2**64 - 8, r.getrandbits(32), r.getrandbits(64)))
        kind = r.randrange(9)
        if kind == 0: struct.pack_into('<Q', image, dyn + 16 * r.randrange(n) + 8, odd())
        elif kind == 1: struct.pack_into('<q', image, dyn + 16 * r.randrange(n), r.choice((0, 4, 9, 11, 20, 35, 37)))
        elif kind == 2: struct.pack_into('<I', image, tags[6] + 24 * r.randrange(1, 300), r.getrandbits(32))
        elif kind == 3: image[tags[5] + r.randrange(tags[10])] = r.choice((0x80, 0xC0, 0xFF))
        elif kind == 4: struct.pack_into('<Q', image, tags[7] + 24 * r.randrange(tags[8] // 24 or 1), odd())
        elif kind == 5: struct.pack_into('<Q', image, tags[23] + 24 * r.randrange(tags[2] // 24) + 8, r.choice((1024, 1027, r.getrandbits(64))))
        elif kind == 6: struct.pack_into('<Q', image, tags[36] + 8 * r.randrange(tags[35] // 8), odd())
        elif kind == 7: struct.pack_into('<I', image, tags[12] + 16 + 4 * r.randrange(2), r.getrandbits(32))
        else: image[r.randrange(tags[5], tags[5] + tags[10])] = 0x41
        for n2 in NAMES:
            target = case / f'rewritten/v143/{n2}.patched.bin'
            if n2 == name: target.write_bytes(image)
            else: target.symlink_to(rewritten / f'{n2}.patched.bin')
            for side in ('fixups.tsv', 'gates.tsv'):
                shutil.copy(rewritten / f'{n2}.{side}', case / f'rewritten/v143/{n2}.{side}')
            (case / f'extracted/v143/exefs/{n2}').write_bytes((exefs / n2).read_bytes()[:256])
        finish_tree(case)
        (case / 'fault.txt').write_text(f'{name} {kind}\n')


def main():
    tool, d2r, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    if mode == 'made-up':
        made_up(tool, d2r, Path(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
    else:
        mutate(tool, d2r, Path(sys.argv[4]), Path(sys.argv[5]), Path(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8]))


if __name__ == '__main__':
    main()
