#!/usr/bin/env python3
"""The ARMv8 SHA-256 code without a device: sha256_hw.c built for arm64 runs in Unicorn and must agree with hashlib's compression
of the same blocks, for chained calls of many lengths. Made-up data.

    sha_arm64_check.py <linked arm64 ELF of sha256_hw.c>
"""
import hashlib, os, struct, sys
import unicorn
from unicorn import arm64_const as A

STACK, DATA, RETURN = 0x7000000, 0x8000000, 0x6000000
IV = (0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19)


def symbols(elf):
    (shoff,) = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', elf, 0x3A)
    sections = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for sh in sections:
        if sh[1] != 2:   # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', elf, off)
            end = elf.index(b'\0', strtab[4] + name)
            out[elf[strtab[4] + name:end].decode()] = value
    return out


def main():
    with open(sys.argv[1], 'rb') as f:
        elf = f.read()
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
    (phoff,) = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    for i in range(phnum):
        kind, _, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind != 1:
            continue
        start, end = vaddr & ~0xFFF, (vaddr + memsz + 0xFFF) & ~0xFFF
        uc.mem_map(start, end - start)
        uc.mem_write(vaddr, elf[off:off + filesz])
    for base, size in ((STACK, 0x10000), (DATA, 0x100000), (RETURN, 0x1000)):
        uc.mem_map(base, size)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # the FP and SIMD unit on
    entry = symbols(elf)['tokt_sha256_hw_blocks']

    def blocks(state_at, data_at, n):
        for reg, v in zip((A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X1, A.UC_ARM64_REG_X2), (state_at, data_at, n)):
            uc.reg_write(reg, v)
        uc.reg_write(A.UC_ARM64_REG_SP, STACK + 0xF000)
        uc.reg_write(A.UC_ARM64_REG_LR, RETURN)
        uc.emu_start(entry, RETURN)

    bad = runs = 0
    for n in [*range(0, 40), 64, 127, 255, 1000]:
        data = os.urandom(n)
        # the padded message, hashed in two chained calls to cover the state going in and out
        padded = data + b'\x80' + bytes(-(n + 9) % 64) + struct.pack('>Q', n * 8)
        total = len(padded) // 64
        uc.mem_write(DATA, struct.pack('<8I', *IV))
        uc.mem_write(DATA + 0x100, padded)
        first = total // 2
        blocks(DATA, DATA + 0x100, first)
        blocks(DATA, DATA + 0x100 + 64 * first, total - first)
        got = struct.pack('>8I', *struct.unpack('<8I', bytes(uc.mem_read(DATA, 32))))
        runs += 1
        if got != hashlib.sha256(data).digest():
            bad += 1
            print(f'differs at {n} bytes')
    print(f'{"FAIL" if bad else "PASS"} arm64 SHA-256 in Unicorn against hashlib: {runs} messages, {bad} differ')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
