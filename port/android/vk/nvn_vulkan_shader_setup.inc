/* Shader setup (plan step 8): every shader of the game translated once on the device, ahead of play, as the PC's
   tools/aot_shaders.py does for this developer's device. Players have no PC step: the APK carries the translator, and
   this finds the shaders in the player's own RomFS and game modules and translates each for the default bind state
   (the translator takes texture kinds from the instructions and vertex input types from their use, so that variant
   serves almost every draw; the rare other bind states are still translated when the game first meets them).

   The inventory is MacNativeGames' prepare_shader_seed.py inventory(), in C: the RomFS files that can hold shaders
   (zstd-compressed with the game's dictionaries, SARC packages, BFSHA/BNSH archives, agl .sharcb packages) and the
   modules' embedded BNSH files; a shader is SHA-256 of its control section and code (totk_shader_id). Only the folders
   that hold shaders in 1.4.3 are read (Shader/, Effect/, Lib/, UI/ and the boot package: about 17,000 files, 1.6 GB of
   the 17 GB RomFS): on the PC this finds exactly the 28,771 shaders of prepare_shader_seed.py's full scan, and its
   translations are byte-identical to aot_shaders.py's (2026-10-30; TOTK_SHADER_SETUP_LIST=<file> lists the ids found).
   A shader it misses is translated when the game first uses it, as before. On the Odin 3 an empty cache took 165 s
   (title screen; 82 shaders fail, the same 82 as on the PC).

   It runs in the background of the game's first session (or after the cache was dropped), from the first present, with
   its own translator processes at low priority so play goes on: a variant made here carries <key>.aot like the PC's,
   so the warm-up skips it until the game first uses it. <cache>/setup.txt records a finished setup (the RomFS index's
   size and the cache revision); TOTK_SHADER_SETUP=0 turns it off, =2 runs it even when setup.txt says it is done. */
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <zstd.h>
int runtime_romfs_load(const char *path, uint8_t **out, uint64_t *size);   /* runtime.h */

typedef struct { uint64_t offset, size; const char *name; } setup_file;
typedef struct setup_shader { char id[65]; int st; uint8_t *control, *code; uint32_t control_size, code_size; struct setup_shader *next; } setup_shader;
static struct {
    int fd; setup_file *files; size_t nfiles, next_file;
    ZSTD_DDict *dicts[8]; unsigned dict_ids[8], ndicts;
    setup_shader *table[8192]; size_t count; setup_shader **list;
    pthread_mutex_t lock; size_t next_job;
    uint64_t translated, present, failed, scanned_bytes, decoded_bytes;
} setup = {.fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER};

#include "player_files.h"
static uint32_t su_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t su_rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int su_range(uint64_t size, uint64_t offset, uint64_t n) { return offset <= size && n <= size - offset; }

/* runtime-index.bin (MacNativeGames prepare_romfs_backend.py): "TOTKRFS1", count, string table offset, then per file
   offset, size, name offset, name length (with its NUL) */
