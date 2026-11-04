/* Graphics pipeline libraries (VK_EXT_graphics_pipeline_library; TOTK_VK_GPL=1 on, 0 off).
   A pipeline the warm-up did not build used to be compiled whole on the translation thread when a draw first needed it:
   on the user's first playthrough (2026-11-02) 1,830 of them, 18.5 s, 33 ms each typically and up to ~150 ms, the
   biggest cause of hitches a new player meets. Here such a pipeline is linked from four libraries instead -- vertex
   input, vertex shader (pre-rasterization), fragment shader, fragment output -- each built once and kept, so a new
   combination of known shaders and state costs a fast link, and the whole optimized pipeline is built on the pipeline
   threads and swapped in when it is ready (the fast-linked one is destroyed once the GPU is past it).
   - Libraries come from graphics_pipeline_build with a part: the same derivation as a whole pipeline.
   - Their key is the state each one is built from (gpl_library_finish), not the pipeline key: a fragment shader library
     serves every vertex layout, blend state and vertex shader it meets.
   - Render passes: a library is built with one representative pass of its compatibility class (formats and attachment
     arrangement; load ops do not count) and links with any compatible pass.
   - Layouts: libraries use one-sided layouts with VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT (vertex set,
     fragment set); with libraries on, every graphics layout carries the flag (graphics_layout_get_locked), so whole and
     linked pipelines bind the same descriptor sets. Bindless (a third set) keeps whole pipelines only. */
