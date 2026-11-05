#!/usr/bin/env python3
"""Made-up NSO modules for comparing the two rewriters: every register shape of every rewrite block, random control flow
around exclusive loads and stores, and the .eh_frame_hdr encodings. No game code: the words come from the rules here.

    gen_synth.py <out dir> <count> [seed]                 made-up modules
    gen_synth.py --mutate <module> <out dir> <count> [seed]   a real module, stored uncompressed, with sites and exclusive
                                                              words written over random instructions (stays in <out dir>)
Needs lz4 (work/venv). The modules are read by regress.sh; they are not meant to be kept.
"""
import random, struct, sys
from pathlib import Path
import lz4.block

SVC, MRS_TPIDR, MSR_TPIDR, MRS_TPIDRRO, MRS_CNTPCT, MRS_CNTVCT = 0xD4000001, 0xD53BD040, 0xD51BD040, 0xD53BD060, 0xD53BE020, 0xD53BE040
LDX = (0x885F7C20, 0xC85FFC20, 0xC87F0820, 0x085F7C20, 0x485FFC20, 0x887F0820)
STX = (0x88027C20, 0xC802FC20, 0xC8220C20, 0x08027C20, 0x4802FC20)
# bit 31 clear, bit 21 set: not exclusive for the rule
OTHER_EXCLUSIVE_SHAPE = (0x08A07C20, 0x48E07C20, 0x08207C20, 0x08607C20)
CLREX = 0xD503305F
PLAIN = (0xD503201F, 0x91000400, 0xF9400020, 0xB9000020, 0x94000010, 0xD63F0200)
RET, BR = 0xD65F03C0, 0xD61F0200


def align(v, a): return (v + a - 1) & ~(a - 1)


def eh_hdr(base, starts, rng):
    """.eh_frame_hdr at image offset base for the function starts; encodings at random."""
    penc = rng.choice((0x1B, 0x3B, 0x03, 0x0B, 0x04, 0x13))
    cenc = rng.choice((0x03, 0x0B, 0x04))
    tenc = rng.choice((0x3B, 0x3B, 0x1B, 0x03, 0x0B, 0x04, 0x33, 0x14))

    def put(off, enc, value):
        fmt, app = enc & 0xF, enc & 0x70
        if app == 0x10: value -= off
        if app == 0x30: value -= base
        size, code = {0x3: (4, '<I'), 0xB: (4, '<i'), 0x4: (8, '<Q')}[fmt]
        return struct.pack(code, value if fmt == 0xB else value & ((1 << 8 * size) - 1))

    out = bytearray(bytes((1, penc, cenc, tenc)))
    out += put(base + len(out), penc, 0x10)
    out += put(base + len(out), cenc, len(starts))
    for s in starts:
        out += put(base + len(out), tenc, s)
        out += put(base + len(out), tenc, s + 4)
    return bytes(out)


def nso(text, ro, data, bss, compress, rng):
    text_mem, ro_mem = 0, align(len(text), 0x1000)
    data_mem = align(ro_mem + len(ro), 0x1000)
    blobs = [lz4.block.compress(bytes(b), store_size=False) if compress else bytes(b) for b in (text, ro, data)]
    head = bytearray(0x100)
    head[:4] = b'NSO0'
    struct.pack_into('<I', head, 0xC, 7 if compress else 0)
    off = 0x100
    for i, (mem, raw, blob) in enumerate(zip((text_mem, ro_mem, data_mem), (text, ro, data), blobs)):
        struct.pack_into('<III', head, 0x10 + 0x10 * i, off, mem, len(raw))
        struct.pack_into('<I', head, 0x60 + 4 * i, len(blob))
        off += len(blob)
    struct.pack_into('<I', head, 0x3C, bss)
    head[0x40:0x60] = rng.randbytes(32)
    return bytes(head) + b''.join(blobs)


def build(words, starts, rng, compress=None, tail=0, bss=None):
    """Text from the words, with the MOD0 header and .eh_frame_hdr in .rodata."""
    text = bytearray(struct.pack(f'<{len(words)}I', *words)) + rng.randbytes(tail)
    ro_mem = align(len(text), 0x1000)
    # the MOD0 offset, in the module's second word
    struct.pack_into('<I', text, 4, ro_mem)
    mod0 = bytearray(b'MOD0' + bytes(28))
    # .eh_frame_hdr, from MOD0
    struct.pack_into('<i', mod0, 16, 0x40)
    ro = bytes(mod0) + bytes(0x20) + eh_hdr(ro_mem + 0x40, starts, rng)
    data = rng.randbytes(rng.choice((0, 8, 0x100, 0x1234)))
    return nso(text, ro, data, rng.randrange(0, 0x9000) if bss is None else bss, rng.random() < .8 if compress is None else compress, rng)


