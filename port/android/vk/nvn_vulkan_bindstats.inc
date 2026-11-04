/* TOTK_VK_BINDSTATS=1: what each graphics draw changes in the state the driver sees, and what Turnip sends again for it.
   graphics_command_apply already drops binds identical to the last; this counts the ones left. On every descriptor-set
   bind (new sets, or the same sets with new dynamic offsets) Turnip emits the pipeline's descriptor preload again
   (tu6_emit_load_state): one CP_LOAD_STATE6 per uniform, storage or texel binding and per stage, and two per element
   of a combined image sampler array. For new sets it also compares each descriptor with the previous draw's, by kind,
   and splits the draws by what they render into (bindstats_target). Logged every 300 presents as [bindstats]. */
static int bindstats_state = -1;
static int bindstats_on_slow(void);
static inline int bindstats_on(void) { return bindstats_state >= 0 ? bindstats_state : bindstats_on_slow(); }
static int bindstats_on_slow(void) {
    if (bindstats_state < 0) { const char *e = getenv("TOTK_VK_BINDSTATS"); bindstats_state = e && atoi(e) > 0;
        if (bindstats_state) LOG("[bindstats] on: per-draw state changes, every 300 presents"); }
    return bindstats_state;
}
enum { BS_TEXTURE, BS_UNIFORM, BS_STORAGE, BS_TEXEL, BS_KINDS };
static const char *const bindstats_kind_name[BS_KINDS] = {"textures", "uniform buffers", "storage", "texel buffers"};
typedef struct { VkDescriptorSetLayout layout; uint64_t *w; size_t n, cap; } bindstats_payload;
static bindstats_payload bindstats_cur[6], bindstats_prev[6];   /* by shader stage: the payload bind_shader built last, and at the last draw */
static struct {
    uint64_t draws, pipeline, rebinds, new_set[2], offsets_only, layout_change[2], preload, dynamic_offsets;
    uint64_t viewport, scissor, unchanged;
    uint64_t total[BS_KINDS], changed[BS_KINDS];   /* descriptors in the draw's sets; of those in a new set, how many differ */
    uint64_t presents0;
} bs;
enum { BT_DEPTH_ONLY, BT_GBUFFER, BT_FULL_COLOUR, BT_SMALL, BT_CLASSES };
static const char *const bindstats_target_name[BT_CLASSES] = {"depth only", "G-buffer (4+ colour targets)", "other colour, 640 wide or more", "other colour, smaller"};
typedef struct { uint32_t w, h, colors, depth; uint64_t draws; } bindstats_shape;
static bindstats_shape bindstats_shapes[64]; static unsigned bindstats_nshapes; static uint64_t bindstats_targets[BT_CLASSES];
/* draw_graphics_inner, per draw: the pass's size, colour target count and whether it has depth */
static void bindstats_target(uint32_t w, uint32_t h, uint32_t colors, int depth) {
    bindstats_targets[!colors ? BT_DEPTH_ONLY : colors >= 4 ? BT_GBUFFER : w >= 640 ? BT_FULL_COLOUR : BT_SMALL]++;
    unsigned i = 0; while (i < bindstats_nshapes && !(bindstats_shapes[i].w == w && bindstats_shapes[i].h == h && bindstats_shapes[i].colors == colors && bindstats_shapes[i].depth == (uint32_t)depth)) i++;
    if (i == bindstats_nshapes) { if (i == 64) return; bindstats_shapes[i] = (bindstats_shape){w, h, colors, (uint32_t)depth, 0}; bindstats_nshapes++; }
    bindstats_shapes[i].draws++;
}
static int bindstats_shape_cmp(const void *a, const void *b) { uint64_t x = ((const bindstats_shape *)a)->draws, y = ((const bindstats_shape *)b)->draws; return x < y ? 1 : x > y ? -1 : 0; }
static void bindstats_copy(bindstats_payload *d, VkDescriptorSetLayout layout, const uint64_t *w, size_t n) {
    if (n > d->cap) { d->w = realloc(d->w, n * sizeof *d->w); if (!d->w) die("bindstats payload"); d->cap = n; }
    if (n) memcpy(d->w, w, n * sizeof *w); d->n = n; d->layout = layout;
}
/* bind_shader, after descriptor_cache_find built descriptor_payload for this shader */
static void bindstats_capture(const shader_entry *s) {
    if (s->stage < 6) bindstats_copy(&bindstats_cur[s->stage], s->layout, descriptor_payload, descriptor_payload_words);
}
static int bindstats_kind(VkDescriptorType t) {
    if (descriptor_type_uniform(t)) return BS_UNIFORM;
    if (t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return BS_TEXEL;
    if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) return BS_STORAGE;
    return BS_TEXTURE;
}
/* descriptor_payload_build's layout: per write (type << 32 | binding), count, then 3 words an element (1 for texel views) */
static void bindstats_walk(const bindstats_payload *cur, const bindstats_payload *prev, int diff, int all_changed) {
    for (size_t i = 0; i + 2 <= cur->n;) {
        VkDescriptorType t = (VkDescriptorType)(cur->w[i] >> 32); uint64_t count = cur->w[i + 1]; int k = bindstats_kind(t);
        size_t per = k == BS_TEXEL ? 1 : 3; i += 2;
        for (uint64_t j = 0; j < count && i + per <= cur->n; j++, i += per) {
            bs.total[k]++;
            if (diff && (all_changed || i + per > prev->n || memcmp(cur->w + i, prev->w + i, per * sizeof *cur->w))) bs.changed[k]++;
        }
    }
}
static unsigned bindstats_preload(const shader_entry *s) {
    unsigned n = 0;
    for (unsigned i = 0; i < s->count; i++) { if (!s->bindings[i].count) continue;
        n += s->binding_plan[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? 2 * s->bindings[i].count : 1; }
    return n;
}
static void bindstats_draw(const shader_entry *vs, const shader_entry *fs, VkPipeline pipeline, VkPipelineLayout layout,
    const VkDescriptorSet sets[2], const uint32_t *dynamic_offsets, unsigned dynamic_count, const VkViewport *viewport, const VkRect2D *scissor) {
    const graphics_command_state *last = &graphics_commands; const shader_entry *sh[2] = {vs, fs};
    bs.draws++; bs.dynamic_offsets += dynamic_count;
    int p = last->pipeline != pipeline, v = !(last->valid & 1) || memcmp(&last->viewport, viewport, sizeof *viewport),
        sc = !(last->valid & 2) || memcmp(&last->scissor, scissor, sizeof *scissor);
    int news[2] = {last->sets[0] != sets[0], last->sets[1] != sets[1]};
    int rebind = last->layout != layout || news[0] || news[1] || last->dynamic_count != dynamic_count ||
        memcmp(last->dynamic_offsets, dynamic_offsets, dynamic_count * sizeof *dynamic_offsets);
    bs.pipeline += p; bs.viewport += v; bs.scissor += sc; bs.unchanged += !p && !v && !sc && !rebind;
    if (rebind) { bs.rebinds++; if (!news[0] && !news[1]) bs.offsets_only++;
        for (int k = 0; k < 2; k++) if (sh[k]) bs.preload += bindstats_preload(sh[k]); }
    for (int k = 0; k < 2; k++) { const shader_entry *s = sh[k]; if (!s || s->stage >= 6) continue;
        bindstats_payload *c = &bindstats_cur[s->stage], *pr = &bindstats_prev[s->stage];
        int relayout = c->layout != pr->layout; bs.new_set[k] += news[k]; bs.layout_change[k] += news[k] && relayout;
        bindstats_walk(c, pr, news[k], relayout);
        bindstats_copy(pr, c->layout, c->w, c->n); }
    if (presents >= bs.presents0 + 300) {
        if (bs.presents0) { double f = (double)(presents - bs.presents0), d = bs.draws ? (double)bs.draws : 1;
            LOG("[bindstats] per frame: %.0f draws; pipeline changes %.0f, viewport %.0f, scissor %.0f, nothing changed %.0f; "
                "descriptor binds %.0f (new vertex set %.0f, of them new layout %.0f; new fragment set %.0f, of them new layout %.0f; "
                "dynamic offsets only %.0f); Turnip preload packets %.0f (%.1f a bind); dynamic offsets %.1f a draw",
                bs.draws / f, bs.pipeline / f, bs.viewport / f, bs.scissor / f, bs.unchanged / f,
                bs.rebinds / f, bs.new_set[0] / f, bs.layout_change[0] / f, bs.new_set[1] / f, bs.layout_change[1] / f,
                bs.offsets_only / f, bs.preload / f, bs.rebinds ? (double)bs.preload / bs.rebinds : 0, bs.dynamic_offsets / d);
            for (int k = 0; k < BS_KINDS; k++)
                LOG("[bindstats]   %s: %.2f a draw, %.2f changed a draw", bindstats_kind_name[k], bs.total[k] / d, bs.changed[k] / d);
            LOG("[bindstats] frame=%llu draws by target per frame: %s %.0f, %s %.0f, %s %.0f, %s %.0f", (unsigned long long)presents,
                bindstats_target_name[0], bindstats_targets[0] / f, bindstats_target_name[1], bindstats_targets[1] / f,
                bindstats_target_name[2], bindstats_targets[2] / f, bindstats_target_name[3], bindstats_targets[3] / f);
            qsort(bindstats_shapes, bindstats_nshapes, sizeof *bindstats_shapes, bindstats_shape_cmp);
            for (unsigned i = 0; i < bindstats_nshapes && i < 8; i++)
                LOG("[bindstats]   %ux%u, %u colour%s%s: %.0f draws", bindstats_shapes[i].w, bindstats_shapes[i].h, bindstats_shapes[i].colors,
                    bindstats_shapes[i].colors == 1 ? "" : "s", bindstats_shapes[i].depth ? " + depth" : "", bindstats_shapes[i].draws / f); }
        uint64_t p0 = presents; memset(&bs, 0, sizeof bs); bs.presents0 = p0;
        memset(bindstats_targets, 0, sizeof bindstats_targets); bindstats_nshapes = 0;
    }
}
