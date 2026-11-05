#!/usr/bin/env python3
"""Byte sequences and what Python's decoder says of them, for test_units: every first byte, with the following bytes taken
around every boundary of the UTF-8 rules. Lines of "<hex> <1 when valid>"."""
import sys

SECOND = (0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF)
REST = (0x41, 0x7F, 0x80, 0xBF, 0xC0)


def valid(b):
    try:
        b.decode()
        return 1
    except UnicodeDecodeError:
        return 0


out = []
for a in range(256):
    out.append(bytes((a,)))
    for b in SECOND:
        out.append(bytes((a, b)))
        for c in REST:
            out.append(bytes((a, b, c)))
            for d in REST:
                out.append(bytes((a, b, c, d)))
                # after an ASCII byte
                out.append(bytes((0x41, a, b, c, d)))
for s in out:
    sys.stdout.write(f'{s.hex()} {valid(s)}\n')
