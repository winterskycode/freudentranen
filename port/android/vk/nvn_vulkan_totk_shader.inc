// TOTK runtime shader variants.
// A shader is SHA-256(control section [ctl_key_len] + code) as in MacNativeGames' nvn_mtl.m prog_get; each translation
// ("variant") depends on the translator's answers from the bound state (texture targets, image formats, vertex input and
// colour output component classes; rt_answers there). Variants live in the shader cache (DM_VK_SHADERS) as
// <key16>.spv / .meta (the D2R loader's files) / .deps, listed per shader in <shader>.variants; a miss is translated by
// the service (totk_xlate --service --spirv-only, port/xlate) and cached. The translation blocks the submit thread.
#include <dlfcn.h>
#include <libgen.h>
#include <spawn.h>
#include <sys/stat.h>
extern char **environ;
int totk_depth_near_minus_w(void);

typedef struct { const char *tex[128]; char img[32][48]; uint8_t has_img[32]; const char *attr[16]; const char *fout[8]; } totk_answers;
/* the slots a variant's .deps read: state elsewhere cannot change which variant matches (totk_state_sig) */
typedef struct { uint64_t tex[2]; uint32_t img; uint16_t attr; uint8_t fout; } totk_mask;
typedef struct totk_variant { char key[17]; char *deps; totk_mask mask; uint8_t used; struct totk_variant *next; } totk_variant;
/* memo: answers hash -> matched variant key (deps matching parses every variant's .deps with sscanf) */
typedef struct totk_shader { char id[65]; totk_variant *variants; struct totk_shader *next; struct { uint64_t h; char key[17]; totk_mask mask; } memo[8]; unsigned memo_next; } totk_shader;
static totk_shader *totk_shaders[1024];
static pthread_mutex_t totk_shader_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *totk_xl_in, *totk_xl_out;
static uint64_t totk_translated;

static int totk_request_constants(int st, char *out, size_t cap);
static const char *totk_cache_dir(void) {
    static char dir[1024];
    if (dir[0]) return dir;
    const char *e = getenv("DM_VK_SHADERS");
    if (!e || !*e) die("DM_VK_SHADERS (the shader cache) is not set");
    /* with TOTK_VK_BINDLESS, the bindless cache (bindless_on set DM_VK_SHADERS) */
    snprintf(dir, sizeof dir, "%s", e);
    mkdir(dir, 0700);
    // the D2R loader's profile check
    char path[1100]; snprintf(path, sizeof path, "%s/host-subgroup-size.txt", dir);
    FILE *f = fopen(path, "w"); if (!f) die("shader cache %s not writable", dir);
    fprintf(f, "%u\n", gpu_subgroup_size); fclose(f);
    snprintf(path, sizeof path, "%s/request-profile.json", dir);
    if ((f = fopen(path, "w"))) { char c[160];
        fprintf(f, "{\"chain\": \"android-vk1\"");
        static const struct { int st; const char *name; } stages[] = {{0, "vs"}, {1, "ps"}, {5, "cs"}};
        for (int i = 0; i < 3; i++) { totk_request_constants(stages[i].st, c, sizeof c); fprintf(f, ", \"%s\": {%s}", stages[i].name, c); }
        fprintf(f, "}\n"); fclose(f); }
    return dir;
}

