/* The patched section: a BKTR update is a list of ranges, each from the base game's section or the update's own, and a second
   list that gives every range of the update's section its own counter. The tables are small (about 3000 entries in 1.4.3), so
   they are flattened into sorted arrays and checked once, at open. A read then only searches them. */
#include "container_internal.h"
#include <stdlib.h>

#define NODE 0x4000u
#define NODE_HEADER 0x10u
#define REL_SIZE 0x14u
#define SUB_SIZE 0x10u
#define MAX_TABLE (256u << 20)
/* a long run is read and decrypted in steps, so the bytes are still in cache */
#define CHUNK (1u << 20)

/* Reads a table and checks its outer node. *total is where its ranges end. */
static int table_read(const tokc_section *s, const uint8_t *info, const char *what, uint8_t **out, uint64_t *size_out,
                      uint32_t *buckets, uint32_t *entries, uint64_t *total, char *err, size_t errlen) {
    uint64_t off = rd64(info), size = rd64(info + 8);
    *out = NULL;
    if (memcmp(info + 0x10, "BKTR", 4) || rd32(info + 0x14) != 1) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's %s table has no BKTR header", what);
    if (size < 2 * NODE || size > MAX_TABLE || !fits(off, size, s->size)) return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's %s table is out of bounds", what);
    uint8_t *t = malloc((size_t)size);
    if (!t) return tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory");
    int rc = tokc_section_read(s, t, (size_t)size, off);
    if (rc) { free(t); return tokc_fail(err, errlen, rc, "cannot read the update's %s table", what); }
    uint32_t n = rd32(t + 4);
    if (rd32(t) != 0 || !n || n > (NODE - NODE_HEADER) / 8 || (uint64_t)NODE * (n + 1) > size) {
        free(t);
        return tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's %s table is malformed", what);
    }
    *out = t; *size_out = size; *buckets = n; *entries = rd32(info + 0x18); *total = rd64(t + 8);
    return TOKC_OK;
}

/* A bucket's header, against the outer node's offset for it and its place. */
static const uint8_t *bucket(const uint8_t *t, uint32_t i, uint32_t max_entries, uint32_t *count) {
    const uint8_t *b = t + (size_t)NODE * (i + 1);
    *count = rd32(b + 4);
    return rd32(b) != i || !*count || *count > max_entries ? NULL : b + NODE_HEADER;
}

/* The subsections cover the section up to the tables, which start with the relocation table. */
static int load_subsections(tokc_bktr *b, const tokc_section *s, const uint8_t *info, uint64_t tables_at, char *err, size_t errlen) {
    uint8_t *t;
    uint64_t size;
    uint32_t buckets, entries;
    int rc = table_read(s, info, "subsection", &t, &size, &buckets, &entries, &b->sub_total, err, errlen);
    if (rc) return rc;
    if (b->sub_total != tables_at || !entries || (uint64_t)entries > (uint64_t)buckets * ((NODE - NODE_HEADER) / SUB_SIZE)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's subsection table is malformed"); goto out; }
    b->sub_off = malloc((size_t)entries * 8); b->sub_ctr = malloc((size_t)entries * 4);
    if (!b->sub_off || !b->sub_ctr) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    uint64_t n = 0;
    for (uint32_t i = 0; i < buckets; i++) {
        uint32_t count;
        const uint8_t *e = bucket(t, i, (NODE - NODE_HEADER) / SUB_SIZE, &count);
        if (!e || rd64(t + NODE_HEADER + 8 * i) != rd64(e) || n + count > entries) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's subsection table is malformed"); goto out; }
        for (uint32_t k = 0; k < count; k++, e += SUB_SIZE, n++) {
            uint64_t off = rd64(e);
            if (off >= b->sub_total || (n ? off <= b->sub_off[n - 1] : off != 0)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's subsection table is out of order"); goto out; }
            b->sub_off[n] = off; b->sub_ctr[n] = rd32(e + 12);
        }
    }
    if (n != entries) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's subsection table is short"); goto out; }
    b->nsub = n;
out:
    free(t);
    return rc;
}

static int load_relocations(tokc_bktr *b, const tokc_section *s, const uint8_t *info, uint64_t base_size, char *err, size_t errlen) {
    uint8_t *t;
    uint64_t size;
    uint32_t buckets, entries;
    int rc = table_read(s, info, "relocation", &t, &size, &buckets, &entries, &b->total, err, errlen);
    if (rc) return rc;
    if (!entries || (uint64_t)entries > (uint64_t)buckets * ((NODE - NODE_HEADER) / REL_SIZE) || !b->total) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's relocation table is malformed"); goto out; }
    b->virt = malloc((size_t)entries * 8); b->phys = malloc((size_t)entries * 8);
    if (!b->virt || !b->phys) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    uint64_t n = 0;
    for (uint32_t i = 0; i < buckets; i++) {
        uint32_t count;
        const uint8_t *e = bucket(t, i, (NODE - NODE_HEADER) / REL_SIZE, &count);
        if (!e || rd64(t + NODE_HEADER + 8 * i) != rd64(e) || n + count > entries) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's relocation table is malformed"); goto out; }
        for (uint32_t k = 0; k < count; k++, e += REL_SIZE, n++) {
            uint64_t virt = rd64(e), phys = rd64(e + 8);
            uint32_t patch = rd32(e + 16);
            if (patch > 1 || phys >= TOKC_BKTR_PATCH || virt >= b->total || (n ? virt <= b->virt[n - 1] : virt != 0)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's relocation table is out of order"); goto out; }
            b->virt[n] = virt; b->phys[n] = patch ? phys | TOKC_BKTR_PATCH : phys;
        }
    }
    if (n != entries) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's relocation table is short"); goto out; }
    b->nrel = n;
    /* every range must lie inside its source, so that a read needs no further check */
    for (uint64_t i = 0; i < n; i++) {
        uint64_t len = (i + 1 < n ? b->virt[i + 1] : b->total) - b->virt[i];
        uint64_t phys = b->phys[i] & ~TOKC_BKTR_PATCH;
        if (!fits(phys, len, b->phys[i] & TOKC_BKTR_PATCH ? b->sub_total : base_size)) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "an update range lies outside its source: wrong base game, or a damaged file"); goto out; }
    }
out:
    free(t);
    return rc;
}

int tokc_bktr_load(tokc_bktr *b, const tokc_section *update, const uint8_t *fs, uint64_t base_size, char *err, size_t errlen) {
    memset(b, 0, sizeof *b);
    int rc = load_subsections(b, update, fs + 0x120, rd64(fs + 0x100), err, errlen);
    if (!rc) rc = load_relocations(b, update, fs + 0x100, base_size, err, errlen);
    if (!rc && b->sub_total > update->size) rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "the update's subsections run past its section");
    if (rc) tokc_bktr_free(b);
    return rc;
}

