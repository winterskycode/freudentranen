/* mod-base.bin from the player's files: tools/make_mod_base.py in C, byte for byte (regress.sh). */
#include "modbase.h"
#include "container_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <zstd.h>

#define LEVEL 19               /* of the blobs, as the Python: what Java reads does not depend on it */
#define MAX_FILE (1ull << 30)  /* no pack is near this: a size above it is damage */
#define STEP (1u << 20)
#define MAX_DICTS 8

typedef struct { const char *path; uint64_t off, size; } file_row;
typedef struct { file_row *rows; uint64_t count; uint8_t *bytes; } index_table;
typedef struct { uint8_t key[8], hash[8]; uint8_t *blob; size_t blob_size; int used; } out_row;
typedef struct { const char *name; size_t name_size; const uint8_t *body; uint64_t size; } member;
typedef struct { const file_row *a, *b; } pair;
typedef struct {
    const tokc_game *g;
    ZSTD_DDict *dicts[MAX_DICTS]; unsigned dict_ids[MAX_DICTS], ndicts;
    pair *pairs; uint64_t npairs;
    out_row *rows;                           /* one slot per pair, used when the file differs */
    _Atomic uint64_t next, done, read_files, read_bytes;
    _Atomic int stop;
    tokm_progress progress; void *ctx;
    pthread_mutex_t lock; int rc; char err[300];
} job;
static tokm_stats last_stats;

static void h8(const void *data, size_t n, uint8_t out[8]) { uint8_t d[32]; tokc_sha256_buf(data, n, d); memcpy(out, d, 8); }
static int fail(job *j, int code, const char *what, const char *path) {
    pthread_mutex_lock(&j->lock);
    if (!j->rc) { j->rc = code; snprintf(j->err, sizeof j->err, "%s: %s", path, what); }
    pthread_mutex_unlock(&j->lock);
    atomic_store(&j->stop, 1);
    return code;
}

/* ---- what a file holds ---- */
static int parse_index(uint8_t *bytes, size_t size, index_table *t) {
    if (size < 24 || memcmp(bytes, "TOTKRFS1", 8)) return TOKC_E_FORMAT;
    uint64_t n = rd64(bytes + 8), strings = rd64(bytes + 16);
    if (n > (size - 24) / 24 || strings != 24 + n * 24) return TOKC_E_FORMAT;
    file_row *rows = malloc((size_t)(n ? n : 1) * sizeof *rows);
    if (!rows) return TOKC_E_NOMEM;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *r = bytes + 24 + i * 24;
        rows[i] = (file_row){(const char *)bytes + strings + rd32(r + 16), rd64(r), rd64(r + 8)};
    }
    *t = (index_table){rows, n, bytes};
    return TOKC_OK;
}
static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}
/* The members of a SARC as the Python's sarc_read gives them; *out is malloc'd and points into data */
static int sarc_parse(const uint8_t *d, uint64_t n, member **out, uint32_t *count) {
    *out = NULL; *count = 0;
    if (n < 0x14 || memcmp(d, "SARC", 4) || d[6] != 0xff || d[7] != 0xfe) return TOKC_E_FORMAT;
    uint64_t head = rd16(d + 4), start = rd32(d + 12);
    if (start > n || !fits(head, 12, n)) return TOKC_E_FORMAT;
    uint32_t c = rd16(d + head + 6);
    uint64_t names = head + 12 + (uint64_t)c * 16 + 8;
    if (names > start) return TOKC_E_FORMAT;
    member *m = malloc((c ? c : 1) * sizeof *m);
    if (!m) return TOKC_E_NOMEM;
    for (uint32_t i = 0; i < c; i++) {
        const uint8_t *e = d + head + 12 + 16ull * i;
        uint64_t at = names + (uint64_t)(rd32(e + 4) & 0xffffff) * 4, lo = rd32(e + 8), hi = rd32(e + 12);
        const uint8_t *end = at < start ? memchr(d + at, 0, (size_t)(start - at)) : NULL;
        if (!end || lo > hi || !fits(start, hi, n)) { free(m); return TOKC_E_FORMAT; }
        m[i] = (member){(const char *)d + at, (size_t)(end - (d + at)), d + start + lo, hi - lo};
        if (m[i].name_size > 0xffff) { free(m); return TOKC_E_FORMAT; }
    }
    *out = m; *count = c;
    return TOKC_OK;
}
static int by_name(const void *a, const void *b) { return strcmp(((const member *)a)->name, ((const member *)b)->name); }