/* gpl_state (nvn_vulkan.c): 1 with the extension, the feature and TOTK_VK_GPL, set at device creation */
static int gpl_on(void) { return gpl_state > 0 && !bl_set; }
static int gpl_prepared;
static int gpl_exp_full(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_VK_GPL_FULL");v=e&&atoi(e);}return v;}   /* diagnostic: libraries with the full layout */
static int gpl_exp_lto(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_VK_GPL_LTO");v=e&&atoi(e);}return v;}   /* diagnostic: link-time optimized links */   /* the libraries were prepared for this pipeline cache (prepare_check, nvn_vulkan_prepare.inc) */
enum { GPL_VI = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, GPL_PR = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
       GPL_FS = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, GPL_FO = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT };
static unsigned gpl_index(unsigned part) { return part == GPL_VI ? 0 : part == GPL_PR ? 1 : part == GPL_FS ? 2 : 3; }
static const char *const gpl_names[4] = {"vertex input", "vertex shader", "fragment shader", "fragment output"};
static _Atomic uint64_t gpl_hits[4], gpl_misses[4], gpl_miss_ns[4], gpl_links, gpl_link_ns, gpl_link_failed, gpl_upgraded, gpl_destroyed;

static __thread int gpl_warming;   /* gpl_warm_main's threads */
typedef struct { uint8_t b[1536]; size_t n; } gpl_blob;
static void gb_put(gpl_blob *g, const void *p, size_t n) { if (g->n + n > sizeof g->b) die("pipeline library key overflow"); memcpy(g->b + g->n, p, n); g->n += n; }
#define GB(g, v) do { __typeof__(v) gb_v_ = (v); gb_put((g), &gb_v_, sizeof gb_v_); } while (0)

typedef struct gpl_lib { struct gpl_lib *next; uint64_t hash; size_t n; VkPipeline lib; uint8_t blob[]; } gpl_lib;
static gpl_lib *gpl_libs[4096]; static pthread_mutex_t gpl_lock = PTHREAD_MUTEX_INITIALIZER;

/* One representative render pass per compatibility class. */
typedef struct gpl_compat { struct gpl_compat *next; graphics_pass_key key; VkRenderPass pass; } gpl_compat;
static gpl_compat *gpl_compats[256];
static VkRenderPass gpl_compat_pass(const graphics_pass_entry *pass) {
    graphics_pass_key k = pass->key; memset(k.load, 0, sizeof k.load); memset(k.stencil_load, 0, sizeof k.stencil_load);
    unsigned b = graphics_hash(&k, sizeof k) & 255; VkRenderPass rp;
    pthread_mutex_lock(&gpl_lock);
    gpl_compat *c = gpl_compats[b]; while (c && memcmp(&c->key, &k, sizeof k)) c = c->next;
    if (!c) { c = calloc(1, sizeof *c); if (!c) die("pipeline library pass allocation"); c->key = k; c->pass = pass->pass; c->next = gpl_compats[b]; gpl_compats[b] = c; }
    rp = c->pass; pthread_mutex_unlock(&gpl_lock); return rp;
}
/* The library layouts: the vertex library's {vertex set, none}, the fragment library's {none, fragment set}: fragment shaders
   read the support buffer from their own set (spirv_own_set), so neither library depends on the other's shader. */
typedef struct gpl_half { struct gpl_half *next; VkDescriptorSetLayout set0, set1; VkPipelineLayout layout; } gpl_half;
static gpl_half *gpl_halves[512];
static VkPipelineLayout gpl_half_layout(VkDescriptorSetLayout set0, VkDescriptorSetLayout set1) {
    uint64_t k[2] = {(uint64_t)(uintptr_t)set0, (uint64_t)(uintptr_t)set1}; unsigned b = graphics_hash(k, sizeof k) & 511; VkPipelineLayout layout;
    pthread_mutex_lock(&gpl_lock);
    gpl_half *h = gpl_halves[b]; while (h && (h->set0 != set0 || h->set1 != set1)) h = h->next;
    if (!h) { h = calloc(1, sizeof *h); if (!h) die("pipeline library layout allocation"); h->set0 = set0; h->set1 = set1;
        VkDescriptorSetLayout sets[2] = {set0, set1};
        VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2, .pSetLayouts = sets};
        VK_CHECK(vkCreatePipelineLayout(device, &info, NULL, &h->layout)); h->next = gpl_halves[b]; gpl_halves[b] = h; }
    layout = h->layout; pthread_mutex_unlock(&gpl_lock); return layout;
}
static void gb_multisample(gpl_blob *g, const VkPipelineMultisampleStateCreateInfo *m) {
    GB(g, m->rasterizationSamples); GB(g, m->sampleShadingEnable); GB(g, m->minSampleShading); GB(g, m->alphaToCoverageEnable); GB(g, m->alphaToOneEnable); GB(g, m->pSampleMask != NULL);
}
/* graphics_pipeline_build's library ending: the part's state into a library create info, its key from that state, and
   the library from the cache or made now. */
static VkResult gpl_library_finish(unsigned part, VkGraphicsPipelineCreateInfo *pi, const pipeline_key *key, shader_entry *vs, shader_entry *fs, graphics_pass_entry *pass,
    const VkPipelineVertexInputStateCreateInfo *vertex, const VkPipelineInputAssemblyStateCreateInfo *assembly, const VkPipelineRasterizationStateCreateInfo *raster,
    const VkPipelineMultisampleStateCreateInfo *multi, const VkPipelineDepthStencilStateCreateInfo *ds, const VkPipelineColorBlendStateCreateInfo *color,
    const VkPipelineDynamicStateCreateInfo *dynamic, const VkPipelineShaderStageCreateInfo *stages, const uint32_t *subgroup, VkPipeline *pipeline) {
    (void)key;
    static __thread gpl_blob g; g.n = 0;
    VkGraphicsPipelineLibraryCreateInfoEXT lib = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .flags = part};
    VkGraphicsPipelineCreateInfo li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &lib, .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (gpl_exp_lto() ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0), .pDynamicState = dynamic};
    GB(&g, gpl_exp_lto());
    const VkPipelineCreateFlags feedback = pi->flags & VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    GB(&g, part); GB(&g, dynamic->dynamicStateCount); gb_put(&g, dynamic->pDynamicStates, dynamic->dynamicStateCount * sizeof *dynamic->pDynamicStates);
    switch (part) {
    case GPL_VI:
        li.pVertexInputState = vertex; li.pInputAssemblyState = assembly;
        GB(&g, vertex->vertexBindingDescriptionCount); gb_put(&g, vertex->pVertexBindingDescriptions, vertex->vertexBindingDescriptionCount * sizeof *vertex->pVertexBindingDescriptions);
        GB(&g, vertex->vertexAttributeDescriptionCount); gb_put(&g, vertex->pVertexAttributeDescriptions, vertex->vertexAttributeDescriptionCount * sizeof *vertex->pVertexAttributeDescriptions);
        GB(&g, assembly->topology); GB(&g, assembly->primitiveRestartEnable);
        break;
    case GPL_PR:
        li.stageCount = 1; li.pStages = &stages[0]; li.pViewportState = pi->pViewportState; li.pRasterizationState = raster;
        li.layout = gpl_exp_full() ? pi->layout : gpl_half_layout(vs->layout, VK_NULL_HANDLE); li.renderPass = gpl_compat_pass(pass);
        GB(&g, stages[0].module); GB(&g, subgroup[0]); GB(&g, li.layout); GB(&g, li.renderPass);
        GB(&g, raster->polygonMode); GB(&g, raster->cullMode); GB(&g, raster->frontFace); GB(&g, raster->depthBiasEnable); GB(&g, raster->lineWidth);
        GB(&g, raster->depthClampEnable); GB(&g, raster->rasterizerDiscardEnable); GB(&g, raster->pNext != NULL);
        break;
    case GPL_FS:
        if (fs->module) { li.stageCount = 1; li.pStages = &stages[1]; }
        li.pDepthStencilState = ds; li.pMultisampleState = multi; li.flags |= feedback;
        li.layout = gpl_exp_full() ? pi->layout : gpl_half_layout(VK_NULL_HANDLE, fs->layout); li.renderPass = gpl_compat_pass(pass);
        GB(&g, fs->module ? stages[1].module : VK_NULL_HANDLE); GB(&g, subgroup[1]); GB(&g, li.layout); GB(&g, li.renderPass); GB(&g, feedback);
        gb_put(&g, &ds->flags, sizeof *ds - offsetof(VkPipelineDepthStencilStateCreateInfo, flags)); gb_multisample(&g, multi);
        break;
    case GPL_FO:
        li.pColorBlendState = color; li.pMultisampleState = multi; li.flags |= feedback; li.renderPass = gpl_compat_pass(pass);
        GB(&g, li.renderPass); GB(&g, feedback); GB(&g, color->flags); GB(&g, color->logicOpEnable); GB(&g, color->logicOp);
        gb_put(&g, color->blendConstants, sizeof color->blendConstants);
        GB(&g, color->attachmentCount); gb_put(&g, color->pAttachments, color->attachmentCount * sizeof *color->pAttachments); gb_multisample(&g, multi);
        break;
    default: die("pipeline library part %#x", part);
    }
    uint64_t hash = graphics_hash(g.b, g.n); unsigned b = hash & 4095, x = gpl_index(part);
    if (gpl_warming) {   /* the library warm-up: only the driver's compiled shader is wanted (gpl_warm_main). No pipeline cache:
                            Turnip then keeps it in Mesa's disk cache alone (read back on first use), not in ours, which is held in
                            memory whole and would have grown by 338 MB (2026-11-03) */
        VkPipeline made = VK_NULL_HANDLE; VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &li, NULL, &made);
        if (r == VK_SUCCESS) vkDestroyPipeline(device, made, NULL);
        *pipeline = VK_NULL_HANDLE; return r; }
    pthread_mutex_lock(&gpl_lock);
    for (gpl_lib *e = gpl_libs[b]; e; e = e->next) if (e->hash == hash && e->n == g.n && !memcmp(e->blob, g.b, g.n)) {
        *pipeline = e->lib; pthread_mutex_unlock(&gpl_lock); atomic_fetch_add(&gpl_hits[x], 1); return VK_SUCCESS; }
    pthread_mutex_unlock(&gpl_lock);
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    VkPipeline made = VK_NULL_HANDLE; VkResult r = pipeline_create_graphics(&li, &made);
    { uint64_t dt = dm_android_clock_ns(CLOCK_MONOTONIC) - t0; atomic_fetch_add(&gpl_miss_ns[x], dt); atomic_fetch_add(&gpl_misses[x], 1);
      static int trace = -1; if (trace < 0) { const char *e = getenv("TOTK_VK_GPL_TRACE"); trace = e && atoi(e); }
      if (trace) LOG("[vulkan-gpl] made %s library in %.2f ms: module %#llx%s", gpl_names[x], dt / 1e6, (unsigned long long)(uintptr_t)(x == 1 ? stages[0].module : x == 2 && fs->module ? stages[1].module : VK_NULL_HANDLE), pipeline_create_background ? " (pipeline thread)" : ""); }
    if (r != VK_SUCCESS) return r;
    gpl_lib *e = malloc(sizeof *e + g.n); if (!e) die("pipeline library allocation");
    e->hash = hash; e->n = g.n; e->lib = made; memcpy(e->blob, g.b, g.n);
    pthread_mutex_lock(&gpl_lock);
    for (gpl_lib *o = gpl_libs[b]; o; o = o->next) if (o->hash == hash && o->n == g.n && !memcmp(o->blob, g.b, g.n)) {   /* another thread made it meanwhile */
        pthread_mutex_unlock(&gpl_lock); vkDestroyPipeline(device, made, NULL); free(e); *pipeline = o->lib; return VK_SUCCESS; }
    e->next = gpl_libs[b]; gpl_libs[b] = e; pthread_mutex_unlock(&gpl_lock);
    *pipeline = made; return VK_SUCCESS;
}
/* The pipeline linked from its four libraries, without link-time optimization (the fast link). */
static VkResult gpl_link(const pipeline_key *key, shader_entry *vs, shader_entry *fs, graphics_pass_entry *pass, VkPipelineLayout layout, VkPipeline *out) {
    static const unsigned parts[4] = {GPL_VI, GPL_PR, GPL_FS, GPL_FO}; VkPipeline libs[4];
    for (unsigned i = 0; i < 4; i++) { VkResult r = graphics_pipeline_build(key, vs, fs, pass, layout, &libs[i], 0, parts[i]); if (r != VK_SUCCESS) { atomic_fetch_add(&gpl_link_failed, 1); return r; } }
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = libs};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
        .flags = (key->pad_[1] & 0x80 ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0) | (gpl_exp_lto() ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0), .layout = layout, .renderPass = pass->pass};
    VkResult r = pipeline_create_graphics(&pi, out);
    atomic_fetch_add(&gpl_link_ns, dm_android_clock_ns(CLOCK_MONOTONIC) - t0);
    if (r == VK_SUCCESS) atomic_fetch_add(&gpl_links, 1); else atomic_fetch_add(&gpl_link_failed, 1);
    return r;
}
/* Fast-linked pipelines replaced by their optimized build: destroyed once the GPU has finished everything recorded
   before the swap. The pipeline threads hand them over; the translation thread stamps them with the next submission
   at a frame boundary (anything that bound the old pipeline is in that submission or an earlier one). */