/* NVN format -> component class and image format name, NVN texture target -> translator type (nvn_mtl.m). */
static const char *totk_fmt_class(int f) {
    /* *_UI2F / *_I2F vertex formats, see vk_format */
    if (f >= 0x51 && f <= 0x68) return (f - 0x51) & 1 ? "sscaled" : "uscaled";
    switch (f) {
        case 0x6B: return "uscaled";
        case 0x6C: return "sscaled";
        case 0x6A: return "sint";
        case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: return "uint";
        case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: return "sint";
        default: return "float";
    }
}
static const char *totk_target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *totk_image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
        default: return NULL;
    }
}
static void totk_answers_build(const nvn_state *ns, int st, totk_answers *a) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < 128; r++) if (ns->textures[st][r]) {
        texture_descriptor d = descriptor_texture((int)(ns->textures[st][r] & 0xfffff)); if (!d.valid) continue;
        a->tex[r] = totk_target_name(d.view.set_target ? d.view.target : d.texture.d.target);
    }
    for (int r = 0; r < 32; r++) if (ns->images[st][r]) {
        texture_descriptor d = descriptor_texture((int)(ns->images[st][r] & 0xfffff)); if (!d.valid) continue;
        const char *fmt = totk_image_format(d.view.set_format ? d.view.format : d.texture.d.format);
        snprintf(a->img[r], sizeof a->img[r], "%s %s", totk_target_name(d.view.set_target ? d.view.target : d.texture.d.target), fmt ? fmt : "-");
        a->has_img[r] = 1;
    }
    if (st == 0) for (int i = 0; i < 16; i++) if (ns->attributes[i].fmt) a->attr[i] = totk_fmt_class(ns->attributes[i].fmt);
    if (st == 1) for (int i = 0; i < ns->rt.n && i < 8; i++) if (ns->rt.color[i])
        a->fout[i] = totk_fmt_class(ns->rt.has_cview[i] && ns->rt.cview[i].set_format ? ns->rt.cview[i].format : ns->rt.color[i]->d.format);
}
/* The answers as the service reads them (JSON objects keyed by slot); also the text the variant name is hashed from. */
static size_t totk_answers_json(const totk_answers *a, char *out, size_t cap) {
    size_t n = 0;
#define TA_PUT(...) do { int w_ = snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__); if (w_ > 0) n += (size_t)w_; } while (0)
    TA_PUT("\"tex\": {"); int first = 1;
    for (int r = 0; r < 128; r++) if (a->tex[r]) { TA_PUT("%s\"%d\": \"%s\"", first ? "" : ", ", r, a->tex[r]); first = 0; }
    TA_PUT("}, \"img\": {"); first = 1;
    for (int r = 0; r < 32; r++) if (a->has_img[r]) {
        char type[16], fmt[32]; if (sscanf(a->img[r], "%15s %31s", type, fmt) != 2) continue;
        if (strcmp(fmt, "-")) TA_PUT("%s\"%d\": {\"type\": \"%s\", \"fmt\": \"%s\"}", first ? "" : ", ", r, type, fmt);
        else TA_PUT("%s\"%d\": {\"type\": \"%s\"}", first ? "" : ", ", r, type);
        first = 0;
    }
    TA_PUT("}, \"attr\": {"); first = 1;
    for (int i = 0; i < 16; i++) if (a->attr[i]) { TA_PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, a->attr[i]); first = 0; }
    TA_PUT("}, \"fout\": {"); first = 1;
    for (int i = 0; i < 8; i++) if (a->fout[i]) { TA_PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, a->fout[i]); first = 0; }
    TA_PUT("}");
#undef TA_PUT
    if (n >= cap) die("shader answers too long");
    return n;
}
/* Does a variant's .deps (lines "tex 3 2d", "img 1 2d R8G8B8A8Unorm", "attr 0 float", "fout 0 sint") hold for these answers?
   An unbound slot answers the translator's default (2d / no image / float), as in nvn_mtl.m deps_match. */
