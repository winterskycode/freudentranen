/* tables_cli: the tables from the command line.

     tables_cli files <rewritten dir> <exefs dir> <output file>
       From a rewritten folder and stored modules on disk, as prepare_runtime.py reads them. For comparing with that tool.

     tables_cli chain <prod.keys> <base> <update> [--out DIR] [--rewritten DIR] [--game-data DIR] [--full] [--bench N]
       The phone's path: the player's files through setup/container and setup/patcher into the tables, all in memory.
       --out writes runtime-tables.bin and rewriter.txt.
       --rewritten and --game-data compare with every file in a live folder: a file this program cannot account for is a
       failure. Prints a line per file, and offsets only, never bytes.
       --full also hashes the whole RomFS image, for ROMFS_IMAGE_SHA256 (17 GB through the reader).
       --bench N repeats tokt_make N times and prints the median, with and without hashing the images.

   Exit status: 0 only when everything compared was the same. Keys are never printed. */
#include "container.h"
#include "rewritten_files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long long ull;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int write_file(const char *dir, const char *name, const void *data, size_t n) {
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return -1; }
    int bad = fwrite(data, 1, n, f) != n;
    if (fclose(f)) bad = 1;
    if (bad) fprintf(stderr, "%s: cannot write\n", path);
    return bad ? -1 : 0;
}

static int cmd_files(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s files <rewritten dir> <exefs dir> <output file>\n", argv[0]); return 2; }
    tokt_files f[TOKT_MODULE_COUNT];
    tokt_module m[TOKT_MODULE_COUNT];
    char err[256] = "";
    int loaded = 0, rc = 1;
    for (; loaded < TOKT_MODULE_COUNT; loaded++) {
        if (tokt_files_load(&f[loaded], argv[2], argv[3], tokt_module_names[loaded], err, sizeof err)) { fprintf(stderr, "%s\n", err); goto out; }
        m[loaded] = (tokt_module){f[loaded].nso, f[loaded].nso_size, f[loaded].image, &f[loaded].rewrite, NULL};
    }
    uint8_t *tables;
    size_t size;
    int made = tokt_make(m, &tables, &size, err, sizeof err);
    if (made) { fprintf(stderr, "refused (%d): %s\n", made, err); goto out; }
    FILE *o = fopen(argv[4], "wb");
    rc = !o || fwrite(tables, 1, size, o) != size;
    if (o && fclose(o)) rc = 1;
    if (rc) fprintf(stderr, "%s: cannot write\n", argv[4]);
    free(tables);
out:
    while (loaded > 0) tokt_files_free(&f[--loaded]);
    return rc;
}

/* ---- the chain ---- */

/* What a live folder must hold, made here from the player's files. */
typedef struct { char name[64]; const uint8_t *data; size_t size; int owned, first_line, seen; const char *why_not; } Expect;
#define MAX_EXPECT 40
typedef struct { Expect e[MAX_EXPECT]; size_t n; int failures; } Folder;

static Expect *expect_ptr(Folder *f, const char *name, const void *data, size_t size) {
    if (f->n == MAX_EXPECT) { fprintf(stderr, "too many expected files\n"); exit(2); }
    Expect *e = &f->e[f->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->data = data; e->size = size;
    return e;
}

static Expect *expect_stream(Folder *f, const char *name, char *text, size_t size) {
    Expect *e = expect_ptr(f, name, text, size);
    e->owned = 1;
    return e;
}

/* The same with printf text, kept in memory. */
__attribute__((format(printf, 3, 4)))
static Expect *expect_text(Folder *f, const char *name, const char *fmt, ...) {
    char *text = NULL;
    size_t size = 0;
    FILE *s = open_memstream(&text, &size);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s, fmt, ap);
    va_end(ap);
    fclose(s);
    return expect_stream(f, name, text, size);
}

