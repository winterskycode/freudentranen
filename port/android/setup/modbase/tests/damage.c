/* Damaged game files through tokm_make, for ASan and UBSan. Small real packs are read once from the player's files, then each round
   serves them from memory as a made-up pair of RomFS images, damaged before and after compression.
     damage <prod.keys> <base> <update> <seed> <rounds>   */
#define tokc_romfs_index fake_index
#define tokc_base_romfs_index fake_base_index
#define tokc_romfs_pread fake_pread
#define tokc_base_romfs_pread fake_base_pread
#define tokc_romfs_same_as_base fake_same
#include "../modbase.c"
#undef tokc_romfs_index
#undef tokc_base_romfs_index
#undef tokc_romfs_pread
#undef tokc_base_romfs_pread
#undef tokc_romfs_same_as_base
#include <fcntl.h>
/* the real ones, hidden above: the samples come from them */
int tokc_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen);
int tokc_base_romfs_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen);
int64_t tokc_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off);
int64_t tokc_base_romfs_pread(const tokc_game *g, void *buf, size_t n, uint64_t off);
int tokc_romfs_same_as_base(const tokc_game *g, uint64_t off, uint64_t base_off, uint64_t n);

enum { MAX_FILES = 40, MAX_PACK = 96 << 10 };
typedef struct { char path[200]; uint8_t *old_bytes, *new_bytes; size_t old_size, new_size; uint8_t *old_plain, *new_plain; uint64_t old_plain_size, new_plain_size; } sample;
static sample files[MAX_FILES]; static unsigned nfiles;
static uint8_t *old_image, *new_image, *dict_pack; static uint64_t old_size, new_size; static size_t dict_size;
static uint8_t *old_index, *new_index; static size_t old_index_size, new_index_size;

