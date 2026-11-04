#!/usr/bin/env python3
"""The 1.0.0 side of the startup screen's mod merge (Mods.java): what 1.0.0 had where 1.4.3 differs, so a mod made for
an older version can be merged into 1.4.3 three ways on the device (the same rules as MacNativeGames' port_mod.py).
Game content: written to the work folder and pushed by push_data.sh, never committed.

    port/android/tools/make_mod_base.py [--extracted work/totk/extracted] -> <extracted>/v143/mod-base.bin

Format (little endian). A key is the first 8 bytes of SHA-256 of the RomFS path, a hash the first 8 bytes of SHA-256 of
the content.
    "TKMODB01", u32 files, u32 packs, u64 files offset, u64 packs offset
    files: {u64 key, u64 1.0.0 hash} for every file 1.0.0 and 1.4.3 both have and that differs, sorted by key
    packs: {u64 key, u64 blob offset, u32 blob size, u32 0} for each of those that is a .pack.zs, sorted by key
    blob (zstd, no dictionary): u32 members, then per member u16 name size, name, u64 hash, u32 size, and the member's
    bytes when it is BYML and 1.4.3's differs (size 0 otherwise)
"""
import argparse, hashlib, struct, sys
from multiprocessing import Pool
from pathlib import Path
try:
    import compression.zstd as zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd): run it with python3.14')

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'MacNativeGames/games/totk/scripts'))
import port_mod as P   # noqa: E402

X = None


def init(extracted):
    global X, old, new, dicts
    X = extracted; old, new = P.Romfs(X/'v0/romfs'), P.Romfs(X/'v143/romfs'); dicts = {}
    for _, _, _, body, _ in P.sarc_read(zstd.decompress(new.read('Pack/ZsDic.pack.zs')))[0]:
        d = zstd.ZstdDict(body); dicts[d.dict_id] = d


def h8(b): return hashlib.sha256(b).digest()[:8]


def unzs(raw):
    info = zstd.get_frame_info(raw)
    return zstd.decompress(raw, zstd_dict=dicts[info.dictionary_id]) if info.dictionary_id else zstd.decompress(raw)


def work(path):
    """(key, 1.0.0 hash, pack blob or None) when the file differs between 1.0.0 and 1.4.3, else None."""
    a, b = old.read(path), new.read(path)
    if b is None or a == b: return None
    blob = None
    if path.endswith('.pack.zs'):
        mine = P.sarc_read(unzs(a))[0]; theirs = {m[0]: m[3] for m in P.sarc_read(unzs(b))[0]}
        out = bytearray(struct.pack('<I', len(mine)))
        for name, _, _, body, _ in mine:
            keep = body[:2] == b'YB' and theirs.get(name) != body
            n = name.encode(); out += struct.pack('<H', len(n)) + n + h8(body) + struct.pack('<I', len(body) if keep else 0) + (body if keep else b'')
        blob = zstd.compress(bytes(out), level=19)
    return h8(path.encode()), h8(a), blob


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--extracted', type=Path, default=ROOT/'work/totk/extracted')
    a = p.parse_args()
    init(a.extracted)
    paths = sorted(set(old.files) & set(new.files))
    with Pool(16, initializer=init, initargs=(a.extracted,)) as pool:
        rows = [r for r in pool.imap_unordered(work, paths, chunksize=256) if r]
    rows.sort()
    files = b''.join(k + v for k, v, _ in rows)
    packs, blobs = [], bytearray()
    head = 8 + 4 + 4 + 8 + 8
    packs_off = head + len(files); blob_base = packs_off + 24*sum(1 for r in rows if r[2] is not None)
    for k, _, blob in rows:
        if blob is None: continue
        packs.append(k + struct.pack('<QII', blob_base + len(blobs), len(blob), 0)); blobs += blob
    out = a.extracted/'v143/mod-base.bin'
    out.write_bytes(b'TKMODB01' + struct.pack('<IIQQ', len(rows), len(packs), head, packs_off) + files + b''.join(packs) + blobs)
    print(f'{out}: {len(rows)} files differ from 1.0.0, {len(packs)} of them packs; {out.stat().st_size/1e6:.1f} MB')


if __name__ == '__main__':
    main()