void tokc_bktr_free(tokc_bktr *b) {
    free(b->virt); free(b->phys); free(b->sub_off); free(b->sub_ctr);
    memset(b, 0, sizeof *b);
}

/* The last entry at or before v; a[0] is 0 and v is in range. */
static uint64_t find(const uint64_t *a, uint64_t n, uint64_t v) {
    uint64_t lo = 0;
    while (n > 1) {
        uint64_t half = n / 2;
        if (a[lo + half] <= v) lo += half;
        n -= half;
    }
    return lo;
}

/* Bytes of the update's own section. One read, then each subsection with its counter. */
static int read_update(const tokc_game *g, uint8_t *p, size_t n, uint64_t off) {
    const tokc_section *s = &g->update;
    const tokc_bktr *b = &g->bktr;
    int rc = tokc_pread_full(s->fd, p, n, s->file_off + off);
    if (rc) return rc;
    uint8_t high[8];
    memcpy(high, s->high, 8);
    for (uint64_t i = find(b->sub_off, b->nsub, off); n; i++) {
        uint64_t end = i + 1 < b->nsub ? b->sub_off[i + 1] : b->sub_total;
        size_t run = end - off < n ? (size_t)(end - off) : n;
        uint32_t c = b->sub_ctr[i];
        high[4] = (uint8_t)(c >> 24); high[5] = (uint8_t)(c >> 16); high[6] = (uint8_t)(c >> 8); high[7] = (uint8_t)c;
        if (s->encrypted) tokc_aes_ctr(&s->aes, high, s->nca_off + off, p, run);
        p += run; off += run; n -= run;
    }
    return TOKC_OK;
}

int64_t tokc_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off) {
    if (!g || (!buf && n)) return TOKC_E_ARGS;
    if (off >= g->image_size) return 0;
    if (n > g->image_size - off) n = (size_t)(g->image_size - off);
    if (n > INT64_MAX) n = INT64_MAX;
    const tokc_bktr *b = &g->bktr;
    uint8_t *p = buf;
    uint64_t v = g->image_off + off;
    size_t left = n;
    for (uint64_t i = find(b->virt, b->nrel, v); left;) {
        uint64_t end = i + 1 < b->nrel ? b->virt[i + 1] : b->total;
        if (v >= end) { i++; continue; }
        uint64_t src = (b->phys[i] & ~TOKC_BKTR_PATCH) + (v - b->virt[i]);
        size_t run = end - v < left ? (size_t)(end - v) : left;
        if (run > CHUNK) run = CHUNK;
        int rc = b->phys[i] & TOKC_BKTR_PATCH ? read_update(g, p, run, src) : tokc_section_read(&g->base, p, run, src);
        if (rc) return rc;
        p += run; v += run; left -= run;
    }
    return (int64_t)n;
}

int64_t tokc_base_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off) {
    if (!g || (!buf && n)) return TOKC_E_ARGS;
    if (off >= g->base_image_size) return 0;
    if (n > g->base_image_size - off) n = (size_t)(g->base_image_size - off);
    if (n > INT64_MAX) n = INT64_MAX;
    uint8_t *p = buf;
    for (size_t left = n; left;) {
        size_t run = left > CHUNK ? CHUNK : left;
        int rc = tokc_section_read(&g->base, p, run, g->base_image_off + off);
        if (rc) return rc;
        p += run; off += run; left -= run;
    }
    return (int64_t)n;
}

int tokc_romfs_same_as_base(const tokc_game *g, uint64_t off, uint64_t base_off, uint64_t n) {
    if (!g || !fits(off, n, g->image_size) || !fits(base_off, n, g->base_image_size)) return 0;
    const tokc_bktr *b = &g->bktr;
    uint64_t v = g->image_off + off, want = g->base_image_off + base_off;
    if (!n) return 1;
    for (uint64_t i = find(b->virt, b->nrel, v); n; i++) {
        uint64_t end = i + 1 < b->nrel ? b->virt[i + 1] : b->total, run = end - v < n ? end - v : n;
        if (b->phys[i] & TOKC_BKTR_PATCH || b->phys[i] + (v - b->virt[i]) != want) return 0;
        v += run; want += run; n -= run;
    }
    return 1;
}

uint64_t tokc_romfs_runs(const tokc_game *g, uint64_t off, uint64_t n) {
    const tokc_bktr *b = &g->bktr;
    if (off >= g->image_size || !n) return 0;
    if (n > g->image_size - off) n = g->image_size - off;
    uint64_t v = g->image_off + off, last = v + n - 1;
    return find(b->virt, b->nrel, last) - find(b->virt, b->nrel, v) + 1;
}
