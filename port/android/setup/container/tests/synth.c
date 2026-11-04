/* See synth.h. The bytes are pseudo-random; the formats are built from the same field offsets the reader uses, so what this
   proves against real files is proved by regress.sh, and what it proves here is that malformed input is handled. */
#define _GNU_SOURCE 1
#include "synth.h"
#include "../container_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define GENERATION 0x0e   /* stored as this + 1 */
#define NODE 0x4000u

typedef struct { uint8_t *p; size_t n; } buf;
static const uint8_t header_key[32] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                       0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f};
static const uint8_t titlekek[16] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
static const uint8_t area_key[16] = {0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf};

static void *zalloc(size_t n) { void *p = calloc(n ? n : 1, 1); if (!p) abort(); return p; }
static size_t align(size_t v, size_t a) { return (v + a - 1) / a * a; }
static uint64_t next(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static void fill(uint64_t *s, uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(next(s) >> 24); }
static void hook(const sy_spec *s, sy_region r, uint8_t *p, size_t n) { if (s->hook) s->hook(s->user, r, p, n); }
static void encrypt_key(uint8_t out[16], const uint8_t kek[16], const uint8_t key[16]) {
    tokc_aes a;
    tokc_aes_init(&a, kek, TOKC_AES_AUTO);
    tokc_aes_encrypt(&a, key, out);
}

/* ---- PFS0 and HFS0 ---- */
typedef struct { const char *name; const uint8_t *data; size_t size; } part;

static buf make_part(const sy_spec *s, const char *magic, uint32_t stride, const part *e, int n) {
    size_t strings = 0, table, data = 0;
    for (int i = 0; i < n; i++) { strings += strlen(e[i].name) + 1; data += e[i].size; }
    strings = align(strings, 16);
    table = 16 + (size_t)n * stride + strings;
    buf b = {zalloc(table + data), table + data};
    memcpy(b.p, magic, 4);
    wr32(b.p + 4, (uint32_t)n); wr32(b.p + 8, (uint32_t)strings);
    size_t at = 0, name = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *r = b.p + 16 + (size_t)i * stride;
        wr64(r, at); wr64(r + 8, e[i].size); wr32(r + 16, (uint32_t)name);
        strcpy((char *)b.p + 16 + (size_t)n * stride + name, e[i].name);
        if (e[i].size) memcpy(b.p + table + at, e[i].data, e[i].size);
        name += strlen(e[i].name) + 1; at += e[i].size;
    }
    hook(s, SY_ROOT_TABLE, b.p, table);
    return b;
}

/* ---- the RomFS ---- */
typedef struct { const char *path; uint64_t off, size; } sfile;

static int by_path(const void *a, const void *b) { return strcmp(((const sfile *)a)->path, ((const sfile *)b)->path); }

