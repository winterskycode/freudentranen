/* The camera jitter that temporal anti-aliasing needs, shared by Arm ASR (TOTK_TAA=asr, nvn_vulkan_asr.inc) and SMAA T2x
   (TOTK_AA=smaat2x, docked; nvn_vulkan_smaa.inc):
    - jitter: the camera's projection is offset by a sub-pixel step in clip space (Halton (2, 3); T2x's two positions),
      in our copies of the game's uniform blocks (taa_uniform), as a game with TAA of its own would: everything drawn with
      the camera moves together, and the game's screen-space effects project with the same matrices;
    - where: compose's output (taa_compose notes the target, taa_before_draw resolves it before the first draw that reads
      it); ASR instead replaces the game's own upscale (asr_upscale_draw);
    - motion: the camera's only, from the G-buffer depth and this and the previous frame's view-projection (the camera
      block frame generation reads, fg_after_pass);
    - history: reset on the first frame, a size change, a camera cut (fg's rule: 8 units in a frame) and any frame after
      one where it did not run (menus, loading). */
#define TAA_PHASES 16
static struct {
    /* -1 unread; 2 Arm ASR, 3 SMAA T2x */
    int on;
    /* ran: this frame */
    int cur, hist_ok, ran;
    uint32_t rw, rh;
    /* this frame's jitter, render pixels (x right, y down) */
    unsigned jit_i; float jx, jy;
    /* mode 3 (SMAA T2x): this frame's jitter position */
    int t2x_sub;
    /* the last frame's, as its matrices carried it (0: none) */
    float pjx, pjy;
    /* this frame's uniform blocks carried the jitter */
    int jittered;
    /* the camera's projection scales (last compose): what taa_uniform matches */
    float px, py;
    /* the G-buffer viewport (last frame): pixels to clip space */
    float vw, vh;
    /* this frame's G-buffer depth: the first G-buffer-shaped pass's (taa_viewport) */
    const image_entry *gdepth;
    /* compose's target, not yet resolved this frame (taa_before_draw) */
    image_entry *target; const void *target_mem;
    double prev_vp[16]; int prev_ok; float prev_pos[3];
    uint64_t done, resets, skipped;
} taa = {.on = -1};

static int taa_enabled_slow(void);
static inline int taa_enabled(void) { return taa.on >= 0 ? taa.on : taa_enabled_slow(); }
static int taa_enabled_slow(void) {
    if (taa.on < 0) { const char *e = getenv("TOTK_TAA"); taa.on = e && !strcmp(e, "asr") ? 2 : 0;
        /* SMAA T2x (TOTK_AA=smaat2x, docked): the two-position jitter, mode 3 */
        if (!taa.on) {
            const char *a = getenv("TOTK_AA"), *m = getenv("TOTK_OPERATION_MODE");
            if (a && !strcmp(a, "smaat2x") && m && !strcmp(m, "docked")) taa.on = 3; }
        LOG("[taa] temporal anti-aliasing (TOTK_TAA): %s", taa.on == 3 ? "SMAA T2x's two jitter positions" : taa.on == 2 ? "Arm ASR" : "off"); }
    return taa.on;
}
/* The menu's anti-aliasing changed while playing (nvn_vulkan.c): SMAA T2x's jitter on or off, docked, unless TOTK_TAA
   runs a temporal method of its own */
static void taa_aa_mode(int t2x) {
    taa_enabled();
    const char *m = getenv("TOTK_OPERATION_MODE");
    if (taa.on == 0 || taa.on == 3) { taa.on = t2x && m && !strcmp(m, "docked") ? 3 : 0; taa.hist_ok = 0; LOG("[taa] SMAA T2x jitter %s", taa.on == 3 ? "on" : "off"); }
}
static double taa_halton(unsigned i, unsigned base) { double f = 1, r = 0; while (i) { f /= base; r += f * (i % base); i /= base; } return r; }

static int taa_tracing(void) { static int t = -1; if (t < 0) { const char *e = getenv("TOTK_TAA_TRACE"); t = e ? atoi(e) : 0; if (t == 1) t = 1500; }
    /* TOTK_TAA_TRACE=<frame>: three frames from it (1: 1500) */
    return t > 0 && presents >= (uint64_t)t && presents < (uint64_t)t + 3; }
/* Every draw's viewport (draw_graphics_inner): the G-buffer pass's size, for the jitter's step in clip space. The frame's
   G-buffer depth is the depth of its first G-buffer-shaped pass (D32F the size of colour target 0, three or more colour
   targets). TOTK_TAA_TRACE=<frame>: the draws on a D32F depth in three frames from it, logged. */