int64_t fake_pread(const tokc_game *g, void *buf, size_t n, uint64_t off) { (void)g; if (off >= new_size) return 0; if (n > new_size - off) n = (size_t)(new_size - off); memcpy(buf, new_image + off, n); return (int64_t)n; }
int64_t fake_base_pread(const tokc_game *g, void *buf, size_t n, uint64_t off) { (void)g; if (off >= old_size) return 0; if (n > old_size - off) n = (size_t)(old_size - off); memcpy(buf, old_image + off, n); return (int64_t)n; }
int fake_same(const tokc_game *g, uint64_t off, uint64_t base_off, uint64_t n) { (void)g; (void)off; (void)base_off; (void)n; return 0; }
static int copy_out(const uint8_t *src, size_t n, uint8_t **bytes, size_t *size) { *bytes = malloc(n); memcpy(*bytes, src, n); *size = n; return 0; }
int fake_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen) { (void)g; (void)err; (void)errlen; return copy_out(new_index, new_index_size, bytes, size); }
int fake_base_index(const tokc_game *g, uint8_t **bytes, size_t *size, char *err, size_t errlen) { (void)g; (void)err; (void)errlen; return copy_out(old_index, old_index_size, bytes, size); }

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint8_t *damaged(const uint8_t *src, uint64_t n, uint64_t *out) {
    static const uint8_t hot[] = {0x00, 0x01, 0x02, 0x7f, 0x80, 0xff};
    uint64_t m = rnd() % 8 ? n : rnd() % (n + 1);
    uint8_t *b = malloc(m ? m : 1); memcpy(b, src, m);
    for (unsigned k = rnd() % 9; m && k; k--) { uint64_t at = rnd() % 4 ? rnd() % m : rnd() % (m < 256 ? m : 256); b[at] = rnd() % 2 ? hot[rnd() % sizeof hot] : (uint8_t)rnd(); }
    *out = m; return b;
}
/* An image of the files' bytes, one damaged way or another, and its index */
static void add(uint8_t **image, uint64_t *size, uint8_t **row, const char *path, const uint8_t *b, uint64_t n, uint64_t name) {
    *image = realloc(*image, *size + n + 1); memcpy(*image + *size, b, n);
    wr64(*row, *size); wr64(*row + 8, n); wr32(*row + 16, (uint32_t)name); wr32(*row + 20, (uint32_t)strlen(path) + 1); *row += 24;
    *size += n;
}
static unsigned victim;   /* one damaged file a round: the first that fails stops the rest */
static void one_side(uint8_t **image, uint64_t *size, uint8_t **index, size_t *index_size, int is_new) {
    uint64_t names = strlen("Pack/ZsDic.pack.zs") + 1; for (unsigned i = 0; i < nfiles; i++) names += strlen(files[i].path) + 1;
    uint64_t count = nfiles + 1, strings = 24 + 24 * count;
    uint8_t *ix = calloc(1, strings + names), *row = ix + 24; memcpy(ix, "TOTKRFS1", 8); wr64(ix + 8, count); wr64(ix + 16, strings);
    free(*image); *image = NULL; *size = 0; uint64_t at = 0;
    for (unsigned i = 0; i <= nfiles; i++) {   /* the paths sort with the dictionaries' after Actor/... and before the rest only by chance: sorted at load */
        const char *path = i < nfiles ? files[i].path : "Pack/ZsDic.pack.zs";
        memcpy(ix + strings + at, path, strlen(path) + 1);
        if (i == nfiles) add(image, size, &row, path, dict_pack, dict_size, at);
        else {
            const sample *f = &files[i]; const uint8_t *plain = is_new ? f->new_plain : f->old_plain; uint64_t pn = is_new ? f->new_plain_size : f->old_plain_size, dn;
            uint8_t *d = i == victim ? damaged(plain, pn, &dn) : NULL; if (!d) { dn = pn; d = malloc(pn ? pn : 1); memcpy(d, plain, pn); }
            size_t bound = ZSTD_compressBound(dn); uint8_t *z = malloc(bound); size_t zn = ZSTD_compress(z, bound, d, dn, 1); free(d);
            if (i != victim || rnd() % 4) add(image, size, &row, path, z, zn, at);
            else { uint64_t n2; uint8_t *z2 = damaged(z, zn, &n2); add(image, size, &row, path, z2, n2, at); free(z2); }
            free(z);
        }
        at += strlen(path) + 1;
    }
    free(*index); *index = ix; *index_size = strings + names;
}
static int by_path(const void *a, const void *b) { return strcmp(((const sample *)a)->path, ((const sample *)b)->path); }
int main(int argc, char **argv) {
    if (argc != 6) { fprintf(stderr, "usage: damage <prod.keys> <base> <update> <seed> <rounds>\n"); return 2; }
    char err[300]; tokc_keys *keys = NULL; tokc_game *g = NULL; uint8_t *nb, *ob; size_t nn, on; index_table now, old;
    if (tokc_keys_parse_fd(open(argv[1], O_RDONLY), &keys, err, sizeof err) || tokc_game_open(open(argv[2], O_RDONLY), open(argv[3], O_RDONLY), keys, &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    tokc_keys_free(keys);
    if (tokc_romfs_index(g, &nb, &nn, err, sizeof err) || tokc_base_romfs_index(g, &ob, &on, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    if (parse_index(nb, nn, &now) || parse_index(ob, on, &old)) return 1;
    for (uint64_t i = 0; i < now.count; i++) if (!strcmp(now.rows[i].path, "Pack/ZsDic.pack.zs")) { dict_size = now.rows[i].size; dict_pack = malloc(dict_size); tokc_romfs_pread(g, dict_pack, dict_size, now.rows[i].off); }
    job real = {.g = g, .lock = PTHREAD_MUTEX_INITIALIZER};
    file_row dict_row = {"Pack/ZsDic.pack.zs", 0, dict_size}; index_table dict_table = {&dict_row, 1, NULL};
    new_image = dict_pack; new_size = dict_size;   /* make_dicts reads through the fake */
    if (make_dicts(&real, &dict_table)) { fprintf(stderr, "%s\n", real.err); return 1; }
    new_image = NULL; new_size = 0;
    for (uint64_t a = 0, b = 0; a < old.count && b < now.count && nfiles < MAX_FILES;) {
        int c = strcmp(old.rows[a].path, now.rows[b].path);
        if (!c && ends_with(old.rows[a].path, ".pack.zs") && old.rows[a].size < MAX_PACK && now.rows[b].size < MAX_PACK && !tokc_romfs_same_as_base(g, now.rows[b].off, old.rows[a].off, old.rows[a].size)) {
            sample *f = &files[nfiles]; snprintf(f->path, sizeof f->path, "%s", old.rows[a].path);
            f->old_size = (size_t)old.rows[a].size; f->new_size = (size_t)now.rows[b].size; f->old_bytes = malloc(f->old_size); f->new_bytes = malloc(f->new_size);
            tokc_base_romfs_pread(g, f->old_bytes, f->old_size, old.rows[a].off); tokc_romfs_pread(g, f->new_bytes, f->new_size, now.rows[b].off);
            f->old_plain = unzs(&real, f->old_bytes, f->old_size, &f->old_plain_size); f->new_plain = unzs(&real, f->new_bytes, f->new_size, &f->new_plain_size);
            if (f->old_plain && f->new_plain && f->old_plain_size < MAX_PACK * 4) nfiles++;
        }
        a += c <= 0; b += c >= 0;
    }
    if (!nfiles) { fprintf(stderr, "no small packs\n"); return 1; }
    qsort(files, nfiles, sizeof *files, by_path);
    /* the index must be sorted: leave out the files that would sort after the dictionaries' place, and keep ZsDic last */
    while (nfiles && strcmp(files[nfiles - 1].path, "Pack/ZsDic.pack.zs") >= 0) nfiles--;
    if (!nfiles) { fprintf(stderr, "no packs before the dictionaries\n"); return 1; }
    rng ^= strtoull(argv[4], NULL, 0) * 0x9e3779b97f4a7c15ull; if (!rng) rng = 1;
    unsigned rounds = (unsigned)atoi(argv[5]), good = 0, bad = 0;
    for (unsigned r = 0; r < rounds; r++) {
        victim = rnd() % nfiles;
        one_side(&old_image, &old_size, &old_index, &old_index_size, 0);
        one_side(&new_image, &new_size, &new_index, &new_index_size, 1);
        uint8_t *out = NULL; size_t n = 0;
        if (tokm_make((const tokc_game *)1, (int)(1 + r % 3), NULL, NULL, &out, &n, err, sizeof err)) bad++; else good++;
        free(out);
    }
    printf("%u files, %u rounds: %u refused, %u made\n", nfiles, rounds, bad, good);
    fflush(stdout);
    for (unsigned i = 0; i < real.ndicts; i++) ZSTD_freeDDict(real.dicts[i]);
    free(now.rows); free(old.rows); free(nb); free(ob); tokc_game_close(g);
    return 0;
}