/* Folder and file records with their sibling and child links, and the expected index. */
static buf make_romfs(sy_game *g, const sy_spec *s, uint64_t *rng) {
    static const struct { const char *name; int parent; } dirs[] = {{"", -1}, {"Actor", 0}, {"Sound", 0}, {"Deep", 1}, {"M\xc3\xbcnze", 3}, {"Zzz", 0}};
    enum { ND = 6 };
    static const char *const names[] = {"alpha.bin", "beta.pack.zs", "gamma.txt", "z.bars", "\xe6\x97\xa5\xe6\x9c\xac.dat"};
    enum { PER_DIR = 5 };
    uint8_t dir_t[1024] = {0}, file_t[8192] = {0};
    uint32_t dir_off[ND], dlen = 0, flen = 0;
    int child[ND], sibling[ND];
    for (int i = 0; i < ND; i++) { child[i] = sibling[i] = -1; }
    for (int i = ND - 1; i >= 1; i--) { sibling[i] = child[dirs[i].parent]; child[dirs[i].parent] = i; }
    for (int i = 0; i < ND; i++) { dir_off[i] = dlen; dlen += 24 + (uint32_t)align(strlen(dirs[i].name), 4); }
    sfile *files = zalloc(sizeof(sfile) * ND * PER_DIR);
    char (*paths)[64] = zalloc(64 * ND * PER_DIR);
    int nf = 0;
    uint64_t data = 0;
    uint32_t first_file[ND];
    for (int d = 0; d < ND; d++) {
        char prefix[64] = "";
        for (int p = d; p > 0; p = dirs[p].parent) {
            char t[64];
            snprintf(t, sizeof t, "%s/%s", dirs[p].name, prefix);
            strcpy(prefix, t);
        }
        first_file[d] = 0xffffffffu;
        int count = 1 + (int)(next(rng) % PER_DIR);
        for (int k = 0; k < count; k++) {
            uint32_t at = flen, n = (uint32_t)strlen(names[k]);
            uint64_t size = k == 0 && d == 1 ? 0 : next(rng) % (d == 2 ? 30000 : 3000);
            uint8_t *f = file_t + at;
            wr32(f, dir_off[d]); wr32(f + 4, k + 1 < count ? at + 32 + (uint32_t)align(n, 4) : 0xffffffffu);
            wr64(f + 8, data); wr64(f + 16, size); wr32(f + 24, 0xffffffffu); wr32(f + 28, n);
            memcpy(f + 32, names[k], n);
            if (!k) first_file[d] = at;
            flen += 32 + (uint32_t)align(n, 4);
            snprintf(paths[nf], 64, "%s%s", prefix, names[k]);
            files[nf].path = paths[nf]; files[nf].off = data; files[nf].size = size; nf++;
            data += align(size, 16);
        }
    }
    for (int i = 0; i < ND; i++) {
        uint8_t *d = dir_t + dir_off[i];
        uint32_t n = (uint32_t)strlen(dirs[i].name);
        wr32(d, dirs[i].parent < 0 ? 0 : dir_off[dirs[i].parent]);
        wr32(d + 4, sibling[i] < 0 ? 0xffffffffu : dir_off[sibling[i]]);
        wr32(d + 8, child[i] < 0 ? 0xffffffffu : dir_off[child[i]]);
        wr32(d + 12, first_file[i]); wr32(d + 16, 0xffffffffu); wr32(d + 20, n);
        memcpy(d + 24, dirs[i].name, n);
    }
    size_t dir_at = 0x50 + 16, file_at = dir_at + dlen + 16, data_at = align(file_at + flen, 16), total = data_at + (size_t)data;
    buf r = {zalloc(total), total};
    uint8_t *h = r.p;
    wr64(h, 0x50); wr64(h + 8, 0x50); wr64(h + 16, 16); wr64(h + 24, dir_at); wr64(h + 32, dlen);
    wr64(h + 40, dir_at + dlen); wr64(h + 48, 16); wr64(h + 56, file_at); wr64(h + 64, flen); wr64(h + 72, data_at);
    memcpy(r.p + dir_at, dir_t, dlen); memcpy(r.p + file_at, file_t, flen);
    fill(rng, r.p + data_at, (size_t)data);
    /* the expected index, before any damage */
    qsort(files, (size_t)nf, sizeof *files, by_path);
    size_t strings = 0;
    for (int i = 0; i < nf; i++) strings += strlen(files[i].path) + 1;
    g->index_size = 24 + (size_t)nf * 24 + strings;
    g->index = zalloc(g->index_size);
    memcpy(g->index, "TOTKRFS1", 8);
    wr64(g->index + 8, (uint64_t)nf); wr64(g->index + 16, 24 + (uint64_t)nf * 24);
    size_t so = 0;
    for (int i = 0; i < nf; i++) {
        uint8_t *row = g->index + 24 + (size_t)i * 24;
        size_t n = strlen(files[i].path) + 1;
        wr64(row, data_at + files[i].off); wr64(row + 8, files[i].size); wr32(row + 16, (uint32_t)so); wr32(row + 20, (uint32_t)n);
        memcpy(g->index + 24 + (size_t)nf * 24 + so, files[i].path, n);
        so += n;
    }
    free(files); free(paths);
    hook(s, SY_ROMFS_HEADER, r.p, 0x50);
    hook(s, SY_ROMFS_DIRS, r.p + dir_at, dlen);
    hook(s, SY_ROMFS_FILES, r.p + file_at, flen);
    return r;
}