static int setup_index(const char *romfs) {
    char path[1100]; snprintf(path, sizeof path, "%s/runtime-index.bin", romfs);
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) || st.st_size < 24) { close(fd); return 0; }
    uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) return 0;
    uint64_t count = su_rd64(m + 8), strings = su_rd64(m + 16);
    if (memcmp(m, "TOTKRFS1", 8) || strings != 24 + count * 24 || strings > (uint64_t)st.st_size) return 0;
    setup.files = calloc(count, sizeof *setup.files); if (!setup.files) return 0;
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *e = m + 24 + i * 24; uint32_t name = su_rd32(e + 16), len = su_rd32(e + 20);
        if (!len || !su_range((uint64_t)st.st_size, strings + name, len) || m[strings + name + len - 1]) continue;
        const char *n = (const char *)m + strings + name;
        /* the folders that hold shaders (see the top) */
        if (strncmp(n, "Shader/", 7) && strncmp(n, "Effect/", 7) && strncmp(n, "Lib/", 4) && strncmp(n, "UI/", 3) && strcmp(n, "Pack/Bootup.Nin_NX_NVN.pack.zs")) continue;
        setup.files[setup.nfiles++] = (setup_file){su_rd64(e), su_rd64(e + 8), n};   /* the map stays: names point into it */
    }
    if (player_on && player_on()) return 1;   /* no image: setup_read goes through the player's files */
    snprintf(path, sizeof path, "%s/romfs.bin", romfs);
    setup.fd = open(path, O_RDONLY);
    return setup.fd >= 0;
}
static uint8_t *setup_read(uint64_t offset, uint64_t size) {
    uint8_t *b = size < (1ull << 31) ? malloc(size ? size : 1) : NULL; if (!b) return NULL;
    int player = player_on && player_on();
    for (uint64_t done = 0; done < size;) { ssize_t r = player ? player_romfs_pread(b + done, size - done, (off_t)(offset + done)) : pread(setup.fd, b + done, size - done, (off_t)(offset + done)); if (r <= 0) { free(b); return NULL; } done += (uint64_t)r; }
    __atomic_add_fetch(&setup.scanned_bytes, size, __ATOMIC_RELAXED);
    return b;
}
/* A member of a SARC by index: its name and bytes; 0 past the last. Little-endian packages only (TOTK's). */
static int setup_sarc_member(const uint8_t *s, uint64_t size, unsigned i, const uint8_t **data, uint64_t *n) {
    if (size < 0x14 || memcmp(s, "SARC", 4) || s[6] != 0xff || s[7] != 0xfe) return 0;
    uint64_t fat = (uint64_t)(s[4] | s[5] << 8), start = su_rd32(s + 0xc);
    if (!su_range(size, fat, 12) || memcmp(s + fat, "SFAT", 4)) return 0;
    unsigned count = (unsigned)(s[fat + 6] | s[fat + 7] << 8);
    if (i >= count || !su_range(size, fat + 12 + 16 * (uint64_t)i, 16)) return 0;
    const uint8_t *e = s + fat + 12 + 16 * (uint64_t)i; uint64_t a = su_rd32(e + 8), b = su_rd32(e + 12);
    if (b < a || !su_range(size, start + a, b - a)) { *data = NULL; *n = 0; return 1; }
    *data = s + start + a; *n = b - a; return 1;
}
static int setup_dictionaries(void) {   /* Pack/ZsDic.pack.zs: a SARC of the game's zstd dictionaries */
    uint8_t *pack = NULL; uint64_t n = 0;
    if (runtime_romfs_load("Pack/ZsDic.pack.zs", &pack, &n)) return 0;
    unsigned long long m = ZSTD_getFrameContentSize(pack, n);
    uint8_t *sarc = m && m < (1ull << 28) ? malloc(m) : NULL;
    if (!sarc || ZSTD_decompress(sarc, m, pack, n) != m) { free(sarc); free(pack); return 0; }
    const uint8_t *d; uint64_t dn;
    for (unsigned i = 0; setup.ndicts < 8 && setup_sarc_member(sarc, m, i, &d, &dn); i++) if (d && dn) {
        ZSTD_DDict *dict = ZSTD_createDDict(d, dn); if (!dict) continue;
        setup.dict_ids[setup.ndicts] = ZSTD_getDictID_fromDDict(dict); setup.dicts[setup.ndicts++] = dict; }
    free(sarc); free(pack); return setup.ndicts > 0;
}
/* The file's content: a zstd frame decompressed (with the dictionary it names), anything else as it is (NULL: a copy is
   not needed). *out owns the bytes when it is set. */