def every_shape(rng):
    """All registers of all sites, the ones refused left out."""
    w = [0x14000002, 0]
    for n in range(31):
        w += [MRS_TPIDR | n, MRS_TPIDRRO | n, MSR_TPIDR | n, MRS_CNTPCT | n, MRS_CNTVCT | n]
    w += [MSR_TPIDR | 31, MRS_CNTPCT | 31, MRS_CNTVCT | 31]
    w += [SVC | i << 5 for i in (0, 1, 0x26, 0x7F, 0xFFFF)]
    return build(w + [RET], [8], rng)


def refused_register(rng):
    return build([0x14000002, 0, rng.choice((MRS_TPIDR, MRS_TPIDRRO)) | 31, RET], [8], rng)


def branch(rng, here, size):
    """A branch to a word of the text; anything further is left to the caller's random words."""
    delta = rng.randrange(-min(here, 40), min(size - here, 40) + 1)
    kind = rng.randrange(5)
    if kind == 0: return 0x14000000 | (delta & 0x3FFFFFF)
    if kind == 1: return 0x54000000 | (delta & 0x7FFFF) << 5 | rng.randrange(14)
    if kind == 2: return rng.choice((0x34000000, 0xB4000000, 0x35000000, 0xB5000000)) | (delta & 0x7FFFF) << 5 | rng.randrange(31)
    if kind == 3: return rng.choice((0x36000000, 0x37000000, 0xB6000000)) | (delta & 0x3FFF) << 5 | rng.randrange(31)
    return rng.choice((RET, BR))


def site(rng):
    n = rng.choice((0, 1, 5, 16, 17, 18, 29, 30))
    return rng.choice((SVC | rng.randrange(0x100) << 5, MRS_TPIDR | n, MSR_TPIDR | n, MRS_TPIDRRO | n, MRS_CNTPCT | n, MRS_CNTVCT | n))


def random_cfg(rng):
    size = rng.randrange(40, 900)
    site_rate, excl_rate = rng.choice(((.003, .02), (.01, .05), (.03, .1), (.0, .1), (.02, .0)))
    words = [0x14000002, 0]
    for i in range(2, size):
        r = rng.random()
        if r < site_rate: words.append(site(rng))
        elif r < site_rate + excl_rate:
            words.append(rng.choice(LDX + STX + OTHER_EXCLUSIVE_SHAPE + (CLREX,)))
        elif r < site_rate + excl_rate + .2: words.append(branch(rng, i, size))
        else: words.append(rng.choice(PLAIN))
    # a site at offset 0, reachable by a branch back
    if rng.random() < .3: words[0] = site(rng)
    starts = sorted(rng.sample(range(8, size * 4, 4), rng.randrange(0, min(30, size // 2))))
    # bad starts: odd ones, a duplicate, out of order, past the text
    if rng.random() < .3: starts += [rng.randrange(8, size * 4) for _ in range(rng.randrange(1, 6))]
    if rng.random() < .2 and starts: starts.append(rng.choice(starts))
    if rng.random() < .3: rng.shuffle(starts)
    if rng.random() < .1: starts.append(size * 4 + 0x40)
    return build(words, starts, rng, tail=rng.choice((0, 0, 1, 2, 3)))


def write_all(out, count, seed):
    rng = random.Random(seed)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'case_shapes').write_bytes(every_shape(rng))
    (out / 'case_refused').write_bytes(refused_register(rng))
    for i in range(count):
        (out / f'case_{i:05}').write_bytes(random_cfg(rng))


def mutate(module, out, count, seed):
    """The real module, stored flat, with words overwritten. Game bytes: <out> must be a scratch folder."""
    d = Path(module).read_bytes()
    flags = struct.unpack_from('<I', d, 0xC)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    stored = struct.unpack_from('<III', d, 0x60)
    raws = []
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + stored[i]]
        raws.append(lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw)
    rng = random.Random(seed)
    head = bytearray(d[:0x100])
    struct.pack_into('<I', head, 0xC, 0)
    off = 0x100
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<I', head, 0x10 + 0x10 * i, off)
        struct.pack_into('<I', head, 0x60 + 4 * i, sz)
        off += sz
    out.mkdir(parents=True, exist_ok=True)
    ldx_at = [o for o in range(0, segs[0][2] - 4, 4) if (struct.unpack_from('<I', raws[0], o)[0] & 0x3F800000) == 0x08000000]
    for i in range(count):
        text = bytearray(raws[0])
        for _ in range(rng.choice((1, 5, 50, 500))):
            near = rng.choice(ldx_at) + 4 * rng.randrange(1, 12) if ldx_at and rng.random() < .5 else None
            at = near if near is not None and near < len(text) - 4 else rng.randrange(8, len(text) - 4) & ~3
            struct.pack_into('<I', text, at, rng.choice((site(rng), site(rng), rng.choice(LDX), rng.choice(STX), CLREX)))
        (out / f'mutant_{i:05}').write_bytes(bytes(head) + bytes(text) + raws[1] + raws[2])


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--mutate']:
        mutate(a[1], Path(a[2]), int(a[3]), int(a[4]) if len(a) > 4 else 1)
    else:
        write_all(Path(a[0]), int(a[1]), int(a[2]) if len(a) > 2 else 1)