static void taa_viewport(const VkViewport *viewport, const image_entry *depth, unsigned colors, const tex_t *c0, const shader_entry *vs, const shader_entry *fs) {
    if (taa.on <= 0 || !depth) return;
    int d32 = depth->format == VK_FORMAT_D32_SFLOAT || depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!taa.gdepth && colors >= 3 && c0 && d32 && depth->texture.d.width == c0->d.width && depth->texture.d.height == c0->d.height && depth->texture.d.width >= 320)
        taa.gdepth = depth;
    if (depth == taa.gdepth) { taa.vw = viewport->width; taa.vh = -viewport->height; }
    if (taa_tracing() && d32)
        LOG("[taa-trace] draw %llu %s vs %.16s fs %.16s colours %u target %ux%u depth %ux%u viewport %gx%g", (unsigned long long)draw_count, depth == taa.gdepth ? "g-buffer" : "other",
            vs ? vs->game_id : "-", fs && fs->module ? fs->game_id : "-", colors, c0 ? c0->d.width : 0, c0 ? c0->d.height : 0, depth->texture.d.width, depth->texture.d.height, viewport->width, -viewport->height);
}

static void smaa_scene_mark(int done);
static int smaa_t2x_apply(image_entry *target, image_entry *depth, int sub, const double *reproj, int reset);
static int taa_camera(uint32_t w, uint32_t h, double m[16]);
static int asr_upscale_draw(const shader_entry *fs);
/* The compose draw (draw_graphics_inner, before its bindings): note its target. */
static void taa_compose(const shader_entry *fs) {
    (void)fs;
    if (taa.on == 2 && (taa.ran || fg.menu)) return;
    if (taa.on == 2 && (!fg.depth || !fg.cam_ok)) { taa.skipped++; return; }
    if (state.rt.n && state.rt.color[0] && !fg.menu) { taa.target = image_get(state.rt.color[0]); taa.target_mem = state.rt.color[0]->mem; }
}
/* Every draw (draw_graphics_inner, before its bindings): the first one that samples compose's target anti-aliases it
   first (SMAA, or SMAA T2x with the jitter). */
static void taa_before_draw(const shader_entry *fs) {
    if (!taa.target || !fs || !fs->module) return;
    int reads = 0;
    for (unsigned i = 0; i < fs->count && !reads; i++) { const shader_binding *b = &fs->bindings[i];
        if (b->kind != 3) continue;
        for (unsigned e = 0; e < b->count && e < 32 && !reads; e++) {
            texture_descriptor d = descriptor_texture(shader_handle(b, fs->stage, e) & 0xfffff);
            reads = d.valid && d.texture.mem == taa.target_mem;
        }
    }
    if (!reads) return;
    image_entry *t = taa.target; taa.target = NULL;
    /* SMAA T2x on compose's output */
    if (taa.on == 3) {
        if (!fg.depth || !fg.cam_ok) { taa.skipped++; return; }
        double m[16]; int reset = taa_camera(t->texture.d.width, t->texture.d.height, m);
        if (smaa_t2x_apply(t, fg.depth, taa.t2x_sub, m, reset)) { smaa_scene_mark(1); taa.cur ^= 1; taa.hist_ok = 1; taa.ran = 1; taa.done++; }
        return;
    }
    if (taa.on <= 0) {
        if (smaa_apply(t)) { smaa_scene_mark(1);
            static int said; if (!said++) LOG("[aa] SMAA on the scene at its render size: compose's %ux%u output", t->texture.d.width, t->texture.d.height); }
        return;
    }
    /* ASR runs at the game's upscale (asr_upscale_draw) */
}
/* This frame's camera against the last one's (fg.cam: view-projection at floats 12..27, row-major): m = the last frame's
   view-projection times this one's inverse (identity on a reset). A reset: no history (taa.hist_ok), a size change, a
   camera cut (fg's distance, 8 units; a sharp turn is left to the history's own tests). Returns the reset. */
