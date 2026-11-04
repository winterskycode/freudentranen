/* Two-phase translation, phase one: the plan of a recording's draws, made when the recording completes.

   The game's frame is assembled from recordings ("pieces") that complete well before the submit (the world piece, 3,300 of
   3,650 draws, about 8 ms before it). Everything the translation decides from the piece's own command stream can be decided
   then, on this thread while it waits for the submit: the shader variants, the pipeline key and its lookup inputs, the vertex
   input description, the primitive. What depends on the order of the frame (pending writes, forwards, image hazards, the
   deferred texture handles, the uniform bytes) stays in phase two, the translation at the submit, which runs as before and
   takes the plan where its check passes.

   A plan is made against a private copy of the tracked state (nvn_state): it starts as the state the translation last left
   (the queue state the first piece of a frame inherits) and the piece's own ops are applied to it. Where a draw depends on
   state the piece did not set itself (inherited), the plan records what it assumed, and the check at the submit compares
   that with the state then. Nothing here reads the game's memory and nothing has a side effect outside the plan table
   (shader variants may be loaded, pipeline layouts created: the same objects the submit would create).

   Keyed by the recording's copy (pointer and serial from the hle's copy_prefix), carried into the submitted stream by the
   piece markers inline_recording writes.

   MEASURED A NET LOSS (2026-10-31, still scene, instructions a frame on the translation thread, back to back): off 60.6 /
   60.7 M; plans made but not used (TOTK_VK_PLAN=2) 68.0 M; plans used (=1) 70.0 M, with 99% of the planned draws passing
   the checks. The work a plan can take over (program lookup, pipeline key, vertex input: 5-8% of the thread by line
   profile) costs less than the checks that make it safe (the state signatures, the inherited-state compares) plus the
   predecode itself (1.1 ms CPU a frame). The thread's cost is the order-dependent bookkeeping (pending writes, forwards,
   image hazards, descriptor sets), which no phase one can take. Kept, off by default (TOTK_VK_PLAN=1 / 2), as the
   measured answer and for a phase one with different content (early pipeline creation); the markers stay on. */
#ifndef PLAN_PART2
typedef struct draw_plan {
    uint8_t vsfs, key_valid, fs_variant;   /* fs_variant: the fragment shader is a program stage (else the empty shader, no check) */
    uint16_t inherit;                       /* PS_* categories the key took from the inherited state */
    uint16_t inherit_attr, inherit_stream;  /* vertex attribute / stream slots taken from it */
    const prog_t *program; uint64_t program_serial;
    shader_entry *vs, *fs; totk_mask vmask, fmask; uint64_t vsig, fsig;   /* the variants and the state signatures they were chosen by */
    VkPipelineLayout layout;
    pipeline_key key; ds_st dyn_depth; poly_st dyn_polygon; unsigned na; VkPrimitiveTopology primitive;
    VkVertexInputAttributeDescription attrs[16]; VkVertexInputBindingDescription streams[16]; unsigned nattr, nstream; uint8_t stream_used[16]; uint32_t attribute_end[16];
    struct { p_set_rt rt; ms_st ms; color_st color; chmask_st mask; poly_st polygon; ds_st depth; blend_st blend[8]; int depth_clamp, raster_discard; va_st attributes[16]; vs_st streams[16]; } inh;   /* inherited values as assumed */
} draw_plan;
enum { PS_PROGRAM = 1u << 0, PS_RT = 1u << 1, PS_COLOR = 1u << 2, PS_MASK = 1u << 3, PS_BLEND = 1u << 4, PS_MS = 1u << 5, PS_POLY = 1u << 6, PS_DEPTH = 1u << 7, PS_CLAMP = 1u << 8, PS_DISCARD = 1u << 9 };
static int plan_check(const draw_plan *pl);   /* the shaders of the plan hold for the state at the submit */
static int plan_check_key(const draw_plan *pl, unsigned na, const VkAttachmentDescription *attachments, const VkAttachmentReference *colors, VkAttachmentReference depth, unsigned nblocks);
static const draw_plan *plan_next_draw(void);
static void drawsig_note(const p_draw *draw, const p_draw_idx *indexed);   /* TOTK_VK_DRAWSIG census, part 2 */
static void plan_piece_enter(uint64_t copy, uint64_t serial);
static void plan_piece_leave(void);
static uint64_t plan_used, plan_fail_program, plan_fail_sig, plan_fail_key, plan_nokey, plan_noplan, plan_draws_seen, plan_key_used;
#else
static int plan_state = -1;
static int plan_on(void) { if (plan_state < 0) { const char *e = getenv("TOTK_VK_PLAN"); plan_state = e ? atoi(e) != 0 : 0; } return plan_state; }
int mtl_plan_on(void) { return plan_on(); }
typedef struct piece_plan { uint64_t copy, serial, used, cb; uint32_t ndraws, cap; draw_plan *draws; int looked_up; } piece_plan;
/* command buffer objects whose recent recordings were planned and never seen at a submit are not planned (the game's job
   threads record many recordings a frame that no submission uses; a streak resets when one is used) */
