/* The rules of host/prompts_romfs.c against the PC's overlays, on a PC. Game bytes stay in memory.
     romfs_rules_check compare <prod.keys> <base> <update> <overlays> <overlays-out>
       every rule's result against the overlay it replaces, decompressed. The bytes of the files differ on purpose: an overlay is
       recompressed at level 19 with the dictionary, a rule's result is a frame of raw blocks (finish_raw_frame).
     romfs_rules_check malformed <prod.keys> <base> <update> <seed> <rounds>
       damaged files through every rule, for ASan and UBSan.
   regress.sh builds and runs it. */
#include "../prompts_romfs.c"
#include "container.h"
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

RomfsGenerator runtime_romfs_generator;
static uint64_t test_extra;
static int test_prompts, test_own_files = 1, fails;
static const tokc_game *game;
static uint8_t *index_bytes;
uint64_t runtime_output_res_extra_bytes(void) { return test_extra; }
int player_on(void) { return test_own_files; }
int dm_android_prompt_places(int8_t places[4], uint16_t *ability_char, char letters[4]) {
    if (!test_prompts) return 0;
    static const int8_t xbox[4] = {BOTTOM, RIGHT, LEFT, TOP};
    memcpy(places, xbox, 4); memcpy(letters, "BAYX", 4); *ability_char = 0;
    return 1;
}
/* runtime_output_res_extra_bytes of runtime/game_patches_v143.c, the size and scene as arguments; sw 0: TOTK_RENDER_RES=0 */
static uint64_t extra_for(unsigned w, unsigned h, unsigned sw, unsigned sh) {
    double grow = 0;
    if ((double)w * h > 1920.0 * 1080.0) grow += (double)w * h / (1920.0 * 1080.0) - 1;
    int scene = sw && (double)sw * sh > 1600.0 * 900.0;
    if (scene) grow += ((double)sw * sh / (1600.0 * 900.0) - 1);
    if (grow <= 0) return 0;
    if (scene) grow *= 1.6;
    uint64_t kib = (uint64_t)ceil(113664.0 * (1 + grow) / 1024.0) * 1024 - 113664; return kib << 10;
}
static void result(int ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }
static _Noreturn void die(const char *what) { fprintf(stderr, "%s\n", what); exit(2); }

int runtime_romfs_load(const char *path, uint8_t **out, uint64_t *size) {
    uint64_t count, strings; memcpy(&count, index_bytes + 8, 8); memcpy(&strings, index_bytes + 16, 8);
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *e = index_bytes + 24 + i * 24; uint64_t off, n; uint32_t name;
        memcpy(&off, e, 8); memcpy(&n, e + 8, 8); memcpy(&name, e + 16, 4);
        if (strcmp((const char *)index_bytes + strings + name, path)) continue;
        uint8_t *b = malloc(n ? n : 1); if (!b || tokc_romfs_pread(game, b, n, off) != (int64_t)n) die("read");
        *out = b; *size = n; return 0;
    }
    return -1;
}
static uint8_t *file_bytes(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1); if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read"); fclose(f);
    *size = (uint64_t)n; return b;
}
/* A .zs file decompressed, with the game's dictionary when its frame names one */
static uint8_t *unzs(const uint8_t *data, uint64_t size, uint64_t *out) {
    unsigned long long m = ZSTD_getFrameContentSize(data, size); if (m == ZSTD_CONTENTSIZE_UNKNOWN || m == ZSTD_CONTENTSIZE_ERROR) die("no content size");
    uint8_t *b = malloc(m ? m : 1); ZSTD_DCtx *d = ZSTD_createDCtx(); if (!b || !d) die("memory");
    size_t r = ZSTD_getDictID_fromFrame(data, size) ? ZSTD_decompress_usingDDict(d, b, m, data, size, dictionary()) : ZSTD_decompressDCtx(d, b, m, data, size);
    ZSTD_freeDCtx(d); if (ZSTD_isError(r) || r != m) die("does not decompress");
    *out = m; return b;
}
static const char kCommon[] = "UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs";

