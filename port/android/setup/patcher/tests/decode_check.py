#!/usr/bin/env python3
"""Does the Capstone built here decode like the one the Python rewriter imports? Compares decode_dump's text with the Python
binding's: the words the scan can pick, then every aligned word of a module's image.

    decode_check.py <decode_dump> <rewritten image> <text end, hex>
The image is a .patched.bin of the Python tool or any raw image. Run with work/venv's python (capstone).
"""
import hashlib, multiprocessing, os, struct, subprocess, sys
import capstone

md = None


def decoder():
    global md
    if md is None:
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        md.detail = True
    return md


def line(w, address):
    for i in decoder().disasm(struct.pack('<I', w), address):
        return f'{address:x} {i.mnemonic} {i.op_str}\n'
    return f'{address:x} (bad)\n'


def site_words():
    yield from (0xD4000001 | i << 5 for i in range(65536))
    for base in (0xD53BD040, 0xD51BD040, 0xD53BD060, 0xD53BE020, 0xD53BE040):
        yield from (base | n for n in range(32))


def chunk(job):
    path, start, end = job
    with open(path, 'rb') as f:
        f.seek(start)
        raw = f.read(end - start)
    return ''.join(line(w, start + 4 * k) for k, (w,) in enumerate(struct.iter_unpack('<I', raw[:len(raw) & ~3]))).encode()


def main():
    dump, image, text_end = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    python = ''.join(line(w, 0x1000) for w in site_words()).encode()
    c = subprocess.run([dump, '--sites'], capture_output=True, check=True).stdout
    bad = python.count(b'(bad)')
    print(f'site words: {len(python.splitlines())} lines, {bad} undecodable, {"SAME" if python == c else "DIFFERENT"}')
    ok = python == c and not bad
    step = 0x40000
    jobs = [(image, o, min(o + step, text_end)) for o in range(0, text_end, step)]
    h = hashlib.sha256()
    with multiprocessing.Pool(os.cpu_count()) as pool:
        for text in pool.imap(chunk, jobs):
            h.update(text)
    proc = subprocess.Popen([dump, image, '0', f'{text_end:x}'], stdout=subprocess.PIPE)
    h2 = hashlib.sha256()
    while block := proc.stdout.read(1 << 22):
        h2.update(block)
    proc.wait()
    same = h.digest() == h2.digest()
    print(f'{text_end // 4} words of text: {"SAME" if same else "DIFFERENT"}')
    sys.exit(0 if ok and same else 1)


if __name__ == '__main__':
    main()
