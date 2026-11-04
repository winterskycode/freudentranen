/* Damaged modules through tokt_make, for ASan and UBSan builds: images cut short, every structure the tables read, the
   patcher's results and the NSO header. Whatever is accepted must pass load_tables' checks.
     fuzz_malformed <rewritten dir> <exefs dir> <iterations> [seed]    exit 0: no crash; prints what was accepted and refused */
#include "fuzz_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state;
static uint64_t rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

static uint64_t odd_value(uint64_t size) {
    static const uint64_t v[] = {0, 1, 3, 4, 7, 8, 24, 0x100, 0xFFF, 0x1000, 0x4000, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFC, 0x3FFFFFFF, 0x40000000,
                                 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFF8, 0xFFFFFFFFFFFFFFE8, 257, 1025, 1026, 1027};
    switch (below(4)) {
        case 0: return v[below(sizeof v / sizeof *v)];
        case 1: return below(size + 64);
        case 2: return size - below(64);
        default: return rnd();
    }
}

static const unsigned widths[] = {1, 2, 4, 8};

/* A field of the results. */
static void damage_layout(fz_case *c, unsigned m) {
    tokp_result *r = &c->rewrite[m];
    uint64_t size = r->image_size, v = odd_value(size);
    switch (below(7)) {
        case 0: r->layout.text_end = v; break;
        case 1: r->layout.ro_off = v; break;
        case 2: r->layout.data_off = v; break;
        case 3: r->layout.key_page_off = v; break;
        case 4: r->layout.patch_off = v; break;
        case 5: r->layout.data_off = r->layout.key_page_off - below(64); break;
        default: r->layout.key_page_off = r->layout.data_off + below(64); break;
    }
}

static void damage_gates(fz_case *c, unsigned m) {
    tokp_result *r = &c->rewrite[m];
    if (!r->ngates) return;
    size_t i = (size_t)below(r->ngates), j = (size_t)below(r->ngates);
    switch (below(3)) {
        case 0: r->gates[i].kind = (uint32_t)below(TOKP_KIND_COUNT + 2); break;
        case 1: r->gates[i].ret = r->gates[j].ret; break;
        default: r->ngates = (size_t)below(r->ngates); break;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <rewritten dir> <exefs dir> <iterations> [seed]\n", argv[0]); return 2; }
    static fz_case c;
    fz_load(&c, argv[1], argv[2]);
    long n = atol(argv[3]), tally[6] = {0};
    state = argc > 4 ? strtoull(argv[4], NULL, 0) | 1 : 0x9E3779B97F4A7C15ull;
    if (fz_run(&c, NULL) != TOKT_OK) { fprintf(stderr, "the undamaged modules are refused\n"); return 2; }
    for (long i = 0; i < n; i++) {
        unsigned m = (unsigned)below(TOKT_MODULE_COUNT), how = (unsigned)below(FZ_REGIONS + 4);
        uint64_t size = c.rewrite[m].image_size, cut[TOKT_MODULE_COUNT] = {0};
        if (how < FZ_REGIONS) {
            const fz_region *r = &c.region[m][how];
            for (uint64_t k = 1 + below(how == FZ_ALL ? 2000 : 12); k; k--) {
                uint64_t at = below(r->len), v = odd_value(size);
                unsigned w = widths[below(4)];
                if (how == FZ_NAMES && below(2)) { v = 0x80 + below(0x80); w = 1; }   /* no terminator, bad UTF-8 */
                if (how == FZ_SYMBOLS || how == FZ_RELA || how == FZ_JMPREL) { at = at / 8 * 8; w = below(2) ? 8 : 4; }
                fz_edit(&c, m, how, at, w, v);
            }
        } else switch (how - FZ_REGIONS) {
            case 0: {   /* cut short, also just past each structure */
                unsigned r = (unsigned)below(FZ_REGIONS);
                uint64_t end = c.region[m][r].off + c.region[m][r].len;
                cut[m] = below(2) ? 1 + below(size) : end > 40 ? end - below(40) : 1;
                if (cut[m] > size) cut[m] = size;
                break;
            }
            case 1: for (uint64_t k = 1 + below(2); k; k--) damage_layout(&c, m); break;
            case 2: damage_gates(&c, m); break;
            default:   /* the NSO header, or too little of it */
                if (below(3)) { for (uint64_t k = 1 + below(4); k; k--) { unsigned at = 0x90 + (unsigned)below(16); c.nso[m][at] = (uint8_t)rnd(); } }
                else c.nso_size[m] = (size_t)below(0x100);
                break;
        }
        tally[-fz_run(&c, cut)]++;
        fz_revert(&c);
        c.nso_size[m] = c.f[m].nso_size < 0x100 ? c.f[m].nso_size : 0x100;
        fz_reset(&c);
    }
    printf("%s: %ld runs: ok %ld, malformed %ld, too big %ld, no memory %ld\n", argv[1], n, tally[0], tally[-TOKT_E_FORMAT], tally[-TOKT_E_TOO_BIG], tally[-TOKT_E_NOMEM]);
    return 0;
}