/* A .zs file decompressed; NULL when it does not. No dictionary set (make_dicts): only frames that need none. */
static uint8_t *unzs(const job *j, const uint8_t *data, uint64_t size, uint64_t *out) {
    unsigned long long m = ZSTD_getFrameContentSize(data, size);
    if (m == ZSTD_CONTENTSIZE_UNKNOWN || m == ZSTD_CONTENTSIZE_ERROR || m > MAX_FILE) return NULL;
    unsigned id = ZSTD_getDictID_fromFrame(data, size);
    const ZSTD_DDict *dict = NULL;
    for (unsigned i = 0; j && i < j->ndicts; i++) if (j->dict_ids[i] == id) dict = j->dicts[i];
    if (id && !dict) return NULL;
    uint8_t *b = malloc(m ? (size_t)m : 1);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t r = !b || !d ? 1 : dict ? ZSTD_decompress_usingDDict(d, b, (size_t)m, data, size, dict) : ZSTD_decompressDCtx(d, b, (size_t)m, data, size);
    ZSTD_freeDCtx(d);
    if (!b || !d || ZSTD_isError(r) || r != m) { free(b); return NULL; }
    *out = m;
    return b;
}
typedef struct { uint8_t *p; size_t n, cap; } buffer;
static int put(buffer *b, const void *data, size_t n) {
    if (n > b->cap - b->n) {
        size_t cap = b->cap ? b->cap : 1u << 16;
        while (cap - b->n < n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) return TOKC_E_NOMEM;
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, data, n); b->n += n;
    return TOKC_OK;
}
/* What the Python's work() makes of a pack, from the two decompressed archives */
int tokm_pack_blob(const uint8_t *mine, uint64_t mine_size, const uint8_t *theirs, uint64_t theirs_size, uint8_t **blob, size_t *blob_size, char *err, size_t errlen) {
    member *a = NULL, *b = NULL; uint32_t na, nb;
    buffer out = {0}; uint8_t *z = NULL; size_t zn = 0;
    int rc = sarc_parse(mine, mine_size, &a, &na);
    if (!rc) rc = sarc_parse(theirs, theirs_size, &b, &nb);
    if (rc) { tokc_fail(err, errlen, rc, "a pack is not a SARC"); goto done; }
    if (nb) qsort(b, nb, sizeof *b, by_name);
    uint8_t head[4]; wr32(head, na);
    rc = put(&out, head, 4);
    for (uint32_t i = 0; i < na && !rc; i++) {
        member key = a[i]; const member *same = nb ? bsearch(&key, b, nb, sizeof *b, by_name) : NULL;
        int keep = a[i].size >= 2 && a[i].body[0] == 'Y' && a[i].body[1] == 'B' && !(same && same->size == a[i].size && !memcmp(same->body, a[i].body, (size_t)a[i].size));
        uint8_t n2[2] = {(uint8_t)a[i].name_size, (uint8_t)(a[i].name_size >> 8)}, hash[8], size4[4];
        if (a[i].size > 0xffffffffu) { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, "a pack member is too large"); break; }
        h8(a[i].body, (size_t)a[i].size, hash); wr32(size4, keep ? (uint32_t)a[i].size : 0);
        if (!(rc = put(&out, n2, 2)) && !(rc = put(&out, a[i].name, a[i].name_size)) && !(rc = put(&out, hash, 8)) && !(rc = put(&out, size4, 4)) && keep) rc = put(&out, a[i].body, (size_t)a[i].size);
    }
    if (!rc) {
        size_t bound = ZSTD_compressBound(out.n);
        z = malloc(bound);
        if (!z) rc = TOKC_E_NOMEM;
        else if (ZSTD_isError(zn = ZSTD_compress(z, bound, out.p, out.n, LEVEL))) rc = TOKC_E_FORMAT;
        if (rc) tokc_fail(err, errlen, rc, "a pack does not compress");
    }
    if (!rc) { *blob = z; *blob_size = zn; z = NULL; }
