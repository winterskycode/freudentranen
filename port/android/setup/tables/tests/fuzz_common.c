/* fuzz_common.h */
#include "fuzz_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static void die(const char *what) { fprintf(stderr, "%s\n", what); abort(); }

static void find_regions(fz_case *c, unsigned m) {
    const uint8_t *img = c->image[m];
    uint64_t size = c->rewrite[m].image_size;
    fz_region *r = c->region[m];
    uint64_t tag[38] = {0};
    uint64_t mod0 = rd32(img + 4);
    int64_t dyn = (int64_t)mod0 + (int32_t)rd32(img + mod0 + 4);
    uint64_t n = 0;
    while (rd64(img + (uint64_t)dyn + 16 * n)) {
        uint64_t t = rd64(img + (uint64_t)dyn + 16 * n);
        if (t < 38) tag[t] = rd64(img + (uint64_t)dyn + 16 * n + 8);
        n++;
    }
    r[FZ_HEAD] = (fz_region){0, 8};
    r[FZ_MOD0] = (fz_region){mod0, 32};
    r[FZ_DYNAMIC] = (fz_region){(uint64_t)dyn, 16 * (n + 1)};
    uint64_t nsym = tag[4] ? rd32(img + tag[4] + 4) : rd32(c->nso[m] + 0x9C) / 24;
    r[FZ_SYMBOLS] = (fz_region){tag[6], nsym * 24};
    r[FZ_NAMES] = (fz_region){tag[5], tag[10]};
    r[FZ_RELA] = (fz_region){tag[7], tag[8]};
    r[FZ_JMPREL] = (fz_region){tag[23], tag[2]};
    r[FZ_RELR] = (fz_region){tag[36], tag[35]};
    r[FZ_HASH] = (fz_region){tag[4], tag[4] ? 8 : 0};
    r[FZ_INIT] = (fz_region){tag[12], 32};
    uint64_t end = c->rewrite[m].layout.text_end;
    r[FZ_PLT] = (fz_region){end > 0x10000 ? end - 0x10000 : 0, end > 0x10000 ? 0x10000 : end};
    r[FZ_ALL] = (fz_region){0, size};
}

void fz_load(fz_case *c, const char *rewritten, const char *exefs) {
    char err[200];
    memset(c, 0, sizeof *c);
    for (unsigned m = 0; m < TOKT_MODULE_COUNT; m++) {
        tokt_files *f = &c->f[m];
        if (tokt_files_load(f, rewritten, exefs, tokt_module_names[m], err, sizeof err)) die(err);
        c->image[m] = f->image;          /* malloc'd at its exact size */
        c->nso_size[m] = f->nso_size < 0x100 ? f->nso_size : 0x100;
        c->nso[m] = malloc(c->nso_size[m]);
        if (!c->nso[m]) die("memory");
        c->gates[m] = malloc((f->rewrite.ngates + 1) * sizeof(tokp_gate));
        if (!c->gates[m]) die("memory");
        fz_reset(c);
        tokt_sha256_hex(c->image[m], (size_t)f->rewrite.image_size, c->digest[m]);
        find_regions(c, m);
    }
}

void fz_reset(fz_case *c) {
    for (unsigned m = 0; m < TOKT_MODULE_COUNT; m++) {
        if (!c->f[m].image) continue;
        c->rewrite[m] = c->f[m].rewrite;
        if (!c->nso[m] || !c->gates[m]) continue;
        memcpy(c->nso[m], c->f[m].nso, c->nso_size[m]);
        if (c->rewrite[m].ngates) memcpy(c->gates[m], c->f[m].rewrite.gates, c->rewrite[m].ngates * sizeof(tokp_gate));
        c->rewrite[m].gates = c->gates[m];
    }
}

void fz_edit(fz_case *c, unsigned mod, unsigned region, uint64_t off, unsigned width, uint64_t value) {
    const fz_region *r = &c->region[mod][region];
    uint64_t size = c->f[mod].rewrite.image_size;
    if (!r->len || c->nundo == FZ_MAX_UNDO) return;
    uint64_t at = r->off + off % r->len;
    if (at >= size) return;
    if (width > size - at) width = (unsigned)(size - at);
    fz_undo *u = &c->undo[c->nundo++];
    u->mod = mod; u->off = at; u->n = width;
    memcpy(u->old, c->image[mod] + at, width);
    for (unsigned i = 0; i < width; i++) c->image[mod][at + i] = (uint8_t)(value >> 8 * i);
}