/* ---- NCA ---- */
typedef struct {
    uint8_t fs[0x200];
    uint8_t *plain;       /* the section, not encrypted */
    size_t size;
    int encrypted_by_caller;
} section;

static void fs_common(section *c, int fs_type, int hash_type, int crypt, uint64_t *rng) {
    c->fs[0] = 2; c->fs[2] = (uint8_t)fs_type; c->fs[3] = (uint8_t)hash_type; c->fs[4] = (uint8_t)crypt;
    fill(rng, c->fs + 0x140, 8);
}
static void counter_high(uint8_t high[8], const uint8_t *fs) { for (int i = 0; i < 8; i++) high[i] = fs[0x147 - i]; }

static void fs_ivfc(section *c, uint64_t off, uint64_t size) {
    uint8_t *v = c->fs + 8;
    memcpy(v, "IVFC", 4); wr32(v + 4, 0x20000); wr32(v + 8, 0x20); wr32(v + 12, 7);
    wr64(v + 0x10 + 5 * 0x18, off); wr64(v + 0x10 + 5 * 0x18 + 8, size);
}

/* A PFS0 section with SHA-256 blocks: hash table, then the PFS0. */
static void pfs_section(section *c, const sy_spec *s, const uint8_t *pfs, size_t pfs_size, int crypt, uint32_t block, sy_region region, uint64_t *rng) {
    size_t blocks = (pfs_size + block - 1) / block, table = blocks * 32, at = align(table, 0x200);
    c->size = align(at + pfs_size, 0x200);
    c->plain = zalloc(c->size);
    memcpy(c->plain + at, pfs, pfs_size);
    hook(s, region, c->plain + at, pfs_size);
    fs_common(c, 1, 2, crypt, rng);
    for (size_t i = 0; i < blocks; i++) {
        size_t n = pfs_size - i * block < block ? pfs_size - i * block : block;
        tokc_sha256_buf(c->plain + at + i * block, n, c->plain + 32 * i);
    }
    if (region == SY_EXEFS) hook(s, SY_HASH_TABLE, c->plain, table);
    tokc_sha256_buf(c->plain, table, c->fs + 8);
    wr32(c->fs + 0x28, block); wr32(c->fs + 0x2c, 2);
    wr64(c->fs + 0x30, 0); wr64(c->fs + 0x38, table); wr64(c->fs + 0x40, at); wr64(c->fs + 0x48, pfs_size);
}

/* The NCA with its sections encrypted. rights: NULL for a key area. */
static buf make_nca(const sy_spec *s, int content_type, uint64_t title, const uint8_t key[16], const uint8_t *rights, section *c, int n) {
    size_t total = TOKC_NCA_HEADER;
    uint8_t *h = zalloc(TOKC_NCA_HEADER);
    memcpy(h + 0x200, "NCA3", 4);
    h[0x204] = 0; h[0x205] = (uint8_t)content_type; h[0x206] = 2; h[0x220] = GENERATION + 1;
    wr64(h + 0x210, title);
    if (rights) memcpy(h + 0x230, rights, 16);
    else encrypt_key(h + 0x300 + 32, area_key, key);
    for (int i = 0; i < n; i++) {
        wr32(h + 0x240 + 16 * i, (uint32_t)(total / 0x200));
        total += c[i].size;
        wr32(h + 0x244 + 16 * i, (uint32_t)(total / 0x200));
        memcpy(h + 0x400 + 0x200 * i, c[i].fs, 0x200);
    }
    wr64(h + 0x208, total);
    hook(s, SY_NCA_HEADER, h, TOKC_NCA_HEADER);
    for (int i = 0; i < n; i++) tokc_sha256_buf(h + 0x400 + 0x200 * i, 0x200, h + 0x280 + 32 * i);
    buf out = {zalloc(total), total};
    tokc_aes a;
    tokc_aes_init(&a, key, TOKC_AES_AUTO);
    size_t at = TOKC_NCA_HEADER;
    for (int i = 0; i < n; i++) {
        uint8_t high[8];
        counter_high(high, c[i].fs);
        memcpy(out.p + at, c[i].plain, c[i].size);
        if (!c[i].encrypted_by_caller && c[i].fs[4] != 1) tokc_aes_ctr(&a, high, at, out.p + at, c[i].size);
        at += c[i].size;
        free(c[i].plain);
    }
    tokc_nca_xts(header_key, h, TOKC_NCA_HEADER, 1);
    memcpy(out.p, h, TOKC_NCA_HEADER);
    free(h);
    return out;
}