typedef struct gpl_retired { struct gpl_retired *next; VkPipeline pipeline; uint64_t ticket; } gpl_retired;
static gpl_retired *gpl_retired_new, *gpl_retired_wait; static pthread_mutex_t gpl_retired_lock = PTHREAD_MUTEX_INITIALIZER;
static void gpl_retire(VkPipeline p) {
    gpl_retired *r = malloc(sizeof *r); if (!r) die("pipeline retire allocation");
    pthread_mutex_lock(&gpl_retired_lock); *r = (gpl_retired){gpl_retired_new, p, 0}; gpl_retired_new = r; pthread_mutex_unlock(&gpl_retired_lock);
}
static void gpl_retire_poll(void) {   /* translation thread, once a frame */
    pthread_mutex_lock(&gpl_retired_lock); gpl_retired *n = gpl_retired_new; gpl_retired_new = NULL; pthread_mutex_unlock(&gpl_retired_lock);
    uint64_t ticket = mtl_last_fence() + 1;
    while (n) { gpl_retired *next = n->next; n->ticket = ticket; n->next = gpl_retired_wait; gpl_retired_wait = n; n = next; }
    int destroyed = 0;
    for (gpl_retired **link = &gpl_retired_wait; *link;) { gpl_retired *r = *link;
        if (mtl_wait(r->ticket, 0)) { vkDestroyPipeline(device, r->pipeline, NULL); *link = r->next; free(r); destroyed++; } else link = &r->next; }
    if (destroyed) { graphics_command_reset(); atomic_fetch_add(&gpl_destroyed, (uint64_t)destroyed); }   /* a new pipeline may reuse a destroyed handle */
}
static void gpl_report(void) {
    if (!gpl_on() || !presents || presents % 600) return;
    static uint64_t frame; if (frame == presents) return; frame = presents;
    char t[600]; int k = 0;
    for (unsigned i = 0; i < 4; i++) { uint64_t m = atomic_load(&gpl_misses[i]);
        k += snprintf(t + k, sizeof t - k, "%s%s %llu made (%.1f ms each) %llu reused", i ? "; " : "", gpl_names[i], (unsigned long long)m, m ? atomic_load(&gpl_miss_ns[i]) / 1e6 / m : 0.0, (unsigned long long)atomic_load(&gpl_hits[i])); }
    uint64_t l = atomic_load(&gpl_links);
    LOG("[vulkan-gpl] %llu pipelines linked (%.2f ms a link), %llu failed, %llu optimized builds swapped in, %llu fast ones destroyed; libraries: %s",
        (unsigned long long)l, l ? atomic_load(&gpl_link_ns) / 1e6 / l : 0.0, (unsigned long long)atomic_load(&gpl_link_failed), (unsigned long long)atomic_load(&gpl_upgraded), (unsigned long long)atomic_load(&gpl_destroyed), t);
}