/* What patcher_cli writes beside an image: the same bytes as the Python tool. */
static void expect_side_files(Folder *f, const char *m, const tokp_result *r) {
    static const char *const keys[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    const tokp_layout *l = &r->layout;
    char name[64], *text;
    size_t size;
    FILE *s;

    snprintf(name, sizeof name, "%s.target", m);
    expect_text(f, name, "%s\n", TOKP_TARGET);
    s = open_memstream(&text, &size);
    fprintf(s, "# PATCH_OFF\t0x%llx\n# KEY_PAGE_OFF\t0x%llx\n# KEY_PAGE_SIZE\t0x%x\n# TEXT_END\t0x%llx\n# RO_OFF\t0x%llx\n# DATA_OFF\t0x%llx\n",
            (ull)l->patch_off, (ull)l->key_page_off, TOKP_KEY_PAGE_SIZE, (ull)l->text_end, (ull)l->ro_off, (ull)l->data_off);
    for (int i = 0; i < 8; i++) fprintf(s, "# KEY_%s_IDX\t%d\n", keys[i], i);
    fclose(s);
    snprintf(name, sizeof name, "%s.fixups.tsv", m);
    expect_stream(f, name, text, size);
    s = open_memstream(&text, &size);
    for (size_t i = 0; i < r->ngates; i++) {
        const tokp_gate *g = &r->gates[i];
        fprintf(s, "0x%llx\t%s\t%u\t0x%llx\n", (ull)g->ret, tokp_kind_names[g->kind], g->arg, (ull)g->original);
    }
    fclose(s);
    snprintf(name, sizeof name, "%s.gates.tsv", m);
    expect_stream(f, name, text, size);
    s = open_memstream(&text, &size);
    for (size_t i = 0; i < r->nsites; i++) fprintf(s, "0x%llx\t%s\n", (ull)r->sites[i].off, tokp_kind_names[r->sites[i].kind]);
    fclose(s);
    snprintf(name, sizeof name, "%s.sites.tsv", m);
    expect_stream(f, name, text, size);
    static const char *const empty[3] = {"cuts.tsv", "params.tsv", "obs.tsv"};
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "%s.%s", m, empty[i]);
        expect_text(f, name, "# BUILD_ID\t%s\n", l->build_id);
    }
    /* The Python tool's summary is the log's first line; main's second is a notice with no data in it. */
    unsigned count[TOKP_KIND_COUNT] = {0}, seen = 0;
    int order[TOKP_KIND_COUNT];
    for (size_t i = 0; i < r->nsites; i++) if (!count[r->sites[i].kind]++) order[seen++] = (int)r->sites[i].kind;
    s = open_memstream(&text, &size);
    fprintf(s, "%s {", m);
    for (unsigned i = 0; i < seen; i++) fprintf(s, "%s'%s': %u", i ? ", " : "", tokp_kind_names[order[i]], count[order[i]]);
    fprintf(s, "} cuts 0 obs 0 patch %llu PATCH_OFF 0x%llx slot_refs %u gates %zu\n", (ull)r->patch_size, (ull)l->patch_off, r->slot_refs, r->ngates);
    fclose(s);
    snprintf(name, sizeof name, "%s.log", m);
    expect_stream(f, name, text, size)->first_line = 1;
}

static void compare_one(Folder *f, const char *dir, Expect *e) {
    char path[4096];
    uint8_t *have = NULL;
    size_t size = 0;
    e->seen = 1;
    if (e->why_not) { printf("not compared  %-24s %s\n", e->name, e->why_not); return; }
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (tokt_read_file(path, &have, &size)) { printf("FAIL  %-24s cannot read\n", e->name); f->failures++; return; }
    size_t compared = size;
    if (e->first_line) { const uint8_t *nl = memchr(have, '\n', size); compared = nl ? (size_t)(nl - have) + 1 : size; }
    if (compared == e->size && (!e->size || !memcmp(have, e->data, compared))) {
        printf("same  %-24s %zu bytes%s\n", e->name, size, compared != size ? " (first line compared)" : "");
    } else {
        size_t at = 0, n = compared < e->size ? compared : e->size;
        while (at < n && have[at] == e->data[at]) at++;
        printf("FAIL  %-24s differs at offset %zu; %zu bytes here, %zu made\n", e->name, at, compared, e->size);
        f->failures++;
    }
    free(have);
}