static struct { uint64_t cb; unsigned streak; } plan_cbs[64]; static uint64_t plan_cb_skipped;
static unsigned *plan_cb_streak(uint64_t cb) { unsigned free_i = 64; for (unsigned i = 0; i < 64; i++) { if (plan_cbs[i].cb == cb) return &plan_cbs[i].streak; if (!plan_cbs[i].cb && free_i == 64) free_i = i; }
    if (free_i == 64) free_i = (unsigned)(cb >> 6) & 63; plan_cbs[free_i].cb = cb; plan_cbs[free_i].streak = 0; return &plan_cbs[free_i].streak; }
#define PLAN_PIECES 128
static piece_plan plan_pieces[PLAN_PIECES]; static uint64_t plan_clock;
static uint64_t plan_pieces_built, plan_draws_built, plan_ns, plan_calls_skipped, plan_bad, plan_unused_pieces, plan_unused_draws, plan_yielded;
int nvn_plan_yield(void) __attribute__((weak));   /* hle_nvn.c: a submit waits for this thread */
enum { PR_OK, PR_NOPROGRAM, PR_STAGE, PR_NOLAST, PR_NOIMAGE, PR_BLOCK, PR_PRIM, PR_VFMT, PR_DIVISOR, PR_COUNT_ }; static uint64_t plan_reason[PR_COUNT_];   /* why a draw's plan stopped where it did */
/* an image entry for a texture without creating one: by serial, else by the storage it shares (image_get would make the alias) */
static image_entry *image_peek(const tex_t *t) {
    image_entry *e = image_lookup(t->serial); if (e) return e;
    struct storage_key sk; if (!storage_key_of(t, &sk)) return NULL;
    for (image_entry *x = storage_index[storage_bucket(&sk)]; x; x = x->storage_next) if (!memcmp(&x->storage, &sk, sizeof sk)) return x;
    return NULL;
}
static piece_plan *plan_find(uint64_t copy, uint64_t serial) {
    for (unsigned i = 0; i < PLAN_PIECES; i++) if (plan_pieces[i].copy == copy && plan_pieces[i].serial == serial) return &plan_pieces[i];
    return NULL;
}
static piece_plan *plan_slot(uint64_t copy, uint64_t serial) {   /* a free slot, else the least recently used */
    piece_plan *old = &plan_pieces[0];
    for (unsigned i = 0; i < PLAN_PIECES; i++) { piece_plan *p = &plan_pieces[i]; if (!p->copy) { old = p; break; } if (p->used < old->used) old = p; }
    if (old->copy && !old->looked_up) { plan_unused_pieces++; plan_unused_draws += old->ndraws; if (old->cb) (*plan_cb_streak(old->cb))++; }   /* planned, never seen at a submit */
    old->copy = copy; old->serial = serial; old->ndraws = 0; old->used = ++plan_clock; old->looked_up = 0; return old;
}
static uint32_t plan_category(unsigned op, const uint8_t *a) {
    switch (op) {
        case OP_BIND_PROGRAM: return PS_PROGRAM; case OP_SET_RT: return PS_RT; case OP_DEPTH_CLAMP: return PS_CLAMP; case OP_RASTER_DISCARD: return PS_DISCARD;
        case OP_BIND_STATE: { static const uint32_t bits[6] = {PS_BLEND, PS_COLOR, PS_MASK, PS_MS, PS_POLY, PS_DEPTH}; int k = ((const p_bind_state *)a)->kind; return k >= 0 && k < 6 ? bits[k] : 0; }
        default: return 0;
    }
}
/* The plan of one draw from the private state ps; set / attr_set / stream_set: what the piece set itself so far. */
static void plan_build_draw(const nvn_state *ps, uint32_t set, uint16_t attr_set, uint16_t stream_set, const p_draw *draw, const p_draw_idx *indexed, draw_plan *pl) {
    memset(pl, 0, sizeof *pl);
    if (!ps->program) { plan_reason[PR_NOPROGRAM]++; return; }
    pl->program = ps->program; pl->program_serial = ps->program->serial;
    shader_entry *vs = NULL, *fs = NULL;
    for (unsigned i = 0; i < ps->program->nstage; i++) { shader_entry *s = program_shader_in(ps, ps->program, i); if (s->stage == 0) vs = s; else if (s->stage == 1) fs = s; else { plan_reason[PR_STAGE]++; return; } }
    if (!vs) { plan_reason[PR_STAGE]++; return; }
    if (!program_shader_last(ps->program, vs, &pl->vmask, &pl->vsig)) { plan_reason[PR_NOLAST]++; return; }
    if (fs) { if (!program_shader_last(ps->program, fs, &pl->fmask, &pl->fsig)) { plan_reason[PR_NOLAST]++; return; } pl->fs_variant = 1; } else fs = graphics_empty_shader();
    pl->vs = vs; pl->fs = fs; pl->layout = graphics_layout_get(vs, fs); pl->vsfs = 1;
    if (!(set & PS_PROGRAM)) pl->inherit |= PS_PROGRAM;   /* checked by program pointer and serial */
    /* the render targets: attachment order and formats, from the image entries that exist (a target without one yet, or a
       block-compressed target, has no planned key) */
    unsigned na = 0; VkAttachmentReference colors[8], depth = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; VkFormat formats[9] = {0};
    for (int i = 0; i < ps->rt.n; i++) {
        colors[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; if (!ps->rt.color[i]) continue;
        image_entry *e = image_peek(ps->rt.color[i]); if (!e) { plan_reason[PR_NOIMAGE]++; return; }
        int bw, bh; nvn_fmt_bpb(e->texture.d.format, &bw, &bh); if (bw != 1 || bh != 1) { plan_reason[PR_BLOCK]++; return; }
        const view_t *v = ps->rt.has_cview[i] ? &ps->rt.cview[i] : NULL;
        formats[na] = attachment_format(e, ps->rt.color[i], v); colors[i].attachment = na++;
    }
    if (ps->rt.depth) { image_entry *e = image_peek(ps->rt.depth); if (!e) { plan_reason[PR_NOIMAGE]++; return; } formats[na] = e->format; depth.attachment = na++; }
    pl->na = na;
    unsigned prim = draw ? draw->prim : indexed->prim;
    if (prim > 8 || prim == 2) { plan_reason[PR_PRIM]++; return; }   /* topology() would die */
    pl->primitive = topology(prim);
    /* the vertex input */
    for (unsigned i = 0; i < 16; i++) { va_st a = ps->attributes[i]; if (!a.fmt || !vs->attributes[i]) continue; VkFormat format = vk_format(a.fmt); if (format == VK_FORMAT_UNDEFINED) { plan_reason[PR_VFMT]++; return; }
        pl->attrs[pl->nattr++] = (VkVertexInputAttributeDescription){i, a.stream, format, a.offset}; if (a.stream >= 16) { plan_reason[PR_VFMT]++; return; } pl->stream_used[a.stream] = 1; int bw, bh; uint32_t end = a.offset + nvn_fmt_bpb(a.fmt, &bw, &bh); if (end > pl->attribute_end[a.stream]) pl->attribute_end[a.stream] = end;
        if (!(attr_set >> i & 1)) { pl->inherit_attr |= (uint16_t)(1u << i); pl->inh.attributes[i] = a; } }
    for (unsigned i = 0; i < 16; i++) if (pl->stream_used[i]) { const vs_st *s = &ps->streams[i]; if (s->divisor > 1) { plan_reason[PR_DIVISOR]++; return; }
        pl->streams[pl->nstream++] = (VkVertexInputBindingDescription){i, s->stride, s->divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (!(stream_set >> i & 1)) { pl->inherit_stream |= (uint16_t)(1u << i); pl->inh.streams[i] = *s; } }
    /* the pipeline key, as draw_graphics_inner builds it */
    pipeline_key key = {.vs = vs, .fs = fs, .colors = ps->rt.n, .primitive = pl->primitive, .color = ps->color, .mask = ps->mask, .polygon = ps->polygon, .depth = depth.attachment != VK_ATTACHMENT_UNUSED ? ps->depth : (ds_st){0}, .multisample = ps->multisample, .attachments = na, .depth_attachment = depth.attachment, .depth_clamp = (uint8_t)ps->depth_clamp, .raster_discard = (uint8_t)ps->raster_discard};
#ifdef __ANDROID__
    key.required_subgroup_size = vk_api.requested_subgroup_size;
#endif
    for (int i = 0; i < ps->rt.n; i++) { key.color_indices[i] = colors[i].attachment; if (colors[i].attachment != VK_ATTACHMENT_UNUSED) key.formats[i] = formats[colors[i].attachment]; if ((ps->color.blend_en >> i) & 1) key.blend[i] = ps->blend[i]; }
    if (depth.attachment != VK_ATTACHMENT_UNUSED) key.formats[8] = formats[depth.attachment];
    for (unsigned i = 0; i < 16; i++) { if (ps->attributes[i].fmt && vs->attributes[i]) key.attributes[i] = ps->attributes[i]; if (pl->stream_used[i]) key.streams[i] = ps->streams[i]; }
    { poly_st p = {0}; p.cull = key.polygon.cull; p.front = key.polygon.front; p.mode = key.polygon.mode; p.offset_en = key.polygon.offset_en; key.polygon = p;
      color_st c = {0}; c.blend_en = key.color.blend_en; c.logic_op = key.color.logic_op; c.alpha_func = key.color.alpha_func; key.color = c;
      ds_st d = {0}; d.dt = key.depth.dt; d.dw = key.depth.dw; d.df = key.depth.df; d.st = key.depth.st; d.sf_f = key.depth.sf_f; d.sf_b = key.depth.sf_b; d.fail_f = key.depth.fail_f; d.zfail_f = key.depth.zfail_f;
      d.zpass_f = key.depth.zpass_f; d.fail_b = key.depth.fail_b; d.zfail_b = key.depth.zfail_b; d.zpass_b = key.depth.zpass_b; key.depth = d;
      for (unsigned i = 0; i < 8; i++) { blend_st b = {0}, o = key.blend[i]; b.target = o.target; b.src_rgb = o.src_rgb; b.dst_rgb = o.dst_rgb; b.src_a = o.src_a; b.dst_a = o.dst_a; b.eq_rgb = o.eq_rgb; b.eq_a = o.eq_a; key.blend[i] = b; } }
    pl->dyn_depth = key.depth; pl->dyn_polygon = key.polygon;
    if (depthall_list[0] && fs->module && strstr(depthall_list, fs->key)) { pl->dyn_depth.dt = 1; pl->dyn_depth.dw = 1; pl->dyn_depth.df = VK_COMPARE_OP_ALWAYS + 1; }
    if (eds_on) { memset(&key.depth, 0, sizeof key.depth); key.polygon.cull = 0; key.polygon.front = 0; if (eds2_on) key.polygon.offset_en = 0; }
    pl->key = key;
    /* what the key took from the inherited state, for the check */
    uint32_t used = PS_RT | PS_COLOR | PS_MASK | PS_MS | PS_POLY | PS_DEPTH | PS_CLAMP | PS_DISCARD | (ps->color.blend_en ? PS_BLEND : 0);
    pl->inherit |= (uint16_t)(used & ~set);
    pl->inh.rt = ps->rt; pl->inh.ms = ps->multisample; pl->inh.color = ps->color; pl->inh.mask = ps->mask; pl->inh.polygon = ps->polygon; pl->inh.depth = ps->depth; memcpy(pl->inh.blend, ps->blend, sizeof pl->inh.blend); pl->inh.depth_clamp = ps->depth_clamp; pl->inh.raster_discard = ps->raster_discard;
    pl->key_valid = 1; plan_reason[PR_OK]++;
}
void mtl_plan_predecode(const uint8_t *body, uint32_t body_bytes, uint64_t copy, uint64_t serial, uint64_t cb) {
    if (!plan_on() || !body_bytes) return;
    if (cb && *plan_cb_streak(cb) >= 3) { plan_cb_skipped++; return; }
    uint64_t t0 = dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID);
    nvn_state ps = state;   /* the inherited state as assumed: the queue state the translation last left */
    uint32_t set = 0; uint16_t attr_set = 0, stream_set = 0;
    piece_plan *pp = plan_slot(copy, serial); pp->cb = cb;
    const uint8_t *p = body + 8, *end = body + 8 + body_bytes;   /* past the recording's own OP_BEGIN */
    unsigned ops = 0;
    while (p < end) {
        op_hdr h; memcpy(&h, p, sizeof h); const uint8_t *a = p + 8;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || p + h.size > end) { pp->copy = 0; plan_bad++; return; }
        if (!(++ops & 255) && nvn_plan_yield && nvn_plan_yield()) { pp->copy = 0; plan_yielded++; plan_ns += dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID) - t0; return; }   /* a submit arrived: translating it comes first */
        switch (h.op) {
            case OP_CALL: pp->copy = 0; plan_calls_skipped++; return;   /* draws of another handle would be out of our count: no plan for this piece */
            case OP_DRAW: case OP_DRAW_IDX:
                if (pp->ndraws == pp->cap) { pp->cap = pp->cap ? pp->cap * 2 : 512; pp->draws = realloc(pp->draws, pp->cap * sizeof *pp->draws); if (!pp->draws) die("draw plan table"); }
                plan_build_draw(&ps, set, attr_set, stream_set, h.op == OP_DRAW ? (const p_draw *)a : NULL, h.op == OP_DRAW_IDX ? (const p_draw_idx *)a : NULL, &pp->draws[pp->ndraws++]);
                break;
            case OP_RENDER_COND: break;   /* decided at the submit from the counters' values then */
            case OP_BIND_VATTR: { const p_bind_vattr *b = (const void *)a; if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) for (int i = b->first; i < b->first + b->n; i++) attr_set |= (uint16_t)(1u << i); nvn_state_apply(&ps, h.op, a); break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const void *)a; if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) for (int i = b->first; i < b->first + b->n; i++) stream_set |= (uint16_t)(1u << i); nvn_state_apply(&ps, h.op, a); break; }
            default: if (nvn_state_apply(&ps, h.op, a)) set |= plan_category(h.op, a); break;
        }
        p += h.size;
    }
    plan_pieces_built++; plan_draws_built += pp->ndraws; plan_ns += dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID) - t0;
    { static uint64_t last; if (presents >= last + 600) { if (last) LOG("[vulkan-plan] %.1f pieces / %.0f draws planned a frame in %.2f ms; at the submit: %.0f draws in pieces, %.0f used a plan's shaders (%.0f its key), failed: program %.0f, signature %.0f, key %.0f, no key %.0f, no plan %.0f; pieces with calls %llu, bad %llu",
        plan_pieces_built / 600.0, plan_draws_built / 600.0, plan_ns / 600e6, plan_draws_seen / 600.0, plan_used / 600.0, plan_key_used / 600.0, plan_fail_program / 600.0, plan_fail_sig / 600.0, plan_fail_key / 600.0, plan_nokey / 600.0, plan_noplan / 600.0, (unsigned long long)plan_calls_skipped, (unsigned long long)plan_bad);
        LOG("[vulkan-plan] predecodes stopped for a submit: %.2f a frame; recordings skipped for their command buffer's unused streak: %.1f a frame", plan_yielded / 600.0, plan_cb_skipped / 600.0); plan_yielded = plan_cb_skipped = 0;
        LOG("[vulkan-plan] plans a frame by outcome: key %.0f, no program %.0f, stage %.0f, no variant %.0f, no image entry %.0f, block target %.0f, primitive %.0f, vertex format %.0f, divisor %.0f; planned but never seen at a submit: %.1f pieces / %.0f draws a frame",
            plan_reason[PR_OK] / 600.0, plan_reason[PR_NOPROGRAM] / 600.0, plan_reason[PR_STAGE] / 600.0, plan_reason[PR_NOLAST] / 600.0, plan_reason[PR_NOIMAGE] / 600.0, plan_reason[PR_BLOCK] / 600.0, plan_reason[PR_PRIM] / 600.0, plan_reason[PR_VFMT] / 600.0, plan_reason[PR_DIVISOR] / 600.0, plan_unused_pieces / 600.0, plan_unused_draws / 600.0);
        memset(plan_reason, 0, sizeof plan_reason); plan_unused_pieces = plan_unused_draws = 0;
        last = presents; plan_pieces_built = plan_draws_built = plan_ns = plan_draws_seen = plan_used = plan_key_used = plan_fail_program = plan_fail_sig = plan_fail_key = plan_nokey = plan_noplan = 0; } }
}
/* Phase two: the replay walks the submitted stream; a piece marker selects the piece's plan, each draw takes the next entry. */
static piece_plan *plan_cur; static uint32_t plan_draw_i; static int plan_depth;
/* Only the pieces copied into the submitted root (depth 0) select a plan: a piece's plan covers the whole of its flattened
   body, nested copies included, so a nested marker changes nothing (a nested recording's own plan, if one was made, goes
   unused and its command buffer's streak soon stops them). */
