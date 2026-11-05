/* Program prefetch (mtl_prog_prefetch, called by hle_nvn.c's nvnProgramSetShaders; TOTK_VK_PREFETCH=0 off). The game
   makes its programs as it loads and streams, before their first draw; what a draw would otherwise do on the translation
   thread is started then, on a background thread:
   - every translated variant of the program's shaders is loaded (reflection, module, descriptor layout: ~0.5 ms each of
     file reads and parsing; 606 of them in the long frames of a new player's walk);
   - a vertex shader paired with a fragment shader that reads more than it writes needs a variant with those outputs
     (graphics_vs_module), a shader the prepare step cannot know: its pipeline library would compile on the translation
     thread at the first draw (7 of them, 25-47 ms each, 179 ms in the same walk). It is built here into Mesa's disk cache,
     as the prepare step builds the others (gpl_warming), so the draw's library is a cache hit.
   Programs are seen once per (program, serial); a job carries the stage ids only (the shader binaries are hashed at
   once, on the caller's thread: the program may be changed before the worker runs). */
typedef struct prefetch_job { struct prefetch_job *next; int n; int st[6]; char id[6][65]; } prefetch_job;
static prefetch_job *prefetch_head, **prefetch_tail = &prefetch_head;
static pthread_mutex_t prefetch_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t prefetch_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t prefetch_programs, prefetch_loaded, prefetch_variants, prefetch_ns;
static int prefetch_state = -1;
static void prefetch_report(void) {
    static uint64_t last; uint64_t n = atomic_load(&prefetch_programs); if (n < last + 2000) return; last = n;
    LOG("[prefetch] %llu programs, %llu shaders loaded ahead, %llu vertex-shader variant libraries built ahead (%.0f ms in all)",
        (unsigned long long)n, (unsigned long long)atomic_load(&prefetch_loaded), (unsigned long long)atomic_load(&prefetch_variants), atomic_load(&prefetch_ns) / 1e6);
}
/* The known variants of a shader (its .variants list), loaded and published; up to max. */
static unsigned prefetch_variants_of(const char *id, shader_entry **out, unsigned max) {
    char path[1200]; snprintf(path, sizeof path, "%s/%s.variants", totk_cache_dir(), id);
    /* outside totk_shader_lock: the translation thread takes it per draw */
    char *list = totk_read_text(path); if (!list) return 0;
    unsigned n = 0;
    for (char *line = strtok(list, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        if (strlen(line) != 16) continue;
        int fresh = 0; { pthread_mutex_lock(&graphics_object_lock); fresh = 1; for (shader_entry *s = shader_hash[shader_key_bucket(line)]; s; s = s->hash_next) if (!strcmp(s->key, line)) { fresh = 0; out[n++] = s; break; } pthread_mutex_unlock(&graphics_object_lock); }
        if (!fresh) continue;
        /* an incomplete translation */
        char deps[1200]; snprintf(deps, sizeof deps, "%s/%s.deps", totk_cache_dir(), line); if (access(deps, F_OK)) continue;
        shader_entry *s = shader_load_by_key(line, 0, 0); if (!s) continue;
        atomic_fetch_add(&prefetch_loaded, 1); out[n++] = totk_named(s, id);
    }
    free(list); return n;
}
static void prefetch_pair(shader_entry *vs, shader_entry *fs) {
    uint32_t missing = fs->module ? fs->io_mask & ~vs->io_mask : 0;
    if (!missing || !vs->spirv || !gpl_on()) return;
    /* made before: its library too */
    for (struct linked_module *m = vs->linked; m; m = m->next) if (m->missing == missing) return;
    static const VkAttachmentDescription attachments[2] = {{.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
    static const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL}, depth = {1, VK_IMAGE_LAYOUT_GENERAL};
    graphics_pass_entry *pass = graphics_pass_get(graphics_pass_key_make(2, 1, attachments, &color, depth), attachments, &color, depth, 0); if (!pass) return;
    pipeline_key k = {.vs = vs, .fs = fs, .colors = 1, .primitive = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC); VkPipeline lib;
    /* the variant module (graphics_vs_module, kept on vs->linked) and its library into Mesa's disk cache */
    gpl_warming = 1;
    if (graphics_pipeline_build(&k, vs, fs, pass, VK_NULL_HANDLE, &lib, 0, GPL_PR) == VK_SUCCESS) atomic_fetch_add(&prefetch_variants, 1);
    gpl_warming = 0; atomic_fetch_add(&prefetch_ns, dm_android_clock_ns(CLOCK_MONOTONIC) - t0);
}
static void *prefetch_main(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-prefetch"); totk_pin_self("TOTK_GPL_WARM_CPUS", "slow");
    { const char *e = getenv("TOTK_PSO_NICE"); int nice = e ? atoi(e) : 10; if (nice) setpriority(PRIO_PROCESS, (id_t)gettid(), nice); }
    resource_create_quiet = 1; pipeline_create_background = 1;
    for (;;) {
        pthread_mutex_lock(&prefetch_lock); while (!prefetch_head) pthread_cond_wait(&prefetch_cv, &prefetch_lock);
        prefetch_job *j = prefetch_head; prefetch_head = j->next; if (!prefetch_head) prefetch_tail = &prefetch_head; pthread_mutex_unlock(&prefetch_lock);
        shader_entry *vs[8], *fs[8]; unsigned nv = 0, nf = 0;
        for (int i = 0; i < j->n; i++) {
            if (j->st[i] == 0 && !nv) nv = prefetch_variants_of(j->id[i], vs, 8);
            else if (j->st[i] == 1 && !nf) nf = prefetch_variants_of(j->id[i], fs, 8);
            else if (j->st[i] == 5) { shader_entry *cs[8]; prefetch_variants_of(j->id[i], cs, 8); }
        }
        for (unsigned a = 0; a < nv; a++) for (unsigned b = 0; b < nf; b++) prefetch_pair(vs[a], fs[b]);
        free(j); prefetch_report();
    }
    return NULL;
}
void mtl_prog_prefetch(const prog_t *p) {
    if (prefetch_state < 0) { const char *e = getenv("TOTK_VK_PREFETCH"); prefetch_state = !(e && !atoi(e));
        if (prefetch_state) { pthread_t t; if (pthread_create(&t, NULL, prefetch_main, NULL)) prefetch_state = 0; else pthread_detach(t); }
        LOG("[prefetch] program prefetch: %s", prefetch_state ? "on (shaders loaded and vertex-shader variants built when the game makes a program)" : "off"); }
    if (!prefetch_state) return;
    const prog_t *program = p; if (!program || program->nstage > 6) return;
    prefetch_job *j = calloc(1, sizeof *j); if (!j) return;
    for (unsigned i = 0; i < program->nstage; i++) {
        const uint8_t *control = program->sd[i].control, *code = (const uint8_t *)(uintptr_t)program->sd[i].data;
        if (!control || !code) continue;
        uint32_t st, length, c1o; memcpy(&st, control + 0x714, 4); memcpy(&length, control + 0x704, 4); memcpy(&c1o, control + 0x700, 4);
        /* totk_shader_id would stop the game */
        if ((st != 0 && st != 1 && st != 5) || !length || length > 16u << 20 || c1o > length || (st != 5 && c1o < 0x30)) continue;
        j->st[j->n] = (int)st; totk_shader_id(control, code, (int)st, j->id[j->n]); j->n++;
    }
    if (!j->n) { free(j); return; }
    atomic_fetch_add(&prefetch_programs, 1);
    pthread_mutex_lock(&prefetch_lock); *prefetch_tail = j; prefetch_tail = &j->next; pthread_cond_signal(&prefetch_cv); pthread_mutex_unlock(&prefetch_lock);
}
