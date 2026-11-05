/* runtime-index.bin from the RomFS's own tables: the walk of MacNativeGames' scripts/extract_romfs.py index_romfs and the file
   of scripts/prepare_romfs_backend.py (MIT; see NOTICE): '<8sQQ' magic, file count, offset of the strings; a '<QQII' row
   per file in path order: image offset, size, string offset, string length with its NUL; then the paths. */
#include "container_internal.h"
#include <stdlib.h>

#define MAX_TABLE (256u << 20)
/* the runtime refuses an index of 128 MB */
#define MAX_STRINGS (120u << 20)
/* the runtime also refuses paths that do not fit its buffers */
#define MAX_PATH 4096u
#define NONE 0xffffffffu

typedef struct { const char *path; uint64_t offset, size; size_t arena_off; } row;
typedef struct { uint32_t off, parent; } todo;

static int by_path(const void *a, const void *b) { return strcmp(((const row *)a)->path, ((const row *)b)->path); }

static int utf8_ok(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t k = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!k || k > n - i) return 0;
        uint32_t v = k == 1 ? c : c & (k == 2 ? 0x1f : k == 3 ? 0x0f : 0x07);
        for (size_t j = 1; j < k; j++) {
            if ((s[i + j] & 0xc0) != 0x80) return 0;
            v = v << 6 | (s[i + j] & 0x3f);
        }
        if ((k == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (k == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
        i += k;
    }
    return 1;
}

/* The checks of the Python's name(): in bounds, UTF-8, not . or .., no / \ or NUL. */
static int name_ok(const uint8_t *table, uint64_t len, uint64_t off, uint32_t n) {
    if (!fits(off, n, len)) return 0;
    const uint8_t *s = table + off;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (uint32_t i = 0; i < n; i++) if (s[i] == '/' || s[i] == '\\' || !s[i]) return 0;
    return utf8_ok(s, n);
}

typedef int64_t (*image_read)(const tokc_game *g, void *buf, size_t n, uint64_t off);

static int read_table(const tokc_game *g, image_read pread, uint8_t **out, uint64_t off, uint64_t size) {
    *out = malloc(size ? (size_t)size : 1);
    if (!*out) return TOKC_E_NOMEM;
    int64_t got = size ? pread(g, *out, (size_t)size, off) : 0;
    return got < 0 ? (int)got : (uint64_t)got == size ? TOKC_OK : TOKC_E_IO;
}

static int make_index(const tokc_game *g, image_read pread, uint64_t length, uint8_t **bytes, size_t *size, char *err, size_t errlen) {
    if (!g || !bytes || !size) return tokc_fail(err, errlen, TOKC_E_ARGS, "no output");
    *bytes = NULL; *size = 0;
    uint8_t h[0x50], *dirs = NULL, *files = NULL, *seen_d = NULL, *seen_f = NULL, *out = NULL;
    char *arena = NULL, path[MAX_PATH];
    size_t arena_used = 0, arena_cap = 0, nrows = 0, cap_rows = 0, ntodo = 0;
    row *rows = NULL;
    todo *stack = NULL;
    int rc = TOKC_OK;
#define FAIL(...) do { rc = tokc_fail(err, errlen, TOKC_E_FORMAT, __VA_ARGS__); goto out; } while (0)
#define OOM() do { rc = tokc_fail(err, errlen, TOKC_E_NOMEM, "out of memory"); goto out; } while (0)
    if (pread(g, h, sizeof h, 0) != (int64_t)sizeof h) { rc = tokc_fail(err, errlen, TOKC_E_IO, "cannot read the RomFS header"); goto out; }
    uint64_t hv[10];
    for (int i = 0; i < 10; i++) hv[i] = rd64(h + 8 * i);
    if (hv[0] != 0x50) FAIL("the RomFS header is malformed");
    for (int i = 1; i < 9; i += 2) if (!fits(hv[i], hv[i + 1], length)) FAIL("a RomFS table is out of bounds");
    uint64_t dlen = hv[4], flen = hv[8], data = hv[9];
    if (dlen > MAX_TABLE || flen > MAX_TABLE) FAIL("the RomFS tables are too large");
    if (data > length) FAIL("the RomFS data is out of bounds");
    if ((rc = read_table(g, pread, &dirs, hv[3], dlen)) || (rc = read_table(g, pread, &files, hv[7], flen))) { rc = tokc_fail(err, errlen, rc, "cannot read the RomFS tables"); goto out; }
    seen_d = calloc((size_t)dlen / 8 + 1, 1); seen_f = calloc((size_t)flen / 8 + 1, 1);
    /* a folder is taken from the stack once and adds at most two: it never holds more than one entry per folder, plus one */
    stack = malloc(((size_t)dlen / 24 + 2) * sizeof *stack);
    if (!seen_d || !seen_f || !stack) OOM();
    /* one path buffer: a folder's children are taken before its siblings, so the path below a todo entry's parent is intact */
    stack[ntodo++] = (todo){0, 0};
    while (ntodo) {
        todo t = stack[--ntodo];
        if (t.off >= dlen || dlen - t.off < 24 || seen_d[t.off / 8] >> (t.off % 8) & 1) FAIL("the RomFS has a bad or cyclic folder entry");
        seen_d[t.off / 8] |= (uint8_t)(1u << (t.off % 8));
        const uint8_t *d = dirs + t.off;
        uint32_t sibling = rd32(d + 4), child = rd32(d + 8), file = rd32(d + 12), n = rd32(d + 20), prefix = t.parent;
        if (!name_ok(dirs, dlen, (uint64_t)t.off + 24, n)) FAIL("the RomFS has an unsafe folder name");
        if (n) {
            if ((uint64_t)prefix + n + 1 >= MAX_PATH) FAIL("a RomFS path is too long");
            memcpy(path + prefix, d + 24, n);
            prefix += n;
            path[prefix++] = '/';
        }
        if (sibling != NONE) stack[ntodo++] = (todo){sibling, t.parent};
        if (child != NONE) stack[ntodo++] = (todo){child, prefix};
        while (file != NONE) {
            if (file >= flen || flen - file < 32 || seen_f[file / 8] >> (file % 8) & 1) FAIL("the RomFS has a bad or cyclic file entry");
            seen_f[file / 8] |= (uint8_t)(1u << (file % 8));
            const uint8_t *f = files + file;
            uint32_t next = rd32(f + 4), fn = rd32(f + 28);
            uint64_t off = rd64(f + 8), fsize = rd64(f + 16);
            if (!fn || !name_ok(files, flen, (uint64_t)file + 32, fn)) FAIL("the RomFS has an unsafe file name");
            if (!fits(data, off, length) || !fits(data + off, fsize, length)) FAIL("a RomFS file is out of bounds");
            if ((uint64_t)prefix + fn + 1 > MAX_PATH) FAIL("a RomFS path is too long");
            size_t need = prefix + (size_t)fn + 1;
            if (arena_used + need > MAX_STRINGS) FAIL("the RomFS has too many files");
            if (arena_used + need > arena_cap) {
                size_t cap = arena_cap ? arena_cap * 2 : 1u << 20;
                while (cap < arena_used + need) cap *= 2;
                char *p = realloc(arena, cap);
                if (!p) OOM();
                arena = p; arena_cap = cap;
            }
            if (nrows == cap_rows) {
                row *p = realloc(rows, (cap_rows = cap_rows ? cap_rows * 2 : 4096) * sizeof *p);
                if (!p) OOM();
                rows = p;
            }
            memcpy(arena + arena_used, path, prefix);
            memcpy(arena + arena_used + prefix, f + 32, fn);
            arena[arena_used + need - 1] = 0;
            rows[nrows++] = (row){NULL, data + off, fsize, arena_used};
            arena_used += need;
            file = next;
        }
    }
    for (size_t i = 0; i < nrows; i++) rows[i].path = arena + rows[i].arena_off;
    if (nrows) qsort(rows, nrows, sizeof *rows, by_path);
    for (size_t i = 1; i < nrows; i++) if (!strcmp(rows[i - 1].path, rows[i].path)) FAIL("the RomFS lists a file twice");
    uint64_t strings = 24 + (uint64_t)nrows * 24, total = strings + arena_used;
    if (!(out = malloc((size_t)total))) OOM();
    memcpy(out, "TOTKRFS1", 8);
    wr64(out + 8, nrows); wr64(out + 16, strings);
    size_t at = 0;
    for (size_t i = 0; i < nrows; i++) {
        uint8_t *r = out + 24 + i * 24;
        size_t n = strlen(rows[i].path) + 1;
        wr64(r, rows[i].offset); wr64(r + 8, rows[i].size); wr32(r + 16, (uint32_t)at); wr32(r + 20, (uint32_t)n);
        memcpy(out + strings + at, rows[i].path, n);
        at += n;
    }
    *bytes = out; *size = (size_t)total; out = NULL;
out:
#undef FAIL
#undef OOM
    free(dirs); free(files); free(seen_d); free(seen_f); free(arena); free(rows); free(stack); free(out);
    return rc;
}

int tokc_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen) {
    return make_index(g, tokc_romfs_pread, tokc_romfs_size(g), bytes, size, err, errlen);
}
int tokc_base_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen) {
    return make_index(g, tokc_base_romfs_pread, tokc_base_romfs_size(g), bytes, size, err, errlen);
}