static void plan_piece_enter(uint64_t copy, uint64_t serial) {
    if (plan_depth++ != 0) return;
    plan_cur = plan_on() ? plan_find(copy, serial) : NULL; plan_draw_i = 0;
    if (plan_cur) { plan_cur->used = ++plan_clock; if (!plan_cur->looked_up && plan_cur->cb) *plan_cb_streak(plan_cur->cb) = 0; plan_cur->looked_up = 1; }
}
static void plan_piece_leave(void) { if (plan_depth > 0 && --plan_depth == 0) plan_cur = NULL; }
static const draw_plan *plan_next_draw(void) {
    if (plan_depth) plan_draws_seen++;
    if (!plan_cur) { if (plan_depth) plan_noplan++; return NULL; }
    if (plan_state == 2) { plan_draw_i++; return NULL; }   /* TOTK_VK_PLAN=2: plans are made but not used (the predecode's own cost) */
    if (plan_draw_i >= plan_cur->ndraws) { plan_noplan++; return NULL; }
    return &plan_cur->draws[plan_draw_i++];
}
static int plan_check(const draw_plan *pl) {
    if (!pl->vsfs) return 0;
    if (state.program != pl->program || state.program->serial != pl->program_serial) { plan_fail_program++; return 0; }
    if (totk_state_sig(&state, 0, &pl->vmask) != pl->vsig || (pl->fs_variant && totk_state_sig(&state, 1, &pl->fmask) != pl->fsig)) { plan_fail_sig++; return 0; }
    plan_used++; return 1;
}
static int plan_check_key(const draw_plan *pl, unsigned na, const VkAttachmentDescription *attachments, const VkAttachmentReference *colors, VkAttachmentReference depth, unsigned nblocks) {
    if (!pl->key_valid || nblocks) { plan_nokey++; return 0; }
    if (pl->na != na || pl->key.depth_attachment != depth.attachment || pl->key.colors != (uint32_t)state.rt.n) { plan_fail_key++; return 0; }
    for (int i = 0; i < state.rt.n; i++) { if (pl->key.color_indices[i] != colors[i].attachment || (colors[i].attachment != VK_ATTACHMENT_UNUSED && pl->key.formats[i] != attachments[colors[i].attachment].format)) { plan_fail_key++; return 0; } }
    if (depth.attachment != VK_ATTACHMENT_UNUSED && pl->key.formats[8] != attachments[depth.attachment].format) { plan_fail_key++; return 0; }
    unsigned inh = pl->inherit;
    if ((inh & PS_RT && memcmp(&state.rt, &pl->inh.rt, sizeof state.rt)) || (inh & PS_MS && memcmp(&state.multisample, &pl->inh.ms, sizeof state.multisample)) ||
        (inh & PS_COLOR && memcmp(&state.color, &pl->inh.color, sizeof state.color)) || (inh & PS_MASK && memcmp(&state.mask, &pl->inh.mask, sizeof state.mask)) ||
        (inh & PS_POLY && memcmp(&state.polygon, &pl->inh.polygon, sizeof state.polygon)) || (inh & PS_DEPTH && memcmp(&state.depth, &pl->inh.depth, sizeof state.depth)) ||
        (inh & PS_BLEND && memcmp(state.blend, pl->inh.blend, sizeof state.blend)) || (inh & PS_CLAMP && state.depth_clamp != pl->inh.depth_clamp) || (inh & PS_DISCARD && state.raster_discard != pl->inh.raster_discard)) { plan_fail_key++; return 0; }
    for (unsigned i = 0; i < 16; i++) { if (pl->inherit_attr >> i & 1 && memcmp(&state.attributes[i], &pl->inh.attributes[i], sizeof state.attributes[i])) { plan_fail_key++; return 0; }
        if (pl->inherit_stream >> i & 1 && memcmp(&state.streams[i], &pl->inh.streams[i], sizeof state.streams[i])) { plan_fail_key++; return 0; } }
    plan_key_used++; return 1;
}
#endif
#ifdef PLAN_PART2
/* TOTK_VK_DRAWSIG=1 census: would a per-draw memo across frames pay? Each draw's bindings are hashed (program, targets,
   the uniform / storage slots and texture slots its shaders use, vertex input, draw parameters, fixed state) and looked
   up in the previous frame's set; a hit is a draw that bound exactly what some draw of the previous frame bound. Variants
   leave one component out, to see what changes between frames. Counts only; no behaviour changes. */