done:
    free(a); free(b); free(out.p); free(z);
    return rc;
}

/* ---- reading ---- */
static int read_new(const job *j, const file_row *r, uint8_t *p, uint64_t n, uint64_t at) { return tokc_romfs_pread(j->g, p, (size_t)n, r->off + at) == (int64_t)n ? TOKC_OK : TOKC_E_IO; }
static int read_old(const job *j, const file_row *r, uint8_t *p, uint64_t n, uint64_t at) { return tokc_base_romfs_pread(j->g, p, (size_t)n, r->off + at) == (int64_t)n ? TOKC_OK : TOKC_E_IO; }
static int make_dicts(job *j, const index_table *t) {
    static const char path[] = "Pack/ZsDic.pack.zs";
    uint64_t lo = 0, hi = t->count;
    while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; if (strcmp(t->rows[m].path, path) < 0) lo = m + 1; else hi = m; }
    if (lo == t->count || strcmp(t->rows[lo].path, path) || t->rows[lo].size > MAX_FILE) return fail(j, TOKC_E_FORMAT, "missing", path);
    const file_row *r = &t->rows[lo];
    uint8_t *raw = malloc((size_t)r->size + 1), *sarc = NULL; uint64_t n = 0; member *m = NULL; uint32_t c = 0; int rc = TOKC_OK;
    if (!raw) return fail(j, TOKC_E_NOMEM, "out of memory", path);
    if ((rc = read_new(j, r, raw, r->size, 0))) fail(j, rc, "cannot be read", path);
    else if (!(sarc = unzs(NULL, raw, r->size, &n)) || sarc_parse(sarc, n, &m, &c)) rc = fail(j, TOKC_E_FORMAT, "is malformed", path);
    for (uint32_t i = 0; !rc && i < c && j->ndicts < MAX_DICTS; i++) {
        ZSTD_DDict *d = ZSTD_createDDict(m[i].body, (size_t)m[i].size);
        if (!d) { rc = fail(j, TOKC_E_FORMAT, "holds a bad dictionary", path); break; }
        j->dicts[j->ndicts] = d; j->dict_ids[j->ndicts++] = ZSTD_getDictID_fromDict(m[i].body, (size_t)m[i].size);
    }
    free(m); free(sarc); free(raw);
    return rc;
}
/* A file both have, when its bytes differ: its row. The base file's hash is made as it is compared. */
static int compare_file(job *j, const pair *p, out_row *row) {
    const file_row *a = p->a, *b = p->b;
    atomic_fetch_add(&j->read_files, 1);
    int differ = a->size != b->size, rc = TOKC_OK;
    uint8_t *x = malloc(STEP), *y = malloc(STEP), digest[32];
    if (!x || !y) { free(x); free(y); return fail(j, TOKC_E_NOMEM, "out of memory", a->path); }
    tokc_sha256 s; tokc_sha256_init(&s);
    for (uint64_t at = 0; at < a->size && !rc; at += STEP) {
        uint64_t n = a->size - at < STEP ? a->size - at : STEP;
        if ((rc = read_old(j, a, x, n, at))) break;
        atomic_fetch_add(&j->read_bytes, n);
        tokc_sha256_update(&s, x, (size_t)n);
        if (differ) continue;
        if ((rc = read_new(j, b, y, n, at))) break;
        atomic_fetch_add(&j->read_bytes, n);
        differ = memcmp(x, y, (size_t)n) != 0;
    }
    free(x); free(y);
    if (rc) return fail(j, rc, "cannot be read", a->path);
    if (!differ) return TOKC_OK;
    tokc_sha256_final(&s, digest);
    memcpy(row->hash, digest, 8);
    h8(a->path, strlen(a->path), row->key);
    row->used = 1;
    return TOKC_OK;
}
static int make_pack(job *j, const pair *p, out_row *row) {
    const file_row *a = p->a, *b = p->b;
    char err[200];
    if (a->size > MAX_FILE || b->size > MAX_FILE) return fail(j, TOKC_E_FORMAT, "is too large", a->path);
    uint8_t *x = malloc((size_t)a->size + 1), *y = malloc((size_t)b->size + 1), *mine = NULL, *theirs = NULL; uint64_t mn = 0, tn = 0;
    int rc = !x || !y ? TOKC_E_NOMEM : TOKC_OK;
    if (rc) fail(j, rc, "out of memory", a->path);
    else if ((rc = read_old(j, a, x, a->size, 0)) || (rc = read_new(j, b, y, b->size, 0))) fail(j, rc, "cannot be read", a->path);
    else {
        atomic_fetch_add(&j->read_files, 1); atomic_fetch_add(&j->read_bytes, a->size + b->size);
        if (a->size != b->size || memcmp(x, y, (size_t)a->size)) {
            h8(x, (size_t)a->size, row->hash);
            h8(a->path, strlen(a->path), row->key);
            row->used = 1;
            if (!(mine = unzs(j, x, a->size, &mn)) || !(theirs = unzs(j, y, b->size, &tn))) rc = fail(j, TOKC_E_FORMAT, "does not decompress", a->path);
            else if ((rc = tokm_pack_blob(mine, mn, theirs, tn, &row->blob, &row->blob_size, err, sizeof err))) fail(j, rc, err, a->path);
        }
    }
    free(x); free(y); free(mine); free(theirs);
    return rc;
}
static void *worker(void *arg) {
    job *j = arg;
    while (!atomic_load(&j->stop)) {
        uint64_t i = atomic_fetch_add(&j->next, 1);
        if (i >= j->npairs) break;
        const pair *p = &j->pairs[i];
        if (p->a->size != p->b->size || !tokc_romfs_same_as_base(j->g, p->b->off, p->a->off, p->a->size)) {
            if (ends_with(p->a->path, ".pack.zs")) make_pack(j, p, &j->rows[i]);
            else compare_file(j, p, &j->rows[i]);
        }
        uint64_t done = atomic_fetch_add(&j->done, 1) + 1;
        if (j->progress && !(done % 4096)) { pthread_mutex_lock(&j->lock); j->progress(j->ctx, done, j->npairs); pthread_mutex_unlock(&j->lock); }
    }
    return NULL;
}
static int by_key(const void *a, const void *b) { return memcmp(((const out_row *)a)->key, ((const out_row *)b)->key, 8); }

