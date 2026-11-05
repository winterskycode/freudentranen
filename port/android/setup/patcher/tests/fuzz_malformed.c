/* Damaged modules through the rewriter, for ASan and UBSan builds: truncation, header fields, flipped bytes in the
   compressed data, and, on a copy stored uncompressed, MOD0, .eh_frame_hdr and text. Every input and image buffer has its
   exact size, so a read or write past one stops the run.
     fuzz_malformed <module> <iterations> [seed]        exit 0: no crash; prints how many were accepted and refused */
#include "patcher_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state;
static uint64_t rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static size_t below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); rewind(f);
    uint8_t *p = malloc(*size);
    if (!p || fread(p, 1, *size, f) != *size) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fclose(f);
    return p;
}

/* The module with its three segments stored uncompressed, so damage reaches what the parsers read. */
static uint8_t *flatten(const uint8_t *nso, size_t size, size_t *flat_size, size_t off[3], size_t len[3]) {
    tokp_layout l;
    char err[200];
    if (tokp_read_layout(nso, size, &l, err, sizeof err)) { fprintf(stderr, "%s\n", err); exit(2); }
    uint8_t *img = malloc(l.patch_off);
    if (tokp_load_image(nso, &l, img, err, sizeof err)) { fprintf(stderr, "%s\n", err); exit(2); }
    size_t at = 0x100;
    for (int i = 0; i < 3; i++) { off[i] = at; len[i] = rd32(nso + 0x18 + 0x10 * i); at += len[i]; }
    uint8_t *f = malloc(at);
    memcpy(f, nso, 0x100);
    wr32(f + 0xC, 0);
    for (int i = 0; i < 3; i++) {
        memcpy(f + off[i], img + rd32(nso + 0x14 + 0x10 * i), len[i]);
        wr32(f + 0x10 + 0x10 * i, (uint32_t)off[i]); wr32(f + 0x60 + 4 * i, (uint32_t)len[i]);
    }
    free(img);
    *flat_size = at;
    return f;
}

static uint32_t odd_value(size_t size) {
    static const uint32_t v[] = {0, 1, 3, 4, 0x100, 0xFFF, 0x1000, 0x4000, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFC, 0x3FFFFFFF, 0x40000000};
    switch (below(3)) {
        case 0: return v[below(sizeof v / sizeof *v)];
        case 1: return (uint32_t)below(size + 16);
        default: return (uint32_t)rnd();
    }
}

static const uint32_t words[] = {0xD4000001, 0xD53BD040, 0xD53BD05F, 0xD51BD040, 0xD53BD060, 0xD53BE020, 0xD53BE040, 0x885F7C20, 0x88027C20, 0xD503305F,
                                 0x14000000, 0x17FFFFFF, 0x54FFFFE0, 0x34000020, 0x36000020, 0xD65F03C0, 0xD61F0200, 0xFFFFFFFF, 0};

/* The MOD0 and .eh_frame_hdr offsets of the flat copy, for aiming at them. */
static size_t find_mod0(const uint8_t *flat, const size_t off[3], const size_t len[3]) {
    uint32_t m0 = rd32(flat + off[0] + 4), ro_mem = rd32(flat + 0x24);
    if (m0 >= ro_mem && m0 - ro_mem + 32 <= len[1]) return off[1] + (m0 - ro_mem);
    return m0 + 32 <= len[0] ? off[0] + m0 : 0;
}