/* The library warm-up: one library of every translated shader (vertex shaders as pre-rasterization libraries, fragment
   shaders as fragment shader libraries), built in the background with neutral state and thrown away. What stays is the
   driver's compiled shader in Mesa's disk cache: Turnip's key for a library shader is the shader, its one-sided layout
   and a shader key that none of our state changes, so the first real library of that shader, with whatever state, is
   a cache hit (median 0.00 ms against 3.6 ms, p90 27 ms, measured 2026-11-03), and a pipeline never met before is a
   link. Shaders not loaded this session are built privately and freed (shader_load_private). TOTK_VK_GPL_WARM: worker
   threads (default 3, 0 off), at nice 10 like the pipeline threads. */
static char (*gpl_warm_keys)[17]; static size_t gpl_warm_count; static _Atomic size_t gpl_warm_next, gpl_warm_done, gpl_warm_built;
static _Atomic uint64_t gpl_warm_begin, gpl_warm_ns; static _Atomic int gpl_warm_left;
static void gpl_warm_one(const char *key) {
    shader_load_private = 1; shader_entry *s = shader_load_by_key(key, 0, 0); int priv = shader_load_was_private; shader_load_private = 0;
    if (!s) return;
    if (s->stage == 0 || (s->stage == 1 && s->module)) {
        static const VkAttachmentDescription attachments[2] = {{.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
            {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
        static const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL}, depth = {1, VK_IMAGE_LAYOUT_GENERAL};
        graphics_pass_entry *pass = graphics_pass_get(graphics_pass_key_make(2, 1, attachments, &color, depth), attachments, &color, depth, 0);
        static shader_entry no_vertex = {.stage = 0};   /* a fragment library needs no vertex shader: its stage is not used */
        shader_entry *vs = s->stage == 0 ? s : &no_vertex, *fs = s->stage == 0 ? graphics_empty_shader() : s;
        pipeline_key k = {.vs = vs, .fs = fs, .colors = 1, .primitive = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipeline lib;
        if (pass && graphics_pipeline_build(&k, vs, fs, pass, VK_NULL_HANDLE, &lib, 0, s->stage == 0 ? GPL_PR : GPL_FS) == VK_SUCCESS) atomic_fetch_add(&gpl_warm_built, 1);
    }
    if (priv) shader_entry_free_private(s);
}
static void *gpl_warm_main(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-libwarm"); totk_pin_self("TOTK_GPL_WARM_CPUS", "slow");   /* off the prime cores: they ran it at 105 C */
    resource_create_quiet = 1; pipeline_create_background = 1; gpl_warming = 1;
    for (size_t i; (i = atomic_fetch_add(&gpl_warm_next, 1)) < gpl_warm_count;) {
        gpl_warm_one(gpl_warm_keys[i]);
        size_t done = atomic_fetch_add(&gpl_warm_done, 1) + 1;
        if (!(done % 2000)) LOG("[vulkan-gpl-warm] %zu of %zu shaders (%.0f s)", done, gpl_warm_count, (dm_android_clock_ns(CLOCK_MONOTONIC) - atomic_load(&gpl_warm_begin)) / 1e9);
    }
    if (atomic_fetch_sub(&gpl_warm_left, 1) == 1) {
        LOG("[vulkan-gpl-warm] done: %zu shaders, %zu libraries built in %.1f s", gpl_warm_count, (size_t)atomic_load(&gpl_warm_built), (dm_android_clock_ns(CLOCK_MONOTONIC) - atomic_load(&gpl_warm_begin)) / 1e9); }
    return NULL;
}
/* from_prepare: prepare mode's call (nvn_vulkan_prepare.inc, 4 threads); in play only with TOTK_VK_GPL_WARM=<threads> set:
   in the background of play it heated the device to 100 C. 1 when the threads started. */
static int gpl_warm_start(int from_prepare) {
    const char *e = getenv("TOTK_VK_GPL_WARM"); int threads = e ? atoi(e) : from_prepare ? 4 : 0; if (threads <= 0) return 0; if (threads > 6) threads = 6;
    static _Atomic int started; if (!gpl_on() || atomic_exchange(&started, 1)) return 0;
    const char *dir = getenv("DM_VK_SHADERS"); DIR *d = dir ? opendir(dir) : NULL; if (!d) return 0;
    size_t cap = 32768; gpl_warm_keys = malloc(cap * sizeof *gpl_warm_keys); if (!gpl_warm_keys) { closedir(d); return 0; }
    for (struct dirent *f; (f = readdir(d));) { size_t n = strlen(f->d_name); if (n != 21 || strcmp(f->d_name + 16, ".meta")) continue;
        if (gpl_warm_count == cap) { cap *= 2; void *m = realloc(gpl_warm_keys, cap * sizeof *gpl_warm_keys); if (!m) break; gpl_warm_keys = m; }
        memcpy(gpl_warm_keys[gpl_warm_count], f->d_name, 16); gpl_warm_keys[gpl_warm_count++][16] = 0; }
    closedir(d);
    atomic_store(&gpl_warm_begin, dm_android_clock_ns(CLOCK_MONOTONIC)); atomic_store(&gpl_warm_left, threads);
    LOG("[vulkan-gpl-warm] start: %zu shaders on %d threads", gpl_warm_count, threads);
    for (int i = 0; i < threads; i++) { pthread_t t; if (pthread_create(&t, NULL, gpl_warm_main, NULL)) { atomic_fetch_sub(&gpl_warm_left, 1); continue; } pthread_detach(t); }
    return 1;
}