/* each variant leaves a set of components out: bit 0 textures, 1 uniform addresses, 2 vertex buffers, 3 draw parameters, 4 storage
   addresses, 5 viewports, 6 scissor, 7 render targets, 8 program, 9 vertex attributes / streams, 10 fixed state (blend, masks, polygon, depth) */
#define DS_VARIANTS 14
#define DS_PARTS 11
static const uint16_t ds_omit[DS_VARIANTS] = {0, 128, 128 | 1, 128 | 2, 128 | 4, 128 | 8, 128 | 16, 128 | 2 | 4, 128 | 2 | 4 | 8, 128 | 2 | 4 | 8 | 16, 128 | 31, 31 | 128 | 1024, 31 | 32 | 64 | 128 | 512 | 1024, 31 | 32 | 64 | 128 | 512};
static const char *const ds_names[DS_VARIANTS] = {"full", "-targets", "-t-tex", "-t-ubo", "-t-vbuf", "-t-params", "-t-ssbo", "-t-ubo-vbuf", "-t-ubo-vbuf-params", "-t-ubo-vbuf-params-ssbo", "-t-bindings (program+state)", "p+s -fixed", "program only", "program+fixed"};
#define DS_SET 32768
#define DS_FRAMES 4   /* a draw matches one of the last four frames (uniform rings rotate over a few frames) */
static uint64_t *ds_prev[DS_VARIANTS][DS_FRAMES], *ds_cur[DS_VARIANTS]; static uint64_t ds_frame, ds_draws, ds_hits[DS_VARIANTS], ds_hits1[DS_VARIANTS], ds_reported;
static int drawsig_state = -1;
static inline void ds_mix(uint64_t *h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ull; } }
static int ds_lookup(uint64_t *set, uint64_t h) { if (!h) h = 1; for (unsigned i = (unsigned)(h >> 40) & (DS_SET - 1), n = 0; n < DS_SET; n++, i = (i + 1) & (DS_SET - 1)) { if (set[i] == h) return 1; if (!set[i]) return 0; } return 0; }
static void ds_insert(uint64_t *set, uint64_t h) { if (!h) h = 1; for (unsigned i = (unsigned)(h >> 40) & (DS_SET - 1), n = 0; n < DS_SET; n++, i = (i + 1) & (DS_SET - 1)) { if (set[i] == h) return; if (!set[i]) { set[i] = h; return; } } }
static void drawsig_note(const p_draw *draw, const p_draw_idx *indexed) {
    if (drawsig_state < 0) { const char *e = getenv("TOTK_VK_DRAWSIG"); drawsig_state = e && atoi(e) > 0; if (drawsig_state) for (int v = 0; v < DS_VARIANTS; v++) { for (int f = 0; f < DS_FRAMES; f++) { ds_prev[v][f] = calloc(DS_SET, 8); if (!ds_prev[v][f]) die("drawsig sets"); } ds_cur[v] = calloc(DS_SET, 8); if (!ds_cur[v]) die("drawsig sets"); } }
    if (!drawsig_state || !state.program) return;
    if (presents != ds_frame) {   /* a new frame: this frame's set becomes the newest previous one, the oldest is reused */
        for (int v = 0; v < DS_VARIANTS; v++) { uint64_t *t = ds_prev[v][DS_FRAMES - 1]; for (int f = DS_FRAMES - 1; f > 0; f--) ds_prev[v][f] = ds_prev[v][f - 1]; ds_prev[v][0] = ds_cur[v]; ds_cur[v] = t; memset(ds_cur[v], 0, DS_SET * 8); }
        ds_frame = presents;
        if (presents >= ds_reported + 600) { if (ds_reported && ds_draws) { char line[1200]; int k = 0; for (int v = 0; v < DS_VARIANTS; v++) k += snprintf(line + k, sizeof line - k, "%s %s %.1f%% (last frame %.1f%%)", v ? "," : "", ds_names[v], 100.0 * ds_hits[v] / ds_draws, 100.0 * ds_hits1[v] / ds_draws);
                LOG("[vulkan-drawsig] %.0f draws a frame; bound exactly what a draw of one of the last %d frames bound:%s", ds_draws / 600.0, DS_FRAMES, line); }
            ds_reported = presents; ds_draws = 0; memset(ds_hits, 0, sizeof ds_hits); memset(ds_hits1, 0, sizeof ds_hits1); }
    }
    uint64_t tex = 0, ubo = 0, ssbo = 0, vbuf = 0, params = 0, vp = 0, sc = 0, rt = 0, prog = 0, vl = 0, fixed = 0;
    ds_mix(&prog, &state.program, sizeof state.program); ds_mix(&prog, &state.program->serial, 8); ds_mix(&rt, &state.rt, sizeof state.rt);
    ds_mix(&fixed, &state.color, sizeof state.color); ds_mix(&fixed, &state.mask, sizeof state.mask); ds_mix(&fixed, &state.polygon, sizeof state.polygon); ds_mix(&fixed, &state.depth, sizeof state.depth); ds_mix(&fixed, state.blend, sizeof state.blend);
    ds_mix(&sc, &state.scissor, sizeof state.scissor); ds_mix(&vp, &state.viewports, sizeof state.viewports); ds_mix(&vl, state.attributes, sizeof state.attributes); ds_mix(&vl, state.streams, sizeof state.streams);
    ds_mix(&vbuf, state.vertices, sizeof state.vertices);
    if (draw) ds_mix(&params, draw, sizeof *draw); else ds_mix(&params, indexed, sizeof *indexed);
    for (unsigned i = 0; i < state.program->nstage && i < 6; i++) { shader_entry *sh = program_shader(state.program, i); unsigned st = sh->stage; if (st >= 6) continue;
        for (unsigned k = 0; k < sh->count; k++) { const shader_binding *b = &sh->bindings[k]; const shader_binding_plan *plan = &sh->binding_plan[k];
            if (descriptor_type_uniform(plan->type)) { if (b->slot >= 3 && b->slot < 19) ds_mix(&ubo, &state.ubo[st][b->slot - 3], sizeof state.ubo[0][0]); }
            else if (plan->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) { if (plan->storage_slot >= 0 && plan->storage_slot < 16) ds_mix(&ssbo, plan->uniform_pointer ? &state.ubo[st][plan->storage_slot] : &state.ssbo[st][plan->storage_slot], sizeof state.ubo[0][0]); }
            else if (plan->type != VK_DESCRIPTOR_TYPE_SAMPLER) for (unsigned j = 0; j < b->count; j++) { int slot = shader_texture_slot(b, j); if (slot >= 0 && slot < 128) { ds_mix(&tex, &state.textures[st][slot], 8); ds_mix(&tex, &state.samplers[st][slot], 8); } } } }
    uint64_t parts[DS_PARTS] = {tex, ubo, vbuf, params, ssbo, vp, sc, rt, prog, vl, fixed};
    for (int v = 0; v < DS_VARIANTS; v++) { uint64_t h = 1469598103934665603ull; for (int p = 0; p < DS_PARTS; p++) if (!(ds_omit[v] >> p & 1)) ds_mix(&h, &parts[p], 8);
        int hit = 0; for (int f = 0; f < DS_FRAMES && !hit; f++) hit = ds_lookup(ds_prev[v][f], h); if (hit) ds_hits[v]++; if (ds_lookup(ds_prev[v][0], h)) ds_hits1[v]++; ds_insert(ds_cur[v], h); }
    ds_draws++;
}
#endif