static int run(const uint8_t *nso, size_t size) {
    /* exact size: the sanitizer sees any read past it */
    uint8_t *in = malloc(size ? size : 1);
    memcpy(in, nso, size);
    tokp_layout l;
    char err[200];
    int rc = tokp_read_layout(in, size, &l, err, sizeof err);
    if (!rc) {
        size_t cap = (size_t)l.patch_off;
        uint8_t *img = malloc(cap);
        tokp_result r;
        rc = img ? tokp_rewrite(in, size, img, cap, &r, err, sizeof err) : TOKP_E_NOMEM;
        if (rc == TOKP_E_SPACE) {
            cap = (size_t)r.image_size;
            free(img);
            img = malloc(cap);
            rc = img ? tokp_rewrite(in, size, img, cap, &r, err, sizeof err) : TOKP_E_NOMEM;
        }
        if (rc == TOKP_OK) {
            if (r.image_size != cap && r.image_size != l.patch_off) { fprintf(stderr, "image size\n"); abort(); }
            if (r.image_size != l.patch_off + r.patch_size) { fprintf(stderr, "patch size\n"); abort(); }
            tokp_result_free(&r);
        }
        free(img);
    }
    free(in);
    if (rc > 0 || rc < TOKP_E_DECODER) { fprintf(stderr, "unknown result %d\n", rc); abort(); }
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <module> <iterations> [seed]\n", argv[0]); return 2; }
    size_t size, flat_size, off[3], len[3];
    uint8_t *nso = read_file(argv[1], &size);
    uint8_t *flat = flatten(nso, size, &flat_size, off, len);
    long n = atol(argv[2]);
    state = argc > 3 ? strtoull(argv[3], NULL, 0) | 1 : 0x9E3779B97F4A7C15ull;
    size_t mod0 = find_mod0(flat, off, len);
    tokp_layout layout;
    char err[200];
    if (tokp_read_layout(flat, flat_size, &layout, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    uint64_t end = layout.patch_off;
    uint8_t *work = malloc(size > flat_size ? size : flat_size);
    long tally[8] = {0};
    for (long i = 0; i < n; i++) {
        size_t m = size;
        const uint8_t *src = nso;
        unsigned how = (unsigned)below(9);
        if (how >= 4) { src = flat; m = flat_size; }
        memcpy(work, src, m);
        switch (how) {
            /* truncated */
            case 0:
                m = below(4) ? below(m) : below(0x200);
                break;
            /* header bytes */
            case 1:
                for (size_t k = 1 + below(8); k; k--) work[below(0x100)] ^= (uint8_t)(1u << below(8));
                break;
            /* header fields */
            case 2: {
                static const unsigned at[] = {0xC, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x3C, 0x60, 0x64, 0x68};
                for (size_t k = 1 + below(3); k; k--) wr32(work + at[below(sizeof at / sizeof *at)], odd_value(m));
                break;
            }
            /* compressed data */
            case 3:
                for (size_t k = 1 + below(64); k; k--) work[0x100 + below(m - 0x100)] = (uint8_t)rnd();
                break;
            /* the MOD0 offset, and the header it leads to; both also at the very end of the image */
            case 4:
                switch (below(4)) {
                    case 0: wr32(work + off[0] + 4, odd_value(m)); break;
                    case 1: wr32(work + off[0] + 4, (uint32_t)(end - below(40))); break;
                    case 2: if (mod0) wr32(work + mod0 + 4 * below(8), odd_value(m)); break;
                    default: if (mod0) wr32(work + mod0 + 16, (uint32_t)(end - below(40) - rd32(work + off[0] + 4))); break;
                }
                break;
            /* .eh_frame_hdr: encodings, count, table */
            case 5:
                if (mod0) {
                    size_t eh = mod0 + (size_t)(int32_t)rd32(work + mod0 + 16);
                    if (eh + 0x40 < m) {
                        if (below(2)) work[eh + 1 + below(3)] = (uint8_t)rnd();
                        else wr32(work + eh + 8, odd_value(m));
                        for (size_t k = below(16); k; k--) wr32(work + eh + 12 + 4 * below(12), odd_value(m));
                    }
                }
                break;
            /* the end of the table: starts that run past everything */
            case 6: {
                if (mod0) {
                    size_t eh = mod0 + (size_t)(int32_t)rd32(work + mod0 + 16);
                    if (eh + 16 < m) { work[eh + 3] = 0x04; wr32(work + eh + 8, 0xFFFFFF); }
                }
                break;
            }
            /* text */
            case 7:
                for (size_t k = 1 + below(200); k; k--)
                    wr32(work + off[0] + (below(len[0] / 4 - 2) + 2) * 4, words[below(sizeof words / sizeof *words)]);
                break;
            default:
                for (size_t k = 1 + below(1000); k; k--) work[below(m)] = (uint8_t)rnd();
                break;
        }
        tally[-run(work, m)]++;
    }
    printf("%s: %ld runs: ok %ld, malformed %ld, refused by a rule %ld, decoder %ld, no memory %ld\n", argv[1], n, tally[0], tally[-TOKP_E_FORMAT],
           tally[-TOKP_E_RULE], tally[-TOKP_E_DECODER], tally[-TOKP_E_NOMEM]);
    free(work); free(flat); free(nso);
    return 0;
}