static int taa_camera(uint32_t w, uint32_t h, double m[16]) {
    double vc[16], ic[16]; for (int i = 0; i < 16; i++) vc[i] = fg.cam[12 + i];
    int reset = !taa.hist_ok || !taa.prev_ok || taa.rw != w || taa.rh != h || !fg_invert4(vc, ic);
    if (!reset) {
        double dx = fg.cam[47] - taa.prev_pos[0], dy = fg.cam[51] - taa.prev_pos[1], dz = fg.cam[55] - taa.prev_pos[2];
        reset = dx * dx + dy * dy + dz * dz > 64.0;
    }
    if (reset) { taa.resets++; for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0); }
    else for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += taa.prev_vp[r * 4 + k] * ic[k * 4 + c]; m[r * 4 + c] = s; }
    for (int i = 0; i < 16; i++) taa.prev_vp[i] = vc[i];
    taa.prev_pos[0] = fg.cam[47]; taa.prev_pos[1] = fg.cam[51]; taa.prev_pos[2] = fg.cam[55]; taa.prev_ok = 1;
    /* the projection's scales: what taa_uniform matches next frame */
    taa.px = fabsf(fg.cam[28]); taa.py = fabsf(fg.cam[33]);
    taa.rw = w; taa.rh = h;
    return reset;
}
/* Every uniform block read for the GPU (uniform_read_cached_read), in our copy (the game's memory is left alone): each
   4x4 that is the main camera's perspective matrix, projection or view-projection (row-major: last row the view's unit
   -z axis, rows 0 and 1 as long as the projection's x and y scales; the shadow cameras are orthographic) gets the
   jitter, row 0 += dx * row 3 and row 1 += dy * row 3 (clip-space x right, y up: the viewport flip turns a step down in
   pixels into -y). The camera block (2368 bytes: view-projection at float 12, projection at 28) carries the last
   frame's at 96 and 112: those get the last frame's jitter, so the game's own reprojection
   matches what it drew. Copies of this frame's: 64-byte (view-projection at 0), 144 (projection at 12), 224/256 (view-
   projection at 0), 496/512 (projection at 16, view-projection at 32), 704 (projection at 40 and 120). TOTK_TAA_TRACE:
   the matrices found in three frames, logged. */
static int taa_camera_matrix(const float *m) {
    float l = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
    if (fabsf(l - 1.0f) > 1e-3f) return 0;
    float r0 = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), r1 = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    return fabsf(r0 - taa.px) <= 1e-3f * taa.px && fabsf(r1 - taa.py) <= 1e-3f * taa.py;
}
static void taa_offset(float *m, float jx, float jy) {
    float dx = 2.0f * jx / taa.vw, dy = -2.0f * jy / taa.vh;
    for (int c = 0; c < 4; c++) { m[c] += dx * m[12 + c]; m[4 + c] += dy * m[12 + c]; }
}
static void taa_uniform(uintptr_t address, uint32_t size, uint8_t *data) {
    if (taa.on <= 0 || fg.menu || taa.px <= 0 || taa.vw <= 0 || taa.vh <= 0 || (taa.jx == 0 && taa.jy == 0 && taa.pjx == 0 && taa.pjy == 0)) return;
    float *f = (float *)data; uint32_t n = size / 4;
    int camera = n >= 44 && taa_camera_matrix(f + 12) && taa_camera_matrix(f + 28);
    for (uint32_t o = 0; o + 16 <= n; o += 4) {
        if (!taa_camera_matrix(f + o)) continue;
        int last = camera && (o == 96 || o == 112);
        if (taa_tracing()) LOG("[taa-mat] frame %llu block %#llx size %u f%u: %s", (unsigned long long)presents, (unsigned long long)address, size, o, last ? "last frame's" : "this frame's");
        if (!last) taa_offset(f + o, taa.jx, taa.jy);
        else taa_offset(f + o, taa.pjx, taa.pjy);
        o += 12; taa.jittered = 1;
    }
}
/* At the end of a frame: the next jitter; a frame it did not run in (menus, loading) breaks the history */
static void taa_frame_end(void) {
    smaa_scene_mark(0);
    if (taa.on <= 0) { taa.target = NULL; return; }
    if (!taa.ran) taa.hist_ok = 0;
    taa.pjx = taa.jittered ? taa.jx : 0; taa.pjy = taa.jittered ? taa.jy : 0;
    /* nothing read compose's target this frame */
    if (taa.target) { taa.skipped++; taa.target = NULL; }
    int ran = taa.ran;
    taa.ran = 0; taa.gdepth = NULL; taa.jittered = 0;
    taa.jit_i = (taa.jit_i + 1) % TAA_PHASES;
    /* not resolved this frame (menus, loading): the next one is drawn unjittered, nothing would average it */
    float k = ran ? 1.0f : 0.0f;
    /* SMAA T2x: (0.25, -0.25) and (-0.25, 0.25) with y up in the reference, x right / y down here */
    if (taa.on == 3) {
        taa.t2x_sub = taa.jit_i & 1; taa.jx = taa.jy = (taa.t2x_sub ? -0.25f : 0.25f) * k;
        return;
    }
    taa.jx = k * (float)(taa_halton(taa.jit_i + 1, 2) - 0.5); taa.jy = k * (float)(taa_halton(taa.jit_i + 1, 3) - 0.5);
}