static int totk_deps_match(const char *deps, const totk_answers *a) {
    for (const char *p = deps; p && *p;) {
        char kind[8], v1[24], v2[32]; int slot; int k = sscanf(p, "%7s %d %23s %31s", kind, &slot, v1, v2);
        if (k >= 3) {
            if (!strcmp(kind, "tex")) { const char *h = slot >= 0 && slot < 128 && a->tex[slot] ? a->tex[slot] : "2d"; if (strcmp(h, v1)) return 0; }
            else if (!strcmp(kind, "attr")) { const char *h = slot >= 0 && slot < 16 && a->attr[slot] ? a->attr[slot] : "float"; if (strcmp(h, v1)) return 0; }
            else if (!strcmp(kind, "fout")) { const char *h = slot >= 0 && slot < 8 && a->fout[slot] ? a->fout[slot] : "float"; if (strcmp(h, v1)) return 0; }
            else if (!strcmp(kind, "img") && k == 4) {
                char want[64]; snprintf(want, sizeof want, "%s %s", v1, v2);
                const char *h = slot >= 0 && slot < 32 && a->has_img[slot] ? a->img[slot] : "- -"; if (strcmp(h, want)) return 0;
            }
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return 1;
}
static totk_mask totk_deps_mask(const char *deps) {
    totk_mask m = {0};
    for (const char *p = deps; p && *p;) {
        char kind[8]; int slot;
        if (sscanf(p, "%7s %d", kind, &slot) == 2 && slot >= 0) {
            if (!strcmp(kind, "tex") && slot < 128) m.tex[slot >> 6] |= 1ull << (slot & 63);
            else if (!strcmp(kind, "img") && slot < 32) m.img |= 1u << slot;
            else if (!strcmp(kind, "attr") && slot < 16) m.attr |= (uint16_t)(1u << slot);
            else if (!strcmp(kind, "fout") && slot < 8) m.fout |= (uint8_t)(1u << slot);
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return m;
}
static char *totk_read_text(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *s = malloc((size_t)n + 1); if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f); s[n] = 0; return s;
}
// caller holds totk_shader_lock
static totk_shader *totk_shader_of(const char *id) {
    unsigned b = (unsigned)strtoul((char[9]){id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], 0}, NULL, 16) & 1023;
    for (totk_shader *s = totk_shaders[b]; s; s = s->next) if (!strcmp(s->id, id)) return s;
    totk_shader *s = calloc(1, sizeof *s); if (!s) die("shader table");
    snprintf(s->id, sizeof s->id, "%s", id); s->next = totk_shaders[b]; totk_shaders[b] = s;
    char path[1200]; snprintf(path, sizeof path, "%s/%s.variants", totk_cache_dir(), id);
    char *index = totk_read_text(path);
    for (char *line = index ? strtok(index, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        if (strlen(line) != 16) continue;
        snprintf(path, sizeof path, "%s/%s.deps", totk_cache_dir(), line);
        // incomplete translation: translated again on demand
        char *deps = totk_read_text(path); if (!deps) continue;
        totk_variant *v = calloc(1, sizeof *v); if (!v) die("variant table");
        memcpy(v->key, line, 16); v->deps = deps; v->mask = totk_deps_mask(deps); v->next = s->variants; s->variants = v;
    }
    free(index);
    return s;
}
static void totk_service_start(void) {
    if (totk_xl_in) return;
    char exe[1100]; const char *e = getenv("TOTK_XLATE");
    if (e && *e) snprintf(exe, sizeof exe, "%s", e);
    // the APK ships the translator beside libtotk.so as libtotk_xlate.so (an executable)
    else {
        Dl_info info; if (!dladdr((void *)totk_service_start, &info) || !info.dli_fname) die("translator: own library path unknown");
        char lib[1024]; snprintf(lib, sizeof lib, "%s", info.dli_fname); snprintf(exe, sizeof exe, "%s/libtotk_xlate.so", dirname(lib));
    }
    /* not inherited by the setup's translators (nvn_vulkan_shader_setup.inc) */
    int to[2], from[2]; if (pipe2(to, O_CLOEXEC) || pipe2(from, O_CLOEXEC)) die("translator pipes");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0); posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addclose(&fa, to[1]); posix_spawn_file_actions_addclose(&fa, from[0]);
    char *argv[] = {exe, "--service", "--spirv-only", "--out", (char *)totk_cache_dir(), "--chain", "android-vk1", NULL};
    /* The app process's environment preloads ART's libsigchain (LD_PRELOAD), which aborts the .NET runtime when it resets
       its fault handlers: the translator gets the environment without it. */
    char *env[256]; int ne = 0;
    for (char **v = environ; *v && ne < 255; v++) if (strncmp(*v, "LD_PRELOAD=", 11)) env[ne++] = *v;
    env[ne] = NULL;
    pid_t pid; int r = posix_spawn(&pid, exe, &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    if (r) die("start shader translator %s: %s", exe, strerror(r));
    close(to[0]); close(from[1]);
    totk_xl_in = fdopen(to[1], "w"); totk_xl_out = fdopen(from[0], "r");
    if (!totk_xl_in || !totk_xl_out) die("translator streams");
    LOG("[shader] translator started (pid %d): %s, cache %s", pid, exe, totk_cache_dir());
}
/* The request fields that depend on the device and the game's setup, not on the shader: the PC's ahead-of-time translation
   (tools/aot_shaders.py) reads them from <cache>/request-profile.json, written when the cache opens, so its variants are
   the ones this device would make. */
static int totk_request_constants(int st, char *out, size_t cap) {
    return snprintf(out, cap, "\"depth_minus_w\": %s, \"host_subgroup_size\": %u, \"vs_subgroups\": %s%s", totk_depth_near_minus_w() ? "true" : "false",
        gpu_subgroup_size,
        gpu_vs_subgroups ? "true" : "false", bindless_on() ? ", \"bindless\": true" : "");
}
static void totk_hex(const uint8_t *p, size_t n, char *out) { static const char h[] = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[2 * i] = h[p[i] >> 4]; out[2 * i + 1] = h[p[i] & 15]; } out[2 * n] = 0; }

/* The cached shader entry of program stage `index` for the bound state, translating the variant if needed. */
/* The shader's identity: SHA-256 of the control section and the code (computed once per program stage). */
static void totk_shader_id(const uint8_t *control, const uint8_t *code, int st, char id[65]) {
    uint32_t length, c1o; memcpy(&length, control + 0x704, 4); memcpy(&c1o, control + 0x700, 4);
    if (!length || length > 16u << 20 || c1o > length || (st != 5 && c1o < 0x30)) die("shader code area %#x / c1 %#x", length, c1o);
    uint32_t key_len; memcpy(&key_len, control + 0x20, 4); if (!(key_len >= 0x7cc && key_len < 0x880)) key_len = 0x880;
    uint8_t md[32]; dm_android_sha256_context c; dm_android_sha256_init(&c); dm_android_sha256_update(&c, control, key_len);
    dm_android_sha256_update(&c, code, length); dm_android_sha256_final(md, &c);
    totk_hex(md, 32, id);
}
static shader_entry *totk_named(shader_entry *s, const char *id) { if (s && !s->game_id[0]) memcpy(s->game_id, id, 16); return s; }
static shader_entry *totk_variant_shader(const nvn_state *ns, const uint8_t *control, const uint8_t *code, int st, const char *id, totk_mask *mask) {
    uint32_t length, c1o; memcpy(&length, control + 0x704, 4); memcpy(&c1o, control + 0x700, 4);
    uint8_t md[32]; dm_android_sha256_context c;
    totk_answers a; totk_answers_build(ns, st, &a);
    pthread_mutex_lock(&totk_shader_lock);
    totk_shader *s = totk_shader_of(id);
    /* the answers' strings are static names except img (text): hash pointers and img text */
    uint64_t ah = 1469598103934665603ull;
    for (int r = 0; r < 128; r++) { ah ^= (uint64_t)(uintptr_t)a.tex[r]; ah *= 1099511628211ull; }
    for (int r = 0; r < 32; r++) if (a.has_img[r]) for (const char *q = a.img[r]; *q; q++) { ah ^= (uint64_t)(r * 256 + (uint8_t)*q); ah *= 1099511628211ull; }
    for (int i = 0; i < 16; i++) { ah ^= (uint64_t)(uintptr_t)a.attr[i]; ah *= 1099511628211ull; }
    for (int i = 0; i < 8; i++) { ah ^= (uint64_t)(uintptr_t)a.fout[i]; ah *= 1099511628211ull; }
    if (!ah) ah = 1;
    for (int i = 0; i < 8; i++) if (s->memo[i].h == ah) { char key[17]; memcpy(key, s->memo[i].key, 17); *mask = s->memo[i].mask; pthread_mutex_unlock(&totk_shader_lock); return totk_named(shader_load_by_key(key, 1, 1), id); }
    for (totk_variant *v = s->variants; v; v = v->next)
        if (totk_deps_match(v->deps, &a)) { char key[17]; memcpy(key, v->key, 17);
            /* a variant translated ahead of time on the PC (tools/aot_shaders.py) carries <key>.aot until the game first
               uses it; the session warm-up (pso_preload_shaders) loads only variants without one */
            if (!v->used) { v->used = 1; char aot[1200]; snprintf(aot, sizeof aot, "%s/%s.aot", totk_cache_dir(), key); unlink(aot); }
            s->memo[s->memo_next].h = ah; memcpy(s->memo[s->memo_next].key, key, 17); s->memo[s->memo_next].mask = *mask = v->mask; s->memo_next = (s->memo_next + 1) & 7;
            pthread_mutex_unlock(&totk_shader_lock); return totk_named(shader_load_by_key(key, 1, 1), id); }
    // translate: the variant name hashes the shader and its full answers
    char answers[16384]; size_t an = totk_answers_json(&a, answers, sizeof answers);
    dm_android_sha256_init(&c); dm_android_sha256_update(&c, id, 64); dm_android_sha256_update(&c, answers, an); dm_android_sha256_final(md, &c);
    char key[17]; totk_hex(md, 8, key);
    const char *dir = totk_cache_dir(); char path[1200];
    // graphics: the translator wants SPH + instructions, without the 0x30-byte NVN header
    size_t skip = st == 5 ? 0 : 0x30;
    snprintf(path, sizeof path, "%s/%s.code", dir, key);
    FILE *f = fopen(path, "wb"); if (!f || fwrite(code + skip, 1, length - skip, f) != length - skip) die("write shader code %s", path); fclose(f);
    char *c1 = malloc(2 * (size_t)(length - c1o) + 1); if (!c1) die("c1 hex"); totk_hex(code + c1o, length - c1o, c1);
    char local[64] = "";
    if (st == 5) { uint32_t l[3]; memcpy(l, control + 0x748, 12); if (!l[0] || !l[1] || !l[2]) die("compute shader %s: thread group size", id); snprintf(local, sizeof local, ", \"local\": [%u, %u, %u]", l[0], l[1], l[2]); }
    totk_service_start();
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    char constants[160]; totk_request_constants(st, constants, sizeof constants);
    fprintf(totk_xl_in, "{\"variant\": \"%s\", \"stage\": \"%s\", \"code\": \"%s\", \"c1\": \"%s\", %s%s, %s}\n",
            key, st == 0 ? "vs" : st == 1 ? "ps" : "cs", path, c1, answers, local, constants);
    fflush(totk_xl_in); free(c1);
    char reply[2048]; if (!fgets(reply, sizeof reply, totk_xl_out)) die("shader translator exited while translating %s", key);
    if (!strstr(reply, "\"ok\": true")) die("shader %s (%s) translation failed: %s", key, id, reply);
    snprintf(path, sizeof path, "%s/%s.deps", dir, key);
    totk_variant *v = calloc(1, sizeof *v); if (!v) die("variant table");
    memcpy(v->key, key, 17); v->deps = totk_read_text(path); if (!v->deps) die("shader %s: no .deps after translation", key);
    v->mask = *mask = totk_deps_mask(v->deps);
    v->next = s->variants; s->variants = v;
    snprintf(path, sizeof path, "%s/%s.variants", dir, id);
    f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", key); fclose(f); }
    totk_translated++;
    LOG("[shader] translated %llu: %s %s (shader %.16s) in %.1f ms", (unsigned long long)totk_translated, st == 0 ? "vs" : st == 1 ? "ps" : "cs", key, id,
        (dm_android_clock_ns(CLOCK_MONOTONIC) - t0) / 1e6);
    pthread_mutex_unlock(&totk_shader_lock);
    return totk_named(shader_load_by_key(key, 1, 1), id);
}

/* program_shader for TOTK: the stage comes from control+0x714 (0 vertex, 1 fragment, 5 compute). Per program and stage
   the last variant is kept with a signature of the state its answers come from, so unchanged state skips the lookup.
   The stage's last few (variant, signature) pairs are kept too (recent): a stage whose state alternates (one draw's,
   then the next's) finds the pair whose signature matches again without the answers, their hash and the shader lookup
   under its lock (~1.7% of the replay thread). The same rule as the last variant: a variant whose .deps slots hold what
   they held when it matched matches again. */
typedef struct { uint64_t sig; totk_mask mask; shader_entry *shader; } totk_recent_variant;
typedef struct totk_program { const prog_t *program; uint64_t serial, sig[6]; shader_entry *shaders[6]; totk_mask mask[6]; char id[6][65]; totk_recent_variant recent[6][4]; unsigned recent_next[6]; struct totk_program *next; } totk_program;
static totk_program *totk_programs[1024];
static uint64_t totk_state_sig(const nvn_state *ns, int st, const totk_mask *m) {
    uint64_t h = 1469598103934665603ull;
#define TS_MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    /* only the slots the current variant's .deps read (m): elsewhere the state cannot change the match. Of a texture
       what the answers read (totk_answers_build): its kind, not its id, so other textures of the same kinds keep the
       variant. */
    for (int w = 0; w < 2; w++) for (uint64_t bits = m->tex[w]; bits; bits &= bits - 1) {
        int r = w * 64 + __builtin_ctzll(bits); uint32_t t = ns->textures[st][r];
        TS_MIX(r); TS_MIX(t ? atomic_load_explicit(&texture_kinds[t & 0xfffff], memory_order_relaxed) & 0xff : 0); }
    for (uint32_t bits = m->img; bits; bits &= bits - 1) {
        int r = __builtin_ctz(bits); uint32_t t = ns->images[st][r];
        TS_MIX(r + 1000); TS_MIX(t ? atomic_load_explicit(&texture_kinds[t & 0xfffff], memory_order_relaxed) : 0); }
    if (st == 0) for (uint32_t bits = m->attr; bits; bits &= bits - 1) TS_MIX(ns->attributes[__builtin_ctz(bits)].fmt);
    if (st == 1) for (uint32_t bits = m->fout; bits; bits &= bits - 1) { int i = __builtin_ctz(bits);
        TS_MIX(i < ns->rt.n && ns->rt.color[i] ? ns->rt.color[i]->d.format : 0); TS_MIX(i < ns->rt.n && ns->rt.has_cview[i] && ns->rt.cview[i].set_format ? ns->rt.cview[i].format : 0); }
#undef TS_MIX
    return h;
}
static void shader_enable_early_tests(shader_entry *s);
static shader_entry *program_shader_in(const nvn_state *ns, const prog_t *program, unsigned index) {
    if (index >= program->nstage || index >= 6) die("Vulkan program stage bounds");
    const uint8_t *control = program->sd[index].control, *code = (const uint8_t *)(uintptr_t)program->sd[index].data;
    if (!control || !code) die("TOTK program without shader binary");
    uint32_t nvn_stage; memcpy(&nvn_stage, control + 0x714, 4);
    if (nvn_stage != 0 && nvn_stage != 1 && nvn_stage != 5) die("TOTK shader stage %u has no translation chain", nvn_stage);
    unsigned b = ((uintptr_t)program >> 4) & 1023; totk_program *e = totk_programs[b];
    while (e && e->program != program) e = e->next;
    if (!e) { e = calloc(1, sizeof *e); if (!e) die("program table"); e->program = program; e->next = totk_programs[b]; totk_programs[b] = e; }
    if (e->serial != program->serial) { e->serial = program->serial; memset(e->shaders, 0, sizeof e->shaders); memset(e->id, 0, sizeof e->id); memset(e->recent, 0, sizeof e->recent); }
    if (!e->id[index][0]) totk_shader_id(control, code, (int)nvn_stage, e->id[index]);
    if (!e->shaders[index] || e->sig[index] != totk_state_sig(ns, (int)nvn_stage, &e->mask[index])) {
        totk_recent_variant *r = NULL;
        /* the current variant too, under another signature: textures of another kind its .deps accept alike */
        for (int i = 0; i < 4 && !r; i++) if (e->recent[index][i].shader &&
                                              e->recent[index][i].sig == totk_state_sig(ns, (int)nvn_stage, &e->recent[index][i].mask)) r = &e->recent[index][i];
        if (r) { e->shaders[index] = r->shader; e->mask[index] = r->mask; e->sig[index] = r->sig; return e->shaders[index]; }
        e->shaders[index] = totk_variant_shader(ns, control, code, (int)nvn_stage, e->id[index], &e->mask[index]);
        e->sig[index] = totk_state_sig(ns, (int)nvn_stage, &e->mask[index]);
        e->recent[index][e->recent_next[index]++ & 3] = (totk_recent_variant){ e->sig[index], e->mask[index], e->shaders[index] }; }
    if(nvn_stage==1&&e->shaders[index]&&!e->shaders[index]->early_tests){uint32_t flags;memcpy(&flags,control+0x718,4);if(flags&1)shader_enable_early_tests(e->shaders[index]);}
    /* diagnostics: each shader's NVN control section, once */
    { static int dump=-1;if(dump<0){const char *x=getenv("TOTK_DUMP_CONTROL");dump=x&&atoi(x);}
      if(dump&&e->shaders[index]&&!e->shaders[index]->control_dumped){const char *dir=getenv("TOTK_FILES_DIR");if(dir){char path[600];snprintf(path,sizeof path,"%s/control",dir);mkdir(path,0700);
          snprintf(path,sizeof path,"%s/control/%s.bin",dir,e->shaders[index]->key);FILE *f=fopen(path,"wb");if(f){fwrite(control,1,0x880,f);fclose(f);}}e->shaders[index]->control_dumped=1;} }
    return e->shaders[index];
}
static shader_entry *program_shader(const prog_t *program, unsigned index) { return program_shader_in(&state, program, index); }
/* The variant a program's stage resolved to last (shader s), with the mask and signature it was chosen by (nvn_vulkan_plan.inc). */
static int program_shader_last(const prog_t *program, const shader_entry *s, totk_mask *mask, uint64_t *sig) {
    unsigned b = ((uintptr_t)program >> 4) & 1023; totk_program *e = totk_programs[b];
    while (e && e->program != program) e = e->next;
    if (!e) return 0;
    for (unsigned i = 0; i < 6 && i < program->nstage; i++) if (e->shaders[i] == s) { *mask = e->mask[i]; *sig = e->sig[i]; return 1; }
    return 0;
}
static void program_remove(const prog_t *program) {
    unsigned b = ((uintptr_t)program >> 4) & 1023; totk_program **link = &totk_programs[b];
    while (*link && (*link)->program != program) link = &(*link)->next;
    if (*link) { totk_program *e = *link; *link = e->next; free(e); }
}