/* Every byte of a and b that differs lies in the SARC member `only` of b; NULL: none may differ */
static uint64_t differing(const uint8_t *a, uint64_t an, uint8_t *b, uint64_t bn, const char *only, uint64_t *outside) {
    uint64_t lo = 0, hi = 0, mn = 0, count = 0; *outside = an != bn;
    if (only) { uint8_t *m = sarc_member(b, bn, only, &mn); if (m) { lo = (uint64_t)(m - b); hi = lo + mn; } }
    for (uint64_t i = 0; i < an && i < bn; i++) if (a[i] != b[i]) { count++; if (i < lo || i >= hi) ++*outside; }
    return count;
}
static void compare_layout(const char *dir) {
    char path[1200], line[300]; uint64_t on, tn, gn, wn, raw_n, plain_n; uint8_t *raw, *g, *w;
    if (runtime_romfs_load(kCommon, &raw, &raw_n)) die("no layout archive");
    snprintf(path, sizeof path, "%s/%s", dir, kCommon);
    uint8_t *over = file_bytes(path, &on); if (!over) die("no overlay of the layout archive");
    uint8_t *target = unzs(over, on, &tn), *plain = unzs(raw, raw_n, &plain_n);
    test_prompts = 0;
    result(generate(kCommon, NULL, 0, NULL, NULL) == 1, "layout archive: the host says it makes it, with no prompts to change");
    result(generate(kCommon, raw, raw_n, &g, &gn) == 1, "layout archive: capture rule ran");
    uint64_t made_n; uint8_t *made = unzs(g, gn, &made_n);
    uint64_t outside, count = differing(made, made_n, target, tn, NULL, &outside);
    snprintf(line, sizeof line, "capture rule equals the overlay, %llu decompressed bytes", (unsigned long long)tn);
    result(!count && !outside, line);
    count = 0; for (uint64_t i = 0; i < plain_n && i < tn; i++) count += plain[i] != target[i];
    snprintf(line, sizeof line, "the rule changed something: %llu bytes differ from the original", (unsigned long long)count);
    result(count > 0, line);
    result(!memcmp(g + RAW_FRAME_HEAD + RAW_BLOCK_HEAD, target, 16), "the result is a frame of raw blocks");
    free(g); free(made);
    /* a file that holds the edit already: nothing to do, the bytes stay */
    uint8_t *stale = NULL; uint64_t sn = 0;
    result(generate(kCommon, over, on, &stale, &sn) == 0 && !stale, "the rule on the overlay itself: unchanged");
    /* with the prompts on: the overlay's archive, plus the layout's own edits */
    test_prompts = 1;
    result(generate(kCommon, raw, raw_n, &w, &wn) == 1, "layout archive: capture rule and prompts ran");
    uint64_t both_n; uint8_t *both = unzs(w, wn, &both_n);
    count = differing(both, both_n, target, tn, "blyt/DoCommand_00.bflyt", &outside);
    snprintf(line, sizeof line, "with prompts: %llu bytes differ from the overlay, all in the command bubble", (unsigned long long)count);
    result(count > 0 && !outside, line);
    free(w); free(both); free(raw); free(over); free(target); free(plain);
    test_prompts = 0;
}
static uint64_t dta_size_at(const uint8_t *b, uint64_t m) {
    uint64_t keys = rd32(b + 4), heaps = byml_field(b, m, keys, rd32(b + 12), "Heaps", 0xc1);
    uint64_t h = byml_field(b, m, keys, rd32(b + heaps), "DynamicTextureAllocatorMem", 0xc1);
    return byml_field(b, m, keys, rd32(b + h), "Size", 0xd1);
}
static void compare_heaps(const char *dir) {
    DIR *d = opendir(dir); if (!d) die("no HeapDef overlays");
    uint8_t *raw; uint64_t raw_n, out_n; if (runtime_romfs_load(kHeapDef, &raw, &raw_n)) die("no HeapDef");
    for (struct dirent *e; (e = readdir(d));) {
        unsigned w, h, sw = 0, sh = 0; int fields = sscanf(e->d_name, "%ux%u-scene-%ux%u", &w, &h, &sw, &sh);
        if (fields != 2 && fields != 4) continue;
        char path[1200], line[300]; uint64_t on, tn, mn; uint8_t *out = NULL;
        snprintf(path, sizeof path, "%s/%s/%s", dir, e->d_name, kHeapDef);
        uint8_t *over = file_bytes(path, &on); if (!over) die("no overlay");
        uint8_t *target = unzs(over, on, &tn);
        test_extra = extra_for(w, h, sw, sh);
        int rc = generate(kHeapDef, raw, raw_n, &out, &out_n);
        uint8_t *made = rc == 1 ? unzs(out, out_n, &mn) : NULL;
        snprintf(line, sizeof line, "HeapDef %s: +%llu KiB, %llu decompressed bytes equal the overlay", e->d_name, (unsigned long long)test_extra >> 10, (unsigned long long)tn);
        result(made && mn == tn && !memcmp(made, target, tn), line);
        /* the old path: an overlay already loaded stays as it is, so the heaps do not grow twice */
        test_own_files = 0; uint8_t *again = NULL; uint64_t an = 0;
        snprintf(line, sizeof line, "HeapDef %s: on the old path the overlay stays", e->d_name);
        result(generate(kHeapDef, over, on, &again, &an) == 0 && !again, line);
        test_own_files = 1;
        free(out); free(made); free(over); free(target);
    }
    closedir(d);
    test_extra = 0;
    result(generate(kHeapDef, NULL, 0, NULL, NULL) == 0, "HeapDef: no rule below 1600x900");
    test_extra = extra_for(1920, 1080, 1600, 900); result(test_extra == 0, "HeapDef: 1600x900 needs no growth");
    test_extra = extra_for(1920, 1080, 1920, 1080);
    /* a mod's table: the same growth on top of its own sizes */
    uint64_t plain_n, mn; uint8_t *plain = unzs(raw, raw_n, &plain_n), *out = NULL; uint32_t mine = 200000;
    uint64_t at = dta_size_at(plain, plain_n); memcpy(plain + at, &mine, 4);
    size_t bound = ZSTD_compressBound(plain_n); uint8_t *mod = malloc(bound); size_t zn = ZSTD_compress(mod, bound, plain, plain_n, 3);
    result(generate(kHeapDef, mod, zn, &out, &out_n) == 1, "HeapDef: a mod's table is grown too");
    uint8_t *made = out ? unzs(out, out_n, &mn) : NULL;
    result(made && rd32(made + at) == mine + (test_extra >> 10), "HeapDef: the mod's size plus the growth");
    free(raw); free(plain); free(mod); free(out); free(made); test_extra = 0;
}