static const uint8_t *setup_decode(const uint8_t *raw, uint64_t n, uint64_t *m_out, uint8_t **out, ZSTD_DCtx *dctx) {
    *out = NULL; *m_out = n;
    if (n < 4 || memcmp(raw, "\x28\xb5\x2f\xfd", 4)) return raw;
    unsigned long long m = ZSTD_getFrameContentSize(raw, n);
    if (m == ZSTD_CONTENTSIZE_UNKNOWN || m == ZSTD_CONTENTSIZE_ERROR || m > (512ull << 20)) return NULL;
    unsigned id = ZSTD_getDictID_fromFrame(raw, n); ZSTD_DDict *dict = NULL;
    for (unsigned i = 0; i < setup.ndicts; i++) if (setup.dict_ids[i] == id) dict = setup.dicts[i];
    if (id && !dict) return NULL;
    uint8_t *b = malloc(m ? m : 1); if (!b) return NULL;
    size_t r = dict ? ZSTD_decompress_usingDDict(dctx, b, m, raw, n, dict) : ZSTD_decompressDCtx(dctx, b, m, raw, n);
    if (ZSTD_isError(r) || r != m) { free(b); return NULL; }
    __atomic_add_fetch(&setup.decoded_bytes, m, __ATOMIC_RELAXED);
    *out = b; *m_out = m; return b;
}
static uint32_t setup_key_len(const uint8_t *control, uint64_t n) {   /* prepare_shader_seed.py control_key_len */
    uint32_t size = n >= 0x24 ? su_rd32(control + 0x20) : 0;
    return size >= 0x7cc && size < 0x880 ? size : 0x880;
}
/* One shader found: kept once by its id. control is the key_len bytes, code the program's bytes up to its length. */
static void setup_add(int st, const uint8_t *control, uint32_t key_len, const uint8_t *code, uint32_t length) {
    char id[65]; uint8_t md[32]; dm_android_sha256_context c; dm_android_sha256_init(&c);
    dm_android_sha256_update(&c, control, key_len); dm_android_sha256_update(&c, code, length); dm_android_sha256_final(md, &c); totk_hex(md, 32, id);
    unsigned b = (unsigned)(su_rd32(md) & 8191);
    pthread_mutex_lock(&setup.lock);
    for (setup_shader *s = setup.table[b]; s; s = s->next) if (!strcmp(s->id, id)) { pthread_mutex_unlock(&setup.lock); return; }
    setup_shader *s = calloc(1, sizeof *s);
    /* the device's request reads the 0x880-byte control (local size at +0x748) and the code from its NVN header */
    if (s) { s->control = calloc(1, 0x880); s->code = malloc(length); }
    if (!s || !s->control || !s->code) die("shader setup: memory");
    memcpy(s->id, id, 65); s->st = st; memcpy(s->control, control, key_len); s->control_size = key_len; memcpy(s->code, code, length); s->code_size = length;
    s->next = setup.table[b]; setup.table[b] = s; setup.count++;
    pthread_mutex_unlock(&setup.lock);
}
/* A program's stage as NVN sees it (prepare_shader_seed.py bnsh_shaders / sharcb_shaders checks): the stage at +0x714
   and the code bounds at +0x700 (c1) / +0x704 (length) */
