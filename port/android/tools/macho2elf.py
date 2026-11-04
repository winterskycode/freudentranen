#!/usr/bin/env python3
"""Convert MacNativeGames' Mach-O arm64 assembly (.S) to ELF for the NDK assembler.

    macho2elf.py <in.S> <out.S>

Mach-O names C symbols with a leading underscore and addresses pages with @PAGE / @PAGEOFF; ELF uses the bare name and
:lo12:. Only the constructs these files use are handled; anything else Mach-O-specific stops the conversion.
"""
import re, sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
for bad in ('.private_extern', '.section __', '.subsections_via_symbols', '.zerofill', '@GOT', '@TLVP'):
    if bad in text:
        sys.exit(f'{src}: unsupported Mach-O construct {bad}')
text = re.sub(r'\b_([A-Za-z_]\w*)@PAGEOFF\b', r':lo12:\1', text)
text = re.sub(r'\b_([A-Za-z_]\w*)@PAGE\b', r'\1', text)
# remaining Mach-O C symbol names (labels, .globl, b/bl targets, .quad data); assembler-local labels start with L or digits
lines = []
for line in text.splitlines():
    if line.lstrip().startswith('#'):   # preprocessor lines keep their names (__ANDROID__ etc.)
        lines.append(line); continue
    code, sep, comment = line.partition('//')
    code = re.sub(r'(?<![\w.$])_([A-Za-z_]\w*)', r'\1', code)
    lines.append(code + sep + comment)
text = '\n'.join(lines) + '\n'
text = text.replace('.cstring', '.section .rodata.str1.1,"aMS",@progbits,1')
# Hidden visibility: inside libtotk.so these symbols must bind locally. A call to a preemptible symbol goes through a
# PLT stub, which clobbers x16/x17, and the host-stack switch takes its target and index in exactly those registers.
text = re.sub(r'^(\s*)\.globl\s+([A-Za-z_]\w*)\s*$', r'\1.globl \2\n\1.hidden \2', text, flags=re.M)
text += '\n.section .note.GNU-stack,"",%progbits\n'
open(dst, 'w').write(text)