int tokm_make(const tokc_game *g, int threads, tokm_progress progress, void *ctx, uint8_t **bytes, size_t *size, char *err, size_t errlen) {
    if (!g || !bytes || !size) return tokc_fail(err, errlen, TOKC_E_ARGS, "no output");
    *bytes = NULL; *size = 0;
    uint8_t *nb = NULL, *ob = NULL; size_t nn, on;
    index_table now = {0}, old = {0};
    job j = {.g = g, .progress = progress, .ctx = ctx, .lock = PTHREAD_MUTEX_INITIALIZER};
    int rc = tokc_romfs_index(g, &nb, &nn, err, errlen);
    if (rc) return rc;
    if ((rc = tokc_base_romfs_index(g, &ob, &on, err, errlen))) { free(nb); return rc; }
    if ((rc = parse_index(nb, nn, &now)) || (rc = parse_index(ob, on, &old))) { tokc_fail(err, errlen, rc, "a RomFS index does not hold together"); goto out; }
    if ((rc = make_dicts(&j, &now))) { tokc_fail(err, errlen, rc, "%s", j.err); goto out; }
    uint64_t most = old.count < now.count ? old.count : now.count;
    j.pairs = malloc((size_t)(most ? most : 1) * sizeof *j.pairs);
    if (!j.pairs) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    for (uint64_t a = 0, b = 0; a < old.count && b < now.count;) {   /* both sorted by path */
        int c = strcmp(old.rows[a].path, now.rows[b].path);
        if (!c) j.pairs[j.npairs++] = (pair){&old.rows[a], &now.rows[b]};
        a += c <= 0; b += c >= 0;
    }
    j.rows = calloc((size_t)(j.npairs ? j.npairs : 1), sizeof *j.rows);
    if (!j.rows) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (threads <= 0) threads = cpus > 4 ? 4 : cpus > 0 ? (int)cpus : 1;
    if (threads > 32) threads = 32;
    pthread_t t[32]; int started = 0;
    for (int i = 0; i < threads; i++) if (!pthread_create(&t[started], NULL, worker, &j)) started++;
    if (!started) worker(&j);
    for (int i = 0; i < started; i++) pthread_join(t[i], NULL);
    if (j.rc) { rc = tokc_fail(err, errlen, j.rc, "%s", j.err); goto out; }
    /* the rows, sorted, in the file's order */
    uint64_t rows = 0, packs = 0, blobs = 0;
    for (uint64_t i = 0; i < j.npairs; i++) {
        if (!j.rows[i].used) continue;
        out_row r = j.rows[i];
        j.rows[i] = (out_row){{0}, {0}, NULL, 0, 0};
        j.rows[rows++] = r;
    }
    qsort(j.rows, (size_t)rows, sizeof *j.rows, by_key);
    for (uint64_t i = 0; i < rows; i++) if (j.rows[i].blob) { packs++; blobs += j.rows[i].blob_size; }
    uint64_t head = 32, packs_off = head + 16 * rows, blob_base = packs_off + 24 * packs, total = blob_base + blobs, at = blob_base;
    uint8_t *o = malloc((size_t)total);
    if (!o) { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; }
    memcpy(o, "TKMODB01", 8); wr32(o + 8, (uint32_t)rows); wr32(o + 12, (uint32_t)packs); wr64(o + 16, head); wr64(o + 24, packs_off);
    uint8_t *pack = o + packs_off;
    for (uint64_t i = 0; i < rows; i++) {
        memcpy(o + head + 16 * i, j.rows[i].key, 8); memcpy(o + head + 16 * i + 8, j.rows[i].hash, 8);
        if (!j.rows[i].blob) continue;
        memcpy(pack, j.rows[i].key, 8); wr64(pack + 8, at); wr32(pack + 16, (uint32_t)j.rows[i].blob_size); wr32(pack + 20, 0);
        memcpy(o + at, j.rows[i].blob, j.rows[i].blob_size);
        pack += 24; at += j.rows[i].blob_size;
    }
    *bytes = o; *size = (size_t)total;
    last_stats = (tokm_stats){j.npairs, atomic_load(&j.read_files), atomic_load(&j.read_bytes), rows, packs};
    if (progress) progress(ctx, j.npairs, j.npairs);
out:
    for (uint64_t i = 0; j.rows && i < j.npairs; i++) free(j.rows[i].blob);
    for (unsigned i = 0; i < j.ndicts; i++) ZSTD_freeDDict(j.dicts[i]);
    free(j.rows); free(j.pairs); free(now.rows); free(old.rows); free(nb); free(ob);
    return rc;
}
void tokm_last_stats(tokm_stats *out) { *out = last_stats; }
