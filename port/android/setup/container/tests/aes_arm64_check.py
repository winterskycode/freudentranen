#!/usr/bin/env python3
"""The ARMv8 AES code without a device: aes_hw.c built for arm64 runs in Unicorn and must agree with OpenSSL (the cryptography
package) on block encryption, decryption and CTR at every tail length. Made-up keys and data.

    aes_arm64_check.py <linked arm64 ELF of aes_hw.c>
"""
import os, struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import unicorn
from unicorn import arm64_const as A

SBOX = None


def rotate(v, n):
    return ((v << n) | (v >> (8 - n))) & 255


def sbox():
    global SBOX
    if SBOX is None:
        # the S-box from the definition: inverse in GF(2^8), then the affine map
        s = [0] * 256
        p = q = 1
        while True:
            p = p ^ ((p << 1) & 255) ^ (0x1B if p & 0x80 else 0)
            q ^= (q << 1) & 255
            q ^= (q << 2) & 255
            q ^= (q << 4) & 255
            if q & 0x80:
                q ^= 0x09
            s[p] = q ^ rotate(q, 1) ^ rotate(q, 2) ^ rotate(q, 3) ^ rotate(q, 4) ^ 0x63
            if p == 1:
                break
        s[0] = 0x63
        SBOX = s
    return SBOX


def expand(key):
    s = sbox()
    w = bytearray(key)
    rcon = 1
    for i in range(16, 176, 4):
        t = bytearray(w[i - 4:i])
        if i % 16 == 0:
            t = bytearray((s[t[1]] ^ rcon, s[t[2]], s[t[3]], s[t[0]]))
            rcon = ((rcon << 1) ^ (0x1B if rcon & 0x80 else 0)) & 255
        w += bytes(w[i + k - 16] ^ t[k] for k in range(4))
    return bytes(w)


def symbols(elf):
    (shoff,) = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', elf, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for sh in sections:
        # SHT_SYMTAB
        if sh[1] != 2:
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', elf, off)
            end = elf.index(b'\0', strtab[4] + name)
            out[elf[strtab[4] + name:end].decode()] = value
    return out


class Arm:
    STACK, DATA, RETURN = 0x7000000, 0x8000000, 0x6000000

    def __init__(self, elf):
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        (phoff,) = struct.unpack_from('<Q', elf, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
        for i in range(phnum):
            kind, _, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
            if kind != 1:
                continue
            start, end = vaddr & ~0xFFF, (vaddr + memsz + 0xFFF) & ~0xFFF
            self.uc.mem_map(start, end - start)
            self.uc.mem_write(vaddr, elf[off:off + filesz])
        for base, size in ((self.STACK, 0x10000), (self.DATA, 0x100000), (self.RETURN, 0x1000)):
            self.uc.mem_map(base, size)
        # the FP and SIMD unit on
        self.uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.sym = symbols(elf)
        self.executed = 0

    def call(self, name, *args):
        for reg, v in zip((A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X1, A.UC_ARM64_REG_X2, A.UC_ARM64_REG_X3, A.UC_ARM64_REG_X4), args):
            self.uc.reg_write(reg, v)
        self.uc.reg_write(A.UC_ARM64_REG_SP, self.STACK + 0xF000)
        self.uc.reg_write(A.UC_ARM64_REG_LR, self.RETURN)
        self.uc.emu_start(self.sym[name], self.RETURN)

    def put(self, at, data): self.uc.mem_write(self.DATA + at, bytes(data))
    def get(self, at, n): return bytes(self.uc.mem_read(self.DATA + at, n))


def main():
    with open(sys.argv[1], 'rb') as f:
        arm = Arm(f.read())
    D = Arm.DATA
    RK, DK, IN, OUT, HIGH, BUF = 0, 0x200, 0x400, 0x500, 0x600, 0x1000
    bad = 0
    for round_ in range(12):
        key = os.urandom(16)
        ecb = Cipher(algorithms.AES(key), modes.ECB())
        arm.put(RK, expand(key))
        arm.call('tokc_aes_hw_make_dk', D + DK, D + RK)
        for _ in range(8):
            block = os.urandom(16)
            arm.put(IN, block)
            arm.call('tokc_aes_hw_encrypt', D + RK, D + IN, D + OUT)
            e = ecb.encryptor()
            if arm.get(OUT, 16) != e.update(block):
                bad += 1
            arm.call('tokc_aes_hw_decrypt', D + DK, D + IN, D + OUT)
            d = ecb.decryptor()
            if arm.get(OUT, 16) != d.update(block):
                bad += 1
        high = os.urandom(8)
        arm.put(HIGH, high)
        for blocks in [*range(20), 64, 257]:
            first = (0xFFFFFFFFFFFFFFF8 >> (4 * round_)) if round_ else 0
            plain = os.urandom(blocks * 16)
            arm.put(BUF, plain)
            arm.call('tokc_aes_hw_ctr', D + RK, D + HIGH, first, D + BUF, blocks)
            e = ecb.encryptor()
            want = b''
            for i in range(blocks):
                ctr = high + struct.pack('>Q', (first + i) & (2 ** 64 - 1))
                ks = e.update(ctr)
                want += bytes(a ^ b for a, b in zip(ks, plain[16 * i:16 * i + 16]))
            if arm.get(BUF, blocks * 16) != want:
                bad += 1
                print(f'CTR differs: {blocks} blocks')
    print(f'{"FAIL" if bad else "PASS"} arm64 AES in Unicorn against OpenSSL: {bad} differ')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