/* Every file listed must be there and the same, and every file there must be listed. */
static void compare_folder(Folder *f, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { printf("FAIL  %s: %s\n", dir, strerror(errno)); f->failures++; return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        Expect *e = NULL;
        for (size_t i = 0; i < f->n; i++) if (!strcmp(f->e[i].name, de->d_name)) e = &f->e[i];
        if (e) compare_one(f, dir, e);
        else { printf("FAIL  %-24s in the folder, not made by this chain\n", de->d_name); f->failures++; }
    }
    closedir(d);
    for (size_t i = 0; i < f->n; i++) if (!f->e[i].seen) { printf("FAIL  %-24s made, but not in the folder\n", f->e[i].name); f->failures++; }
}

static void folder_free(Folder *f) {
    for (size_t i = 0; i < f->n; i++) if (f->e[i].owned) free((void *)f->e[i].data);
    f->n = 0;
}

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static double median_make(const tokt_module *m, int runs, char *err, size_t errlen) {
    double *t = malloc((size_t)runs * sizeof *t);
    if (!t) return -1;
    for (int i = 0; i < runs; i++) {
        uint8_t *tables;
        size_t size;
        double t0 = now_ms();
        int rc = tokt_make(m, &tables, &size, err, errlen);
        t[i] = now_ms() - t0;
        if (rc) { free(t); return -1; }
        free(tables);
    }
    qsort(t, (size_t)runs, sizeof *t, cmp_double);
    double v = t[runs / 2];
    free(t);
    return v;
}

/* The RomFS image through the reader, for ROMFS_IMAGE_SHA256. */
static int hash_image(const tokc_game *g, char hex[65]) {
    enum { CHUNK = 8 << 20 };
    static const char digits[] = "0123456789abcdef";
    uint8_t *buf = malloc(CHUNK), d[32];
    tokt_sha256 s;
    uint64_t total = tokc_romfs_size(g);
    if (!buf) return -1;
    tokt_sha256_init(&s);
    for (uint64_t off = 0; off < total;) {
        int64_t n = tokc_romfs_pread(g, buf, CHUNK, off);
        if (n <= 0) { free(buf); return -1; }
        tokt_sha256_update(&s, buf, (size_t)n);
        off += (uint64_t)n;
    }
    free(buf);
    tokt_sha256_final(&s, d);
    for (int i = 0; i < 32; i++) { hex[2 * i] = digits[d[i] >> 4]; hex[2 * i + 1] = digits[d[i] & 15]; }
    hex[64] = 0;
    return 0;
}

