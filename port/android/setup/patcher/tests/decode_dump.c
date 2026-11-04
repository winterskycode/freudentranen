/* What this build of Capstone makes of a set of words, as text, for decode_check.py to compare with the Python binding.
     decode_dump --sites             every word the rewriter's scan can pick
     decode_dump <image> <from> <to> the aligned words of a raw image file, offsets in hex
   One line per word: address, mnemonic, operands, or "(bad)". */
#include <capstone/capstone.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static csh cs;
static cs_insn *ins;

static void one(uint32_t w, uint64_t addr) {
    uint8_t b[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
    const uint8_t *p = b;
    size_t left = 4;
    uint64_t a = addr;
    if (cs_disasm_iter(cs, &p, &left, &a, ins)) printf("%" PRIx64 " %s %s\n", addr, ins->mnemonic, ins->op_str);
    else printf("%" PRIx64 " (bad)\n", addr);
}

int main(int argc, char **argv) {
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &cs) != CS_ERR_OK) return 2;
    cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON);
    ins = cs_malloc(cs);
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    if (argc == 2) {
        for (uint32_t i = 0; i < 65536; i++) one(0xD4000001 | i << 5, 0x1000);
        static const uint32_t bases[] = {0xD53BD040, 0xD51BD040, 0xD53BD060, 0xD53BE020, 0xD53BE040};
        for (int k = 0; k < 5; k++) for (uint32_t n = 0; n < 32; n++) one(bases[k] | n, 0x1000);
    } else if (argc == 4) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        uint64_t from = strtoull(argv[2], NULL, 16), to = strtoull(argv[3], NULL, 16);
        fseek(f, (long)from, SEEK_SET);
        for (uint64_t o = from; o < to; o += 4) {
            uint8_t b[4];
            if (fread(b, 1, 4, f) != 4) break;
            one((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24, o);
        }
        fclose(f);
    } else { fprintf(stderr, "usage: %s --sites | <image> <from> <to>\n", argv[0]); return 2; }
    cs_free(ins, 1);
    cs_close(&cs);
    return 0;
}
