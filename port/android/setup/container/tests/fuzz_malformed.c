/* Damaged made-up files through the reader, for ASan and UBSan builds: containers, NCA headers, tickets, content meta, ExeFS,
   BKTR tables and RomFS tables damaged in the plain before they are hashed and encrypted, then files cut short, bytes flipped
   in the encrypted file, and damaged key text.
     fuzz_malformed <iterations> [seed]        exit 0: no crash; prints how many were accepted and refused */
#include "fuzz_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state;
static uint64_t rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static uint64_t odd_value(void) {
    static const uint64_t v[] = {0, 1, 2, 3, 0x10, 0x50, 0x200, 0xFFF, 0x1000, 0x3FF, 0x332, 0x3FF0, 0x4000, 0x4001, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                 0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFF0, 0xFFFFFFFFFFFFFE00};
    return below(3) ? v[below(sizeof v / sizeof *v)] : rnd();
}

/* Places worth aiming at, by region: the fields, not the padding. */
static uint32_t aim(sy_region r) {
    switch (r) {
    case SY_NCA_HEADER:
        switch (below(5)) {
        case 0: return 0x200 + below(0x40);          /* magic, types, size, title */
        case 1: return 0x240 + below(0x40);          /* section table */
        case 2: return 0x400 + below(0x800);         /* section headers */
        case 3: return 0x500 + below(0x40) + 0x200 * below(2);   /* BKTR headers */
        default: return below(0xC00);
        }
    case SY_BKTR_REL: case SY_BKTR_SUB:
        switch (below(4)) {
        case 0: return below(0x40);                  /* outer node */
        case 1: return 0x4000 + below(0x100);        /* first bucket */
        case 2: return 0x8000 + below(0x40);         /* a second bucket, or past the table */
        default: return below(0x8000);
        }
    case SY_ROMFS_HEADER: return below(0x50);
    case SY_TICKET: return 0x140 + below(0x180);
    default: return below(0x400);
    }
}

int main(int argc, char **argv) {
    uint64_t iterations = argc > 1 ? strtoull(argv[1], NULL, 0) : 1000, seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    uint64_t accepted = 0, refused = 0;
    state = seed * 0x9E3779B97F4A7C15ull + 1;
    for (uint64_t i = 0; i < iterations; i++) {
        fz_edit edits[12];
        fz_plan plan = {edits, below(sizeof edits / sizeof *edits)};
        if (i % 3 == 0 && !plan.count) plan.count = 1;
        sy_region focus = (sy_region)below(SY_REGION_COUNT);
        for (size_t e = 0; e < plan.count; e++) {
            sy_region r = below(4) ? focus : (sy_region)below(SY_REGION_COUNT);
            edits[e] = (fz_edit){(uint8_t)r, (uint8_t)(1u << below(4)), aim(r), odd_value()};
        }
        sy_spec spec;
        sy_default(&spec, rnd());
        spec.xci = (int)below(4) == 0;
        spec.pieces = 1 + below(below(8) ? 60 : 900);
        spec.subsections = 1 + below(below(8) ? 30 : 1100);
        spec.hook = fz_hook; spec.user = &plan;
        sy_game g;
        sy_make(&g, &spec);
        size_t base = g.base.size, update = g.update.size, keys = g.keys_size;
        int damage = (int)below(6);
        if (damage == 0) base = below((uint32_t)base + 1);
        if (damage == 1) update = below((uint32_t)update + 1);
        if (damage == 2) for (int k = 0; k < 16; k++) g.base.data[below((uint32_t)base)] ^= (uint8_t)(1u << below(8));
        if (damage == 3) for (int k = 0; k < 16; k++) g.update.data[below(0x4000 < update ? 0x4000 : (uint32_t)update)] = (uint8_t)rnd();
        if (damage == 4) {
            keys = below((uint32_t)keys + 1);
            for (int k = 0; k < 3 && keys; k++) g.keys_text[below((uint32_t)keys)] = (char)(below(2) ? rnd() : "=\n#; \r"[below(6)]);
        }
        if (fz_exercise(g.base.data, base, g.update.data, update, g.keys_text, keys, rnd())) accepted++;
        else refused++;
        sy_free(&g);
    }
    printf("%llu damaged pairs: %llu accepted, %llu refused\n", (unsigned long long)iterations, (unsigned long long)accepted, (unsigned long long)refused);
    return 0;
}