static int setup_stage_ok(const uint8_t *control, uint32_t key_len, uint64_t code_avail, int want, uint32_t *length) {
    if (key_len < 0x718) return 0;
    uint32_t c1 = su_rd32(control + 0x700), len = su_rd32(control + 0x704), st = su_rd32(control + 0x714);
    if (want >= 0 && (int)st != want) return -1;
    if (st != 0 && st != 1 && st != 5) return 0;
    if (!len || c1 > len || len > code_avail) return want >= 0 ? -1 : 0;
    *length = len; return 1;
}
static void setup_bnsh(const uint8_t *d, uint64_t n) {   /* one BNSH file (offsets relative to it) */
    if (n < 0x88 || memcmp(d, "BNSH", 4) || d[12] != 0xff || d[13] != 0xfe || memcmp(d + 96, "grsc", 4)) return;
    uint64_t size = su_rd32(d + 28); if (size > n) return; n = size;
    uint32_t count = su_rd32(d + 124); uint64_t variations = su_rd64(d + 128);
    if (!su_range(n, variations, (uint64_t)count * 64)) return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t program = su_rd64(d + variations + (uint64_t)i * 64 + 16); if (!su_range(n, program, 56)) continue;
        static const struct { int rel, st; } stages[] = {{8, 0}, {40, 1}, {48, 5}};
        for (int k = 0; k < 3; k++) {
            uint64_t at = su_rd64(d + program + stages[k].rel); if (!at || !su_range(n, at + 8, 24)) continue;
            uint64_t control_off = su_rd64(d + at + 8), code_off = su_rd64(d + at + 16); uint32_t code_size = su_rd32(d + at + 24), control_size = su_rd32(d + at + 28);
            if (!su_range(n, control_off, control_size) || !su_range(n, code_off, code_size) || control_size < 0x7cc) continue;   /* the SDK's older layout */
            const uint8_t *control = d + control_off; uint32_t key_len = setup_key_len(control, control_size), length;
            if (control_size < key_len) continue;
            if (setup_stage_ok(control, key_len, code_size, stages[k].st, &length) == 1) setup_add(stages[k].st, control, key_len, d + code_off, length);
        }
    }
}
static void setup_sharcb(const uint8_t *d, uint64_t n) {   /* agl shader package (BAHS): prepare_shader_seed.py sharcb_shaders */
    if (n < 32 || memcmp(d, "BAHS", 4)) return;
    uint64_t start = su_rd32(d + 20), pool_size = su_rd32(d + 24), pool = su_rd32(d + 28);
    if (start < 32 || start > pool || !su_range(n, pool, pool_size)) return;
    static const uint32_t sizes[] = {0x880, 0x88c, 0x7d1, 0x848};
    for (int z = 0; z < 4; z++)
        for (uint64_t at = start; at + 4 <= pool; at++) {
            if (su_rd32(d + at) != sizes[z] || at < start + 8 || (at - 8) % 4) continue;
            uint64_t record = at - 8, code_off = su_rd64(d + record); int32_t rel; memcpy(&rel, d + record + 12, 4);
            int64_t control_at = (int64_t)record + rel;
            if (control_at < (int64_t)start || (uint64_t)control_at + 0x24 > pool) continue;
            const uint8_t *control = d + control_at; uint32_t key_len = setup_key_len(control, pool - (uint64_t)control_at), length;
            if ((uint64_t)control_at + key_len > pool || code_off >= pool_size) continue;
            if (setup_stage_ok(control, key_len, pool_size - code_off, -1, &length) == 1) setup_add((int)su_rd32(control + 0x714), control, key_len, d + pool + code_off, length);
        }
}
static void setup_visit(const uint8_t *d, uint64_t n, int depth, ZSTD_DCtx *dctx) {
    if (n >= 4 && !memcmp(d, "BAHS", 4)) setup_sharcb(d, n);
    if (n >= 4 && !memcmp(d, "SARC", 4)) {
        if (depth > 4) return;
        const uint8_t *m; uint64_t mn;
        for (unsigned i = 0; setup_sarc_member(d, n, i, &m, &mn); i++) if (m) {
            uint8_t *own; uint64_t dn; const uint8_t *dd = setup_decode(m, mn, &dn, &own, dctx);
            if (dd) setup_visit(dd, dn, depth + 1, dctx);
            free(own); }
        return;
    }
    /* BFSHA and BNSH files, effect and layout resources: complete BNSH files inside */
    for (uint64_t pos = 0; pos + 32 <= n;) {
        const uint8_t *hit = memmem(d + pos, n - pos, "BNSH\0\0\0\0", 8); if (!hit) break;
        pos = (uint64_t)(hit - d); uint64_t size = su_rd32(d + pos + 28);
        if (su_range(n, pos, size)) setup_bnsh(d + pos, size);
        pos += size > 8 ? size : 8;
    }
}
static void *setup_scan_worker(void *unused) {
    (void)unused; setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    for (;;) {
        size_t i = __atomic_fetch_add(&setup.next_file, 1, __ATOMIC_RELAXED); if (i >= setup.nfiles) break;
        const setup_file *f = &setup.files[i];
        uint8_t *raw = setup_read(f->offset, f->size); if (!raw) continue;
        int container = f->size >= 4 && (!memcmp(raw, "\x28\xb5\x2f\xfd", 4) || !memcmp(raw, "SARC", 4) || !memcmp(raw, "FSHA", 4) || !memcmp(raw, "BNSH", 4) || !memcmp(raw, "BAHS", 4));
        if (container || memmem(raw, f->size, "BNSH\0\0\0\0", 8)) {
            uint8_t *own; uint64_t n; const uint8_t *d = setup_decode(raw, f->size, &n, &own, dctx);
            if (d) setup_visit(d, n, 0, dctx);
            free(own); }
        free(raw);
    }
    ZSTD_freeDCtx(dctx);
    return NULL;
}
/* The modules' embedded BNSH files (<data>/rewritten/<name>.patched.bin) */
static void setup_modules(const char *dir) {
    DIR *d = opendir(dir); if (!d) return; struct dirent *e; ZSTD_DCtx *dctx = ZSTD_createDCtx();
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name); if (l < 12 || strcmp(e->d_name + l - 12, ".patched.bin")) continue;
        char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int fd = open(path, O_RDONLY); struct stat st; if (fd < 0) continue;
        if (!fstat(fd, &st) && st.st_size > 0) { uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) { setup_visit(m, (uint64_t)st.st_size, 0, dctx); munmap(m, (size_t)st.st_size); } }
        close(fd);
    }
    ZSTD_freeDCtx(dctx); closedir(d);
}
/* The same from the player's own files: there are no images on disk, so each module is rewritten again here (about 60 ms) */
static void setup_player_modules(void) {
    static const char *const names[3] = {"main", "subsdk0", "sdk"}; ZSTD_DCtx *dctx = ZSTD_createDCtx();
    for (int i = 0; i < 3; i++) { size_t n; uint8_t *m = player_module_copy(names[i], &n); if (m) { setup_visit(m, n, 0, dctx); free(m); } }
    ZSTD_freeDCtx(dctx);
}
/* A translator process of the setup's own (totk_service_start's, with its own pipes) */
static int setup_service(FILE **in, FILE **out, pid_t *child) {
    char exe[1100]; const char *e = getenv("TOTK_XLATE");
    if (e && *e) snprintf(exe, sizeof exe, "%s", e);
    else { Dl_info info; if (!dladdr((void *)setup_service, &info) || !info.dli_fname) return 0;
        char lib[1024]; snprintf(lib, sizeof lib, "%s", info.dli_fname); snprintf(exe, sizeof exe, "%s/libtotk_xlate.so", dirname(lib)); }
    /* close-on-exec: another translator inheriting these ends would keep this one's input open after it is closed */
    int to[2], from[2]; if (pipe2(to, O_CLOEXEC)) return 0; if (pipe2(from, O_CLOEXEC)) { close(to[0]); close(to[1]); return 0; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0); posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addclose(&fa, to[1]); posix_spawn_file_actions_addclose(&fa, from[0]);
    char *argv[] = {exe, "--service", "--spirv-only", "--out", (char *)totk_cache_dir(), "--chain", "android-vk1", NULL};
    char *env[256]; int ne = 0; for (char **v = environ; *v && ne < 255; v++) if (strncmp(*v, "LD_PRELOAD=", 11)) env[ne++] = *v;
    env[ne] = NULL;
    pid_t pid; int r = posix_spawn(&pid, exe, &fa, NULL, argv, env);   /* the child keeps this thread's low priority */
    posix_spawn_file_actions_destroy(&fa); close(to[0]); close(from[1]);
    if (r) { close(to[1]); close(from[0]); return 0; }
    *in = fdopen(to[1], "w"); *out = fdopen(from[0], "r"); *child = pid;
    return *in && *out;
}
static void setup_service_end(FILE **in, FILE **out, pid_t child) {   /* end of input: it exits; reaped here */
    if (*in) fclose(*in);
    if (*out) fclose(*out);
    *in = *out = NULL; if (child > 0) waitpid(child, NULL, 0);
}
/* The variant made for a shader joins its .variants list, and the runtime's table if the shader is loaded there
   (totk_shader_of reads the file only once). */