static buf make_meta(const sy_spec *s, uint64_t title, uint64_t other, uint32_t version, int type, const uint8_t key[16], uint64_t *rng) {
    uint8_t cnmt[0x60] = {0};
    wr64(cnmt, title); wr32(cnmt + 8, version); cnmt[0xC] = (uint8_t)type; cnmt[0xE] = 0x10;
    wr64(cnmt + 0x20, other);
    hook(s, SY_CNMT, cnmt, sizeof cnmt);
    part e = {"0123abcd.cnmt", cnmt, sizeof cnmt};
    sy_spec quiet = *s;
    quiet.hook = NULL;   /* a table of its own: damaged through SY_META_PFS */
    buf pfs = make_part(&quiet, "PFS0", 24, &e, 1);
    section c;
    memset(&c, 0, sizeof c);
    pfs_section(&c, s, pfs.p, pfs.n, 3, 0x1000, SY_META_PFS, rng);
    free(pfs.p);
    return make_nca(s, 1, title, key, NULL, &c, 1);
}

static void make_ticket(const sy_spec *s, uint8_t t[0x2c0], const uint8_t rights[16], const uint8_t key[16]) {
    memset(t, 0, 0x2c0);
    wr32(t, 0x10004);
    uint8_t *b = t + 0x140;
    strcpy((char *)b, "Root-CA00000003-XS00000020");
    encrypt_key(b + 0x40, titlekek, key);
    b[0x140] = 2;
    memcpy(b + 0x160, rights, 16);
    hook(s, SY_TICKET, t, 0x2c0);
}

/* Written into a table of one tree level at a time: outer node, then buckets. */
static void write_tree_head(uint8_t *t, uint32_t buckets, uint64_t total) {
    wr32(t + 4, buckets); wr64(t + 8, total);
}

void sy_default(sy_spec *s, uint64_t seed) {
    memset(s, 0, sizeof *s);
    s->seed = seed;
    s->pieces = 40; s->subsections = 12;
    s->base_version = TOKC_VERSION_BASE; s->update_version = TOKC_VERSION_UPDATE;
    s->base_title = TOKC_TITLE_BASE; s->update_title = TOKC_TITLE_UPDATE;
}