static int cmd_chain(int argc, char **argv) {
    const char *out = NULL, *rewritten = NULL, *game_data = NULL;
    int full = 0, bench = 0;
    if (argc < 5) { fprintf(stderr, "usage: %s chain <prod.keys> <base> <update> [--out DIR] [--rewritten DIR] [--game-data DIR] [--full] [--bench N]\n", argv[0]); return 2; }
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) full = 1;
        else if (i + 1 >= argc) { fprintf(stderr, "%s: needs a value\n", argv[i]); return 2; }
        else if (!strcmp(argv[i], "--out")) out = argv[++i];
        else if (!strcmp(argv[i], "--rewritten")) rewritten = argv[++i];
        else if (!strcmp(argv[i], "--game-data")) game_data = argv[++i];
        else if (!strcmp(argv[i], "--bench")) bench = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    char err[256] = "";
    int keys_fd = open(argv[2], O_RDONLY | O_CLOEXEC), base_fd = open(argv[3], O_RDONLY | O_CLOEXEC), update_fd = open(argv[4], O_RDONLY | O_CLOEXEC);
    if (keys_fd < 0 || base_fd < 0 || update_fd < 0) { fprintf(stderr, "cannot open the inputs: %s\n", strerror(errno)); return 1; }

    double t0 = now_ms();
    tokc_keys *keys;
    tokc_game *game;
    int rc = tokc_keys_parse_fd(keys_fd, &keys, err, sizeof err);
    if (rc) { fprintf(stderr, "keys: %s\n", err); return 1; }
    close(keys_fd);
    rc = tokc_game_open(base_fd, update_fd, keys, &game, err, sizeof err);
    tokc_keys_free(keys);
    if (rc) { fprintf(stderr, "game: %s\n", err); return 1; }
    double t_open = now_ms() - t0, t_read = 0, t_rewrite = 0;

    static const tokc_module which[TOKT_MODULE_COUNT] = {TOKC_MODULE_MAIN, TOKC_MODULE_SUBSDK0, TOKC_MODULE_SDK};
    uint8_t *nso[TOKT_MODULE_COUNT], *image[TOKT_MODULE_COUNT];
    size_t nso_size[TOKT_MODULE_COUNT];
    tokp_result result[TOKT_MODULE_COUNT];
    tokt_module m[TOKT_MODULE_COUNT];
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) {
        t0 = now_ms();
        if ((rc = tokc_game_module(game, which[i], &nso[i], &nso_size[i], err, sizeof err))) { fprintf(stderr, "%s: %s\n", tokt_module_names[i], err); return 1; }
        t_read += now_ms() - t0;
        t0 = now_ms();
        if ((rc = tokp_rewrite_alloc(nso[i], nso_size[i], &image[i], &result[i], err, sizeof err))) { fprintf(stderr, "%s: %s\n", tokt_module_names[i], err); return 1; }
        t_rewrite += now_ms() - t0;
        m[i] = (tokt_module){nso[i], nso_size[i], image[i], &result[i], NULL};
    }
    t0 = now_ms();
    if ((rc = tokt_check_main(nso[TOKT_MAIN], nso_size[TOKT_MAIN], err, sizeof err))) { fprintf(stderr, "%s\n", err); return 1; }
    double t_check = now_ms() - t0;
    uint8_t *tables;
    size_t size;
    t0 = now_ms();
    if ((rc = tokt_make(m, &tables, &size, err, sizeof err))) { fprintf(stderr, "tables: %s\n", err); return 1; }
    double t_make = now_ms() - t0;
    printf("SHA-256: %s\n", tokt_sha_impl_name());
    printf("ms: keys and open %.1f, modules read %.1f, rewrite %.1f, main check %.1f, tables %.1f\n", t_open, t_read, t_rewrite, t_check, t_make);

    uint64_t strings, strings_size;
    memcpy(&strings, tables + 40, 8); memcpy(&strings_size, tables + 48, 8);
    uint64_t nsym = 0, nreloc = 0, ngate = 0;
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) {
        uint32_t c[3];
        memcpy(c, tables + 64 + 208 * i + 192, sizeof c);
        nsym += c[0]; nreloc += c[1]; ngate += c[2];
    }
    printf("tables: %zu bytes: symbols %llu, relocations %llu, gates %llu, names %llu, header and records 688\n", size, (ull)(nsym * 24), (ull)(nreloc * 24), (ull)(ngate * 24), (ull)strings_size);

    if (bench > 0) {
        char hex[TOKT_MODULE_COUNT][65];
        double with_hash = median_make(m, bench, err, sizeof err);
        for (int i = 0; i < TOKT_MODULE_COUNT; i++) { tokt_sha256_hex(image[i], (size_t)result[i].image_size, hex[i]); m[i].image_sha256 = hex[i]; }
        double without = median_make(m, bench, err, sizeof err);
        printf("tables, median of %d: %.2f ms hashing the images, %.2f ms given their hashes\n", bench, with_hash, without);
        for (int i = 0; i < TOKT_MODULE_COUNT; i++) m[i].image_sha256 = NULL;
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("peak resident: %ld MB\n", ru.ru_maxrss / 1024);

    if (out) {
        if (write_file(out, "runtime-tables.bin", tables, size) || write_file(out, "rewriter.txt", TOKT_REWRITER_TXT, sizeof TOKT_REWRITER_TXT - 1)) return 1;
    }

    int failures = 0;
    if (rewritten) {
        static Folder f;
        char name[64];
        for (int i = 0; i < TOKT_MODULE_COUNT; i++) {
            snprintf(name, sizeof name, "%s.patched.bin", tokt_module_names[i]);
            expect_ptr(&f, name, image[i], (size_t)result[i].image_size);
            expect_side_files(&f, tokt_module_names[i], &result[i]);
        }
        expect_text(&f, "build_src.sha256", "%s  patch_nso.py\n", TOKP_PY_SHA256);
        compare_folder(&f, rewritten);
        failures += f.failures;
        folder_free(&f);
    }
    if (game_data) {
        static Folder f;
        uint8_t *index;
        size_t index_size;
        char index_hex[65], image_hex[65] = "";
        t0 = now_ms();
        if ((rc = tokc_romfs_index(game, &index, &index_size, err, sizeof err))) { fprintf(stderr, "index: %s\n", err); return 1; }
        double t_index = now_ms() - t0;
        tokt_sha256_hex(index, index_size, index_hex);
        free(index);
        printf("ms: RomFS index %.1f (%zu bytes)\n", t_index, index_size);
        if (full) {
            t0 = now_ms();
            if (hash_image(game, image_hex)) { fprintf(stderr, "the RomFS image did not read\n"); return 1; }
            printf("ms: whole RomFS image hashed through the reader %.0f\n", now_ms() - t0);
        }
        else {
            /* without --full the file's own image hash stands in, and the size and index hash are still compared */
            char path[4096], *text = NULL, *at;
            uint8_t *live;
            size_t live_size;
            snprintf(path, sizeof path, "%s/romfs-config.txt", game_data);
            if (tokt_read_file(path, &live, &live_size)) { fprintf(stderr, "%s: cannot read\n", path); return 1; }
            text = malloc(live_size + 1);
            if (!text) return 1;
            memcpy(text, live, live_size); text[live_size] = 0;
            if ((at = strstr(text, "ROMFS_IMAGE_SHA256 ")) && strlen(at) >= 19 + 64) { memcpy(image_hex, at + 19, 64); image_hex[64] = 0; }
            free(text); free(live);
            printf("not compared  ROMFS_IMAGE_SHA256       needs --full\n");
        }
        expect_ptr(&f, "runtime-tables.bin", tables, size);
        expect_ptr(&f, "rewriter.txt", TOKT_REWRITER_TXT, sizeof TOKT_REWRITER_TXT - 1);
        expect_text(&f, "romfs-config.txt", "ROMFS_IMAGE_SIZE %llu\nROMFS_IMAGE_SHA256 %s\nROMFS_INDEX_SHA256 %s\n", (ull)tokc_romfs_size(game), image_hex, index_hex);
        expect_ptr(&f, "shared-fonts.txt", "", 0)->why_not = "firmware input, not in the NSPs";
        compare_folder(&f, game_data);
        failures += f.failures;
        folder_free(&f);
    }
    free(tables);
    for (int i = 0; i < TOKT_MODULE_COUNT; i++) { free(nso[i]); free(image[i]); tokp_result_free(&result[i]); }
    tokc_game_close(game);
    close(base_fd); close(update_fd);
    if (rewritten || game_data) printf("%d failures\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "files")) return cmd_files(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "chain")) return cmd_chain(argc, argv);
    fprintf(stderr, "usage: %s files ... | chain ...\n", argv[0]);
    return 2;
}