void fz_revert(fz_case *c) {
    while (c->nundo) {
        fz_undo *u = &c->undo[--c->nundo];
        memcpy(c->image[u->mod] + u->off, u->old, u->n);
    }
}

static int range_ok(uint64_t size, uint64_t off, uint64_t count, uint64_t each) { return off % 8 == 0 && off <= size && count <= (size - off) / each; }

/* What loader.c load_tables and relocate() insist on. */
static void check_tables(const uint8_t *b, size_t size) {
    if (size < 64 || size >= TOKT_MAX_TABLES) die("tables size");
    if (memcmp(b, "TOTKTAB1", 8) || strncmp((const char *)b + 8, TOKT_VERSION, 8) || rd32(b + 16) != 3 || rd32(b + 20) != 1) die("tables header");
    uint64_t strings = rd64(b + 40), strings_size = rd64(b + 48);
    if (rd64(b + 56) != size || !strings_size || !range_ok(size, strings, strings_size, 1) || b[strings + strings_size - 1]) die("tables layout");
    if (strings + strings_size != size) die("tables end");
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *t = b + 64 + 208 * i;
        if (strcmp((const char *)t, tokt_module_names[i]) || strnlen((const char *)t + 16, 72) != 64) die("tables module");
        uint64_t data_off = rd64(t + 88 + 24), key_off = rd64(t + 88 + 40), symbols = rd64(t + 88 + 80), relocs = rd64(t + 88 + 88), gates = rd64(t + 88 + 96);
        uint32_t nsym = rd32(t + 192), nreloc = rd32(t + 196), ngate = rd32(t + 200);
        if (!range_ok(size, symbols, nsym, 24) || !range_ok(size, relocs, nreloc, 24) || !range_ok(size, gates, ngate, 24)) die("tables bounds");
        for (uint32_t k = 0; k < nsym; k++) if (rd32(b + symbols + 24 * k + 16) >= strings_size) die("symbol name");
        for (uint32_t k = 0; k < nreloc; k++) {
            const uint8_t *r = b + relocs + 24 * k;
            uint64_t where = rd64(r), addend = rd64(r + 16);
            uint32_t kind = rd32(r + 8), symbol = rd32(r + 12);
            if (where < data_off || where + 8 > key_off || (kind != 1027 && symbol >= nsym)) die("relocation bounds");
            if (kind == 1027 ? addend >= key_off : kind != 257 && kind != 1025 && kind != 1026) die("relocation kind");
        }
        for (uint32_t k = 1; k < ngate; k++) if (rd64(b + gates + 24 * k) <= rd64(b + gates + 24 * (k - 1))) die("gate order");
    }
}

int fz_run(fz_case *c, const uint64_t image_bytes[TOKT_MODULE_COUNT]) {
    tokt_module m[TOKT_MODULE_COUNT];
    uint8_t *cut[TOKT_MODULE_COUNT] = {NULL};
    tokp_result r[TOKT_MODULE_COUNT];
    for (unsigned i = 0; i < TOKT_MODULE_COUNT; i++) {
        r[i] = c->rewrite[i];
        const uint8_t *img = c->image[i];
        if (image_bytes && image_bytes[i]) {     /* an exact-size copy of the start */
            r[i].image_size = image_bytes[i];
            cut[i] = malloc((size_t)image_bytes[i]);
            if (!cut[i]) die("memory");
            memcpy(cut[i], img, (size_t)image_bytes[i]);
            img = cut[i];
        }
        m[i] = (tokt_module){c->nso[i], c->nso_size[i], img, &r[i], c->digest[i]};
    }
    uint8_t *tables = NULL;
    size_t size = 0;
    char err[200];
    int rc = tokt_make(m, &tables, &size, err, sizeof err);
    if (rc == TOKT_OK) {
        uint8_t *exact = malloc(size);    /* exact size for the check too */
        if (!exact) die("memory");
        memcpy(exact, tables, size);
        check_tables(exact, size);
        free(exact);
    } else if (tables || size) die("output after an error");
    free(tables);
    for (unsigned i = 0; i < TOKT_MODULE_COUNT; i++) free(cut[i]);
    if (rc > 0 || rc < TOKT_E_NOMEM) die("unknown result");
    return rc;
}