/* ---- damage ---- */
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint8_t *damaged(const uint8_t *src, uint64_t n, uint64_t *out) {
    static const uint8_t hot[] = {0x00, 0x01, 0x02, 0x7f, 0x80, 0xff};
    uint64_t m = rnd() % 8 ? n : rnd() % (n + 1);   /* cut short now and then */
    uint8_t *b = malloc(m ? m : 1); memcpy(b, src, m);   /* exactly m bytes: ASan sees any read past them */
    for (unsigned k = rnd() % 9; m && k; k--) {
        uint64_t at = rnd() % 4 ? rnd() % m : rnd() % (m < 256 ? m : 256);   /* the headers more often */
        b[at] = rnd() % 2 ? hot[rnd() % sizeof hot] : (uint8_t)rnd();
    }
    *out = m; return b;
}
/* A little SARC of the given members, in the form sarc_member reads */
static uint8_t *small_sarc(const char *const *names, uint8_t *const *data, const uint64_t *sizes, unsigned count, uint64_t *out) {
    uint64_t text = 0; for (unsigned i = 0; i < count; i++) text += (strlen(names[i]) + 4) & ~3ull;
    uint64_t head = 0x14, nodes = head + 12, fnt = nodes + 16ull * count, body = fnt + 8 + text, total = body;
    for (unsigned i = 0; i < count; i++) total += sizes[i];
    uint8_t *s = calloc(1, total); memcpy(s, "SARC", 4); s[4] = 0x14; s[6] = 0xff; s[7] = 0xfe;
    uint32_t v = (uint32_t)total; memcpy(s + 8, &v, 4); v = (uint32_t)body; memcpy(s + 12, &v, 4);
    memcpy(s + head, "SFAT", 4); s[head + 6] = (uint8_t)count; memcpy(s + fnt, "SFNT", 4);
    uint64_t at = 0, name = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = s + nodes + 16 * i; uint32_t attr = 0x01000000u | (uint32_t)(name / 4), a = (uint32_t)at, z = (uint32_t)(at + sizes[i]);
        memcpy(e + 4, &attr, 4); memcpy(e + 8, &a, 4); memcpy(e + 12, &z, 4);
        strcpy((char *)s + fnt + 8 + name, names[i]); name += (strlen(names[i]) + 4) & ~3ull;
        memcpy(s + body + at, data[i], sizes[i]); at += sizes[i];
    }
    *out = total; return s;
}
static void malformed(unsigned rounds) {
    uint8_t *raw; uint64_t raw_n, arc_n, heap_n;
    if (runtime_romfs_load(kCommon, &raw, &raw_n)) die("no layout archive");
    uint8_t *arc = unzs(raw, raw_n, &arc_n); free(raw);
    if (runtime_romfs_load(kHeapDef, &raw, &raw_n)) die("no HeapDef");
    uint8_t *heap = unzs(raw, raw_n, &heap_n); free(raw);
    static const char *const names[] = {"anim/AutoBuilder_00_In.bflan", "anim/ShortCutBG_00_In.bflan", "blyt/DoCommand_00.bflyt"};
    uint8_t *member[3]; uint64_t size[3];
    for (int i = 0; i < 3; i++) if (!(member[i] = sarc_member(arc, arc_n, names[i], &size[i]))) die("no member");
    uint64_t small_n; uint8_t *small = small_sarc(names, member, size, 3, &small_n);
    uint64_t moved = 0, whole = 0, heaps = 0;
    for (unsigned r = 0; r < rounds; r++) {
        uint64_t n; int other = 0;
        for (int i = 0; i < 2; i++) {
            uint8_t *b = damaged(member[i], size[i], &n);
            moved += (uint64_t)capture_member(b, n, kCapture[i ? 0 : 1].pane, &other); free(b);
        }
        uint8_t *b = damaged(small, small_n, &n); moved += (uint64_t)capture_timing(b, n); free(b);
        /* the whole rule, damaged before and after compression */
        b = damaged(small, small_n, &n);
        size_t bound = ZSTD_compressBound(n); uint8_t *z = malloc(bound), *out = NULL; uint64_t out_n; size_t zn = ZSTD_compress(z, bound, b, n, 1); free(b);
        uint64_t dn; uint8_t *d = damaged(z, zn, &dn); test_prompts = (int)(r & 1);
        if (generate(kCommon, r & 2 ? d : z, r & 2 ? dn : zn, &out, &out_n) == 1) { whole++; free(out); }
        free(d); free(z);
        b = damaged(heap, heap_n, &n); test_extra = 90u << 20; test_own_files = (int)(r & 1);
        bound = ZSTD_compressBound(n); z = malloc(bound); zn = ZSTD_compress(z, bound, b, n, 1); free(b); out = NULL;
        if (generate(kHeapDef, z, zn, &out, &out_n) == 1) { heaps++; free(out); }
        free(z);
    }
    free(small); free(arc); free(heap);
    printf("%u rounds: %llu keys moved, %llu layouts and %llu heap tables served\n", rounds, (unsigned long long)moved, (unsigned long long)whole, (unsigned long long)heaps);
    result(1, "damaged files through the rules");
}
static void open_game(char **argv) {
    char err[300]; tokc_keys *keys; tokc_game *g; size_t size; uint8_t *bytes;
    int kf = open(argv[2], O_RDONLY), bf = open(argv[3], O_RDONLY), uf = open(argv[4], O_RDONLY);
    if (kf < 0 || bf < 0 || uf < 0) die("cannot open the files");
    if (tokc_keys_parse_fd(kf, &keys, err, sizeof err) || tokc_game_open(bf, uf, keys, &g, err, sizeof err)) die(err);
    tokc_keys_free(keys);
    if (tokc_romfs_index(g, &bytes, &size, err, sizeof err)) die(err);
    game = g; index_bytes = bytes;
}
int main(int argc, char **argv) {
    if (argc == 7 && !strcmp(argv[1], "compare")) {
        open_game(argv); compare_layout(argv[5]); compare_heaps(argv[6]);
    } else if (argc == 7 && !strcmp(argv[1], "malformed")) {
        open_game(argv); rng ^= strtoull(argv[5], NULL, 0) * 0x9e3779b97f4a7c15ull; if (!rng) rng = 1;
        malformed((unsigned)atoi(argv[6]));
    } else die("usage: romfs_rules_check compare <keys> <base> <update> <overlays> <overlays-out> | malformed <keys> <base> <update> <seed> <rounds>");
    printf("failures: %d\n", fails);
    return fails ? 1 : 0;
}