void sy_make(sy_game *g, const sy_spec *s) {
    uint64_t rng = s->seed * 0x9E3779B97F4A7C15ull + 88172645463325252ull;
    memset(g, 0, sizeof *g);
    char hexes[3][70];
    static const char x[] = "0123456789abcdef";
    const uint8_t *src[3] = {header_key, titlekek, area_key};
    size_t len[3] = {32, 16, 16};
    for (int i = 0; i < 3; i++) {
        for (size_t k = 0; k < len[i]; k++) { hexes[i][2 * k] = x[src[i][k] >> 4]; hexes[i][2 * k + 1] = x[src[i][k] & 15]; }
        hexes[i][2 * len[i]] = 0;
    }
    g->keys_text = zalloc(400);
    g->keys_size = (size_t)snprintf(g->keys_text, 400, "; made up\nheader_key = %s\ntitlekek_%02x = %s\nKEY_AREA_KEY_APPLICATION_%02X = %s\nother_key = zz\n", hexes[0], GENERATION, hexes[1], GENERATION, hexes[2]);

    /* the patched section: a stretch before the RomFS, as the hash levels are, then the RomFS */
    buf romfs = make_romfs(g, s, &rng);
    size_t lead = 0x800, virt_size = align(lead + romfs.n, 0x200);
    uint8_t *virt = zalloc(virt_size);
    fill(&rng, virt, lead);
    memcpy(virt + lead, romfs.p, romfs.n);
    g->image_size = romfs.n; g->image = romfs.p;

    /* cut it into pieces, each from the base or the update */
    uint32_t pieces = s->pieces ? s->pieces : 1;
    uint64_t *cut = zalloc(sizeof(uint64_t) * (pieces + 1));
    uint8_t *from_update = zalloc(pieces);
    for (uint32_t i = 1; i < pieces; i++) cut[i] = (uint64_t)((double)virt_size * i / pieces) + next(&rng) % 7;
    cut[pieces] = virt_size;
    size_t update_data = 0, base_data = 0;
    for (uint32_t i = 0; i < pieces; i++) {
        from_update[i] = (uint8_t)(next(&rng) & 1);
        if (from_update[i]) update_data += cut[i + 1] - cut[i];
        else base_data += cut[i + 1] - cut[i];
    }
    /* base section: a RomFS of its own to probe, then its pieces, last first so that source offsets differ from image ones */
    size_t base_lead = 0x200, base_size = align(base_lead + base_data, 0x200);
    uint8_t *base = zalloc(base_size);
    wr64(base + 0x100, 0x50);
    uint64_t *phys = zalloc(sizeof(uint64_t) * pieces);
    size_t at = base_lead, up = 0;
    for (uint32_t i = pieces; i-- > 0;) {
        if (from_update[i]) continue;
        size_t n = (size_t)(cut[i + 1] - cut[i]);
        memcpy(base + at, virt + cut[i], n);
        phys[i] = at; at += n;
    }
    size_t tables_at = align(update_data ? update_data : 16, 16);
    uint32_t rel_buckets = (pieces + 817) / 818, sub_count = s->subsections ? s->subsections : 1;
    if (sub_count > tables_at / 2) sub_count = (uint32_t)(tables_at / 2);   /* a little update has room for few */
    uint32_t sub_buckets = (sub_count + 1022) / 1023;
    size_t rel_size = (size_t)NODE * (rel_buckets + 1), sub_size = (size_t)NODE * (sub_buckets + 1);
    size_t section_size = align(tables_at + rel_size + sub_size, 0x200);
    uint8_t *update = zalloc(section_size);
    for (uint32_t i = 0; i < pieces; i++) {
        if (!from_update[i]) continue;
        size_t n = (size_t)(cut[i + 1] - cut[i]);
        memcpy(update + up, virt + cut[i], n);
        phys[i] = up; up += n;
    }
    /* relocation table */
    uint8_t *rel = zalloc(rel_size);
    write_tree_head(rel, rel_buckets, virt_size);
    for (uint32_t b = 0; b < rel_buckets; b++) {
        uint8_t *node = rel + (size_t)NODE * (b + 1);
        uint32_t first = b * 818, count = pieces - first < 818 ? pieces - first : 818;
        wr32(node, b); wr32(node + 4, count); wr64(node + 8, first + count < pieces ? cut[first + count] : virt_size);
        wr64(rel + 16 + 8 * b, cut[first]);
        for (uint32_t k = 0; k < count; k++) {
            uint8_t *e = node + 16 + 0x14 * k;
            wr64(e, cut[first + k]); wr64(e + 8, phys[first + k]); wr32(e + 16, from_update[first + k]);
        }
    }
    /* subsection table: counters for the update's data */
    uint64_t *sub_cut = zalloc(sizeof(uint64_t) * (sub_count + 1));
    uint32_t *sub_ctr = zalloc(sizeof(uint32_t) * sub_count);
    for (uint32_t i = 0; i < sub_count; i++) { sub_cut[i] = (uint64_t)tables_at * i / sub_count; sub_ctr[i] = (uint32_t)next(&rng); }
    /* tiny tables_at: keep the cuts strictly rising */
    for (uint32_t i = 1; i < sub_count; i++) if (sub_cut[i] <= sub_cut[i - 1]) sub_cut[i] = sub_cut[i - 1] + 1;
    sub_cut[sub_count] = tables_at;
    uint8_t *sub = zalloc(sub_size);
    write_tree_head(sub, sub_buckets, tables_at);
    for (uint32_t b = 0; b < sub_buckets; b++) {
        uint8_t *node = sub + (size_t)NODE * (b + 1);
        uint32_t first = b * 1023, count = sub_count - first < 1023 ? sub_count - first : 1023;
        wr32(node, b); wr32(node + 4, count); wr64(node + 8, first + count < sub_count ? sub_cut[first + count] : tables_at);
        wr64(sub + 16 + 8 * b, sub_cut[first]);
        for (uint32_t k = 0; k < count; k++) { uint8_t *e = node + 16 + 16 * k; wr64(e, sub_cut[first + k]); wr32(e + 12, sub_ctr[first + k]); }
    }
    hook(s, SY_BKTR_REL, rel, rel_size);
    hook(s, SY_BKTR_SUB, sub, sub_size);

    /* keys of the two programs, and the shape of each file */
    uint8_t base_key[16], update_key[16], meta_key[16], base_rights[16] = {0}, update_rights[16] = {0};
    fill(&rng, base_key, 16); fill(&rng, update_key, 16); fill(&rng, meta_key, 16);
    wr64(base_rights, s->base_title); wr64(update_rights, s->update_title);
    base_rights[15] = update_rights[15] = GENERATION + 1;

    /* update program: ExeFS and the patched RomFS */
    static const char *const module_names[3] = {"main", "sdk", "subsdk0"};
    static const size_t module_sizes[3] = {20011, 5003, 3001};
    part modules[4];
    for (int i = 0; i < 3; i++) {
        g->module_size[i] = module_sizes[i];
        g->module[i] = zalloc(module_sizes[i]);
        fill(&rng, g->module[i], module_sizes[i]);
        modules[i] = (part){module_names[i], g->module[i], module_sizes[i]};
    }
    uint8_t npdm[100];
    fill(&rng, npdm, sizeof npdm);
    modules[3] = (part){"main.npdm", npdm, sizeof npdm};
    sy_spec quiet = *s;
    quiet.hook = NULL;
    buf exefs = make_part(&quiet, "PFS0", 24, modules, 4);
    section uc[2];
    memset(uc, 0, sizeof uc);
    pfs_section(&uc[0], s, exefs.p, exefs.n, 3, 0x1000, SY_EXEFS, &rng);
    free(exefs.p);
    fs_common(&uc[1], 0, 3, 4, &rng);
    fs_ivfc(&uc[1], lead, romfs.n);
    uint8_t *fs = uc[1].fs;
    wr64(fs + 0x100, tables_at); wr64(fs + 0x108, rel_size); memcpy(fs + 0x110, "BKTR", 4); wr32(fs + 0x114, 1); wr32(fs + 0x118, pieces);
    wr64(fs + 0x120, tables_at + rel_size); wr64(fs + 0x128, sub_size); memcpy(fs + 0x130, "BKTR", 4); wr32(fs + 0x134, 1); wr32(fs + 0x138, sub_count);
    memcpy(update + tables_at, rel, rel_size); memcpy(update + tables_at + rel_size, sub, sub_size);
    /* its place in the NCA is the ExeFS's end: section 1 goes after section 0 */
    size_t update_nca_off = TOKC_NCA_HEADER + uc[0].size;
    {
        tokc_aes a;
        uint8_t high[8];
        tokc_aes_init(&a, update_key, TOKC_AES_AUTO);
        counter_high(high, fs);
        tokc_aes_ctr(&a, high, update_nca_off + tables_at, update + tables_at, section_size - tables_at);
        for (uint32_t i = 0; i < sub_count; i++) {
            uint8_t h2[8];
            memcpy(h2, high, 8);
            h2[4] = (uint8_t)(sub_ctr[i] >> 24); h2[5] = (uint8_t)(sub_ctr[i] >> 16); h2[6] = (uint8_t)(sub_ctr[i] >> 8); h2[7] = (uint8_t)sub_ctr[i];
            tokc_aes_ctr(&a, h2, update_nca_off + sub_cut[i], update + sub_cut[i], (size_t)(sub_cut[i + 1] - sub_cut[i]));
        }
    }
    uc[1].plain = update; uc[1].size = section_size; uc[1].encrypted_by_caller = 1;
    buf update_program = make_nca(s, 0, TOKC_TITLE_BASE, update_key, update_rights, uc, 2);

    /* base program: a plain logo first, as the game has, then the RomFS */
    uint8_t logo_data[64];
    fill(&rng, logo_data, sizeof logo_data);
    part logo_entry = {"logo", logo_data, sizeof logo_data};
    buf logo = make_part(&quiet, "PFS0", 24, &logo_entry, 1);
    section bc[2];
    memset(bc, 0, sizeof bc);
    pfs_section(&bc[0], &quiet, logo.p, logo.n, 1, 0x1000, SY_META_PFS, &rng);
    free(logo.p);
    fs_common(&bc[1], 0, 3, 3, &rng);
    fs_ivfc(&bc[1], 0x100, base_size - 0x100);
    bc[1].plain = base; bc[1].size = base_size;
    buf base_program = make_nca(s, 0, s->base_title == TOKC_TITLE_BASE ? TOKC_TITLE_BASE : s->base_title, base_key, s->xci ? NULL : base_rights, bc, 2);

    buf base_meta = make_meta(s, s->base_title, s->update_title, (uint32_t)s->base_version, 0x80, meta_key, &rng);
    buf update_meta = make_meta(s, s->update_title, s->base_title, (uint32_t)s->update_version, 0x81, meta_key, &rng);

    uint8_t tik[2][0x2c0], cert[0x300] = {0};
    make_ticket(s, tik[0], base_rights, base_key);
    make_ticket(s, tik[1], update_rights, update_key);
    wr32(cert, 0x10000); strcpy((char *)cert + 0x240, "Root-CA00000003");   /* looks like a ticket until its issuer */
    static const uint8_t xml[] = "<?xml version=\"1.0\"?><ContentMeta></ContentMeta>";

    part ue[5] = {{"0a.cert", cert, sizeof cert}, {"0b.tik", tik[1], 0x2c0}, {"0c.nca", update_program.p, update_program.n}, {"0d.cnmt.nca", update_meta.p, update_meta.n}, {"0e.cnmt.xml", xml, sizeof xml}};
    buf un = make_part(s, "PFS0", 24, ue, 5);
    g->update.data = un.p; g->update.size = un.n;
    if (s->xci) {
        part se[2] = {{"aa.nca", base_program.p, base_program.n}, {"bb.nca", base_meta.p, base_meta.n}};
        buf secure = make_part(s, "HFS0", 64, se, 2);
        buf empty = make_part(&quiet, "HFS0", 64, NULL, 0);
        part re[3] = {{"update", empty.p, empty.n}, {"normal", empty.p, empty.n}, {"secure", secure.p, secure.n}};
        buf root = make_part(s, "HFS0", 64, re, 3);
        size_t head = 0x200;
        g->base.size = head + root.n;
        g->base.data = zalloc(g->base.size);
        memcpy(g->base.data + 0x100, "HEAD", 4);
        wr64(g->base.data + 0x130, head);
        memcpy(g->base.data + head, root.p, root.n);
        free(secure.p); free(empty.p); free(root.p);
    } else {
        part be[5] = {{"1a.cert", cert, sizeof cert}, {"1b.tik", tik[0], 0x2c0}, {"1c.nca", base_program.p, base_program.n}, {"1d.cnmt.nca", base_meta.p, base_meta.n}, {"1e.cnmt.xml", xml, sizeof xml}};
        buf bn = make_part(s, "PFS0", 24, be, 5);
        g->base.data = bn.p; g->base.size = bn.n;
    }
    free(update_program.p); free(base_program.p); free(base_meta.p); free(update_meta.p);
    free(virt); free(cut); free(from_update); free(phys); free(rel); free(sub); free(sub_cut); free(sub_ctr);
}

void sy_free(sy_game *g) {
    free(g->base.data); free(g->update.data); free(g->keys_text); free(g->image); free(g->index);
    for (int i = 0; i < 3; i++) free(g->module[i]);
    memset(g, 0, sizeof *g);
}

int sy_fd(const void *data, size_t size) {
    int fd = memfd_create("synth", MFD_CLOEXEC);
    if (fd < 0) abort();
    const uint8_t *p = data;
    while (size) {
        ssize_t w = write(fd, p, size);
        if (w <= 0) abort();
        p += w; size -= (size_t)w;
    }
    return fd;
}