static void setup_register(const char *id, const char *key) {
    char path[1200];
    pthread_mutex_lock(&totk_shader_lock);
    unsigned b = (unsigned)strtoul((char[9]){id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], 0}, NULL, 16) & 1023;
    for (totk_shader *s = totk_shaders[b]; s; s = s->next) if (!strcmp(s->id, id)) {
        for (totk_variant *v = s->variants; v; v = v->next) if (!strcmp(v->key, key)) goto listed;
        snprintf(path, sizeof path, "%s/%s.deps", totk_cache_dir(), key); char *deps = totk_read_text(path);
        totk_variant *v = deps ? calloc(1, sizeof *v) : NULL;
        if (v) { memcpy(v->key, key, 17); v->deps = deps; v->mask = totk_deps_mask(deps); v->next = s->variants; s->variants = v; } else free(deps);
        break; }
    snprintf(path, sizeof path, "%s/%s.variants", totk_cache_dir(), id);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", key); fclose(f); }
listed:
    pthread_mutex_unlock(&totk_shader_lock);
}
static const char setup_answers[] = "\"tex\": {}, \"img\": {}, \"attr\": {}, \"fout\": {}";   /* the default bind state (aot_shaders.py) */
static void *setup_translate_worker(void *unused) {
    (void)unused; setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    FILE *in = NULL, *out = NULL; pid_t child = 0; const char *dir = totk_cache_dir();
    for (;;) {
        size_t j = __atomic_fetch_add(&setup.next_job, 1, __ATOMIC_RELAXED); if (j >= setup.count) break;
        const setup_shader *s = setup.list[j];
        uint8_t md[32]; dm_android_sha256_context c; dm_android_sha256_init(&c);
        dm_android_sha256_update(&c, s->id, 64); dm_android_sha256_update(&c, setup_answers, sizeof setup_answers - 1); dm_android_sha256_final(md, &c);
        char key[17], path[1200]; totk_hex(md, 8, key);
        snprintf(path, sizeof path, "%s/%s.variants", dir, s->id);
        { struct stat st; if (!stat(path, &st) && st.st_size >= 17) { __atomic_add_fetch(&setup.present, 1, __ATOMIC_RELAXED); continue; } }   /* the shader has a variant already */
        if (!in && !setup_service(&in, &out, &child)) { LOG("[shader-setup] translator did not start"); break; }
        uint32_t length = s->code_size, c1o = su_rd32(s->control + 0x700); size_t skip = s->st == 5 ? 0 : 0x30;
        if (length <= skip) { __atomic_add_fetch(&setup.failed, 1, __ATOMIC_RELAXED); continue; }
        snprintf(path, sizeof path, "%s/%s.code", dir, key);
        FILE *f = fopen(path, "wb"); if (!f) break;
        int wrote = fwrite(s->code + skip, 1, length - skip, f) == length - skip; fclose(f); if (!wrote) break;
        char *c1 = malloc(2 * (size_t)(length - c1o) + 1); if (!c1) break; totk_hex(s->code + c1o, length - c1o, c1);
        char local[64] = "";
        if (s->st == 5) { uint32_t l[3]; memcpy(l, s->control + 0x748, 12); snprintf(local, sizeof local, ", \"local\": [%u, %u, %u]", l[0], l[1], l[2]); }
        char constants[160]; totk_request_constants(s->st, constants, sizeof constants);
        fprintf(in, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": \"%s\", \"c1\": \"%s\", %s%s, %s}\n",
                key, s->st == 0 ? "vs" : s->st == 1 ? "ps" : "cs", path, c1, setup_answers, local, constants);
        fflush(in); free(c1);
        char reply[2048]; if (!fgets(reply, sizeof reply, out)) { LOG("[shader-setup] translator exited at %s", key); setup_service_end(&in, &out, child); unlink(path); continue; }
        unlink(path);
        if (!strstr(reply, "\"ok\": true")) { if (__atomic_add_fetch(&setup.failed, 1, __ATOMIC_RELAXED) <= 5) LOG("[shader-setup] %s (%.16s) failed: %.200s", key, s->id, reply); continue; }
        snprintf(path, sizeof path, "%s/%s.aot", dir, key); f = fopen(path, "w"); if (f) fclose(f);   /* the warm-up skips it until used */
        setup_register(s->id, key);
        if (!(__atomic_add_fetch(&setup.translated, 1, __ATOMIC_RELAXED) % 1000))
            LOG("[shader-setup] %llu translated, %llu there already, %llu failed, of %zu", (unsigned long long)setup.translated, (unsigned long long)setup.present, (unsigned long long)setup.failed, setup.count);
    }
    setup_service_end(&in, &out, child);
    return NULL;
}
static int setup_order(const void *a, const void *b) { return strcmp((*(setup_shader *const *)a)->id, (*(setup_shader *const *)b)->id); }
static void *setup_thread(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-shsetup"); setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    const char *romfs = getenv("TOTK_ROMFS"), *game_data = getenv("TOTK_GAME_DATA"), *dir = totk_cache_dir();
    char marker[1100], want[256]; snprintf(marker, sizeof marker, "%s/setup.txt", dir);
    { char path[1100]; struct stat st; snprintf(path, sizeof path, "%s/runtime-index.bin", romfs ? romfs : "");
      if (!romfs || stat(path, &st)) { LOG("[shader-setup] no RomFS index: skipped"); return NULL; }
      snprintf(path, sizeof path, "%s/xlate-revision.txt", dir); char *rev = totk_read_text(path);
      snprintf(want, sizeof want, "index %lld revision %d\n", (long long)st.st_size, rev ? atoi(rev) : 0); free(rev); }
    const char *mode = getenv("TOTK_SHADER_SETUP"); int force = mode && atoi(mode) == 2;
    { char *have = totk_read_text(marker); int done = have && !strcmp(have, want); free(have);
      if (done && !force) { LOG("[shader-setup] done before (%s)", marker); return NULL; } }
    if (!setup_index(romfs) || !setup_dictionaries()) { LOG("[shader-setup] RomFS or zstd dictionaries unreadable: skipped"); return NULL; }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); int scanners = cpus > 2 ? (int)cpus - 2 : 1; if (scanners > 6) scanners = 6;
    pthread_t t[8];
    for (int i = 0; i < scanners; i++) pthread_create(&t[i], NULL, setup_scan_worker, NULL);
    for (int i = 0; i < scanners; i++) pthread_join(t[i], NULL);
    if (player_on && player_on()) setup_player_modules();
    else { if (game_data) { char modules[1100]; snprintf(modules, sizeof modules, "%s/../rewritten", game_data); setup_modules(modules); }
        close(setup.fd); setup.fd = -1; }
    setup.list = malloc((setup.count ? setup.count : 1) * sizeof *setup.list); if (!setup.list) return NULL;
    size_t k = 0; for (unsigned b = 0; b < 8192; b++) for (setup_shader *s = setup.table[b]; s; s = s->next) setup.list[k++] = s;
    qsort(setup.list, setup.count, sizeof *setup.list, setup_order);
    uint64_t t1 = dm_android_clock_ns(CLOCK_MONOTONIC);
    LOG("[shader-setup] %zu shaders in %zu RomFS files (%.0f MB read, %.0f MB decompressed) in %.1f s", setup.count, setup.nfiles,
        setup.scanned_bytes / 1e6, setup.decoded_bytes / 1e6, (t1 - t0) / 1e9);
    { const char *list = getenv("TOTK_SHADER_SETUP_LIST");   /* the ids found, one a line (checked against the PC inventory) */
      FILE *f = list && *list ? fopen(list, "w") : NULL; if (f) { for (size_t i = 0; i < setup.count; i++) fprintf(f, "%s %d\n", setup.list[i]->id, setup.list[i]->st); fclose(f); } }
    int workers = getenv("TOTK_SHADER_SETUP_JOBS") ? atoi(getenv("TOTK_SHADER_SETUP_JOBS")) : 3; if (workers < 1) workers = 1; if (workers > 8) workers = 8;
    for (int i = 0; i < workers; i++) pthread_create(&t[i], NULL, setup_translate_worker, NULL);
    for (int i = 0; i < workers; i++) pthread_join(t[i], NULL);
    if (setup.translated + setup.present + setup.failed >= setup.count) { FILE *f = fopen(marker, "w"); if (f) { fputs(want, f); fclose(f); } }
    /* what the scan kept (~100 MB of control sections and code) is not needed in play */
    for (size_t i = 0; i < setup.count; i++) { free(setup.list[i]->control); free(setup.list[i]->code); free(setup.list[i]); }
    free(setup.list); setup.list = NULL; memset(setup.table, 0, sizeof setup.table); free(setup.files); setup.files = NULL;
    LOG("[shader-setup] finished in %.0f s: %llu translated, %llu there already, %llu failed, of %zu", (dm_android_clock_ns(CLOCK_MONOTONIC) - t0) / 1e9,
        (unsigned long long)setup.translated, (unsigned long long)setup.present, (unsigned long long)setup.failed, setup.count);
    return NULL;
}
/* From the first present (the device, the translator's request constants and the game's depth mode are known). */
static int prepare_mode(void); static void prepare_start(void);   /* nvn_vulkan_prepare.inc */
static void shader_setup_start(void) {
    static int started; if (started) return; started = 1;
    if (prepare_mode()) { prepare_start(); return; }   /* the setup runs there, then the libraries */
    const char *e = getenv("TOTK_SHADER_SETUP"); if (e && !atoi(e)) return;
    pthread_t t; if (!pthread_create(&t, NULL, setup_thread, NULL)) pthread_detach(t);
}
