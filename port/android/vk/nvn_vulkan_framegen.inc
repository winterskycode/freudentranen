/* Frame generation (TOTK_FRAMEGEN=1; off by default): one picture halfway between two game frames, presented before the
   real one. Paired with a game frame cap of half the display rate (30 on a 60 Hz panel, 60 on 120 Hz), vsync then shows
   the generated and the real picture one refresh each: twice the frame rate for the same game work.
   The inputs are the ones the Mac port's MetalFX interpolation found (totk_mfx.m, nvn_mtl.m canvas_learn / mfx_pass_setup):
    - the canvas: the last colour target with the presented texture's shape before a pass into the presented texture,
      without depth or as the only colour target (with the HUD hidden, diving for one, the frame ends on the bloom
      composite, which keeps the depth bound: counting only depth-less passes would take the 640x360 bloom source);
    - the G-buffer depth: a D32F depth of the colour size with three or more colour targets, and the camera from that
      pass's first draw (vertex uniform 1, the 2368-byte block: view-projection at floats 12..27, row-major, position at
      47/51/55);
    - the scenes of this and the previous frame: the canvas (R11G11B10F display values, 1280x720 handheld, 1920x1080
      docked) copied before the game's first UI draw into it, so the interpolation never sees the HUD. The HUD then goes
      on top from this frame's finished picture wherever that differs from its scene (shaders/framegen_hud.comp), so
      it is never warped. LSFG (nvn_vulkan_lsfg.inc) takes the scenes and the HUD the same way;
    - without a scene (no UI drawn in a frame): the finished pictures of this and the previous frame (a copy kept here:
      the presented textures are reused).
   shaders/framegen_interp.comp picks per pixel between the camera motion and no motion (things that move with the
   camera, the HUD when it is in the pictures), whichever matches the two pictures better. A camera cut (more than 8
   units or 25 degrees in a frame), a missing input or the first frame present the real picture alone. */
static int fg_dump_state; static void fg_dump_image(image_entry *e, uint32_t w, uint32_t h, const char *tag);
static const char *const fg_ui_vs[] = {"7f2113e2", "e9fd7b43", "a662e313", "16684efc", "3e11cee5", "48c18ae6", "35cbc686", "4872bec9", "9f47ed90"};
static struct {
    int on;
    /* last_c0_img: this frame's */
    uint64_t swap[4]; int nswap; uint32_t swap_w, swap_h; uint64_t canvas, last_c0; image_entry *last_c0_img;
    /* a frame without UI anti-aliased at the copy into the presented texture */
    int aa_done; uint64_t aa_hudless;
    image_entry *depth; graphics_frame_entry *gbuffer; int want_cam, cam_ok, prev_ok, scene_done, cur;
    float cam[84], prev_cam[84];
    image_entry scene[2], out; int scene_valid[2];
    /* the previous frame's finished picture (top-left last_w x last_h of a presented-texture-sized image); kept_at:
       presents + 1 of the frame it was kept in */
    image_entry last; int last_ok; uint32_t last_w, last_h; uint64_t kept_at;
    VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline pipeline; VkSampler linear;
    VkDescriptorSetLayout hud_set_layout; VkPipelineLayout hud_layout; VkPipeline hud_pipeline;
    /* TOTK_LSFG: LSFG takes the scenes and the HUD too (nvn_vulkan_lsfg.inc) */
    int lsfg;
    /* TOTK_FG_HUD=generated: the HUD goes through frame generation with the scene (fg_scene) */
    int hud_generated;
    uint64_t made, frames, no_input, cuts, snapshots, hud_less, panel_limited;
    /* this frame's G-buffer viewport as a share of its target (dynres_note), 0 before it is known */
    float view[2];
    /* the current pass is a G-buffer pass; this frame's draws into G-buffer passes */
    int in_gbuffer; unsigned gdraws;
    /* menu mode (fg_menu_frame), and the frame cap to restore after it */
    int menu, low_frames, menu_cap; uint64_t menus;
    /* TOTK_FG_TRACE: frames left to log pass by pass (set when the canvas changes) */
    int trace; unsigned trace_draws;
} fg = {.on = -1, .lsfg = -1, .hud_generated = -1};

/* TOTK_FRAMEGEN=fsr<rate>: FSR 3.1 makes the picture (nvn_vulkan_fsrfg.inc), else the built-in pass */
static int fg_fsr;
static void fg_parse(const char *v, int *use_lsfg, int *hz, int *mult);
static int fg_enabled(void) {
    if (fg.on < 0) { int l, hz, m; fg_parse(getenv("TOTK_FRAMEGEN"), &l, &hz, &m); fg.on = !l && hz > 0; LOG("[framegen] %s", !fg.on ? "off" : fg_fsr && fsr_device_ok ? "on: FSR 3.1, a generated picture between game frames" : "on: a generated picture between game frames"); }
    return fg.on;
}
static int smaa_on(void); static int smaa_apply(image_entry *canvas); static void smaa_toggle(void); static int smaa_scene_done(void);
/* The canvas, the scene snapshots and the HUD are followed for either frame generation, and for anti-aliasing (the
   canvas before the first UI draw) */
/* TAA needs the G-buffer depth and the camera too */
static int taa_enabled(void);
static int mvs_on(void);
static void mo_frame(void);
static int fg_track(void) {
    if (fg.lsfg < 0) fg.lsfg = lsfg_requested();
    return fg_enabled() || fg.lsfg || smaa_on() || taa_enabled() || mvs_on();
}
static int fg_is_ui(const char *id) {
    for (size_t i = 0; i < sizeof fg_ui_vs / sizeof *fg_ui_vs; i++) if (!strncmp(id, fg_ui_vs[i], 8)) return 1;
    return 0;
}
/* Our own colour image as an image_entry, so the barrier tracking (image_access) and the view cache work on it. Called
   while recording: a new image is moved to GENERAL right away. */
static void fg_image_levels(image_entry *e, VkFormat format, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage);
static void fg_image(image_entry *e, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) { fg_image_levels(e, format, w, h, 1, usage); }
/* An image of our own (frame generation, anti-aliasing), in the general layout; recreated when its format or size
   changes. */
static void fg_image_levels(image_entry *e, VkFormat format, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage) {
    if (e->image && e->format == format && e->texture.d.width == w && e->texture.d.height == h && e->texture.d.levels == levels) return;
    if (e->image) {
        vkDeviceWaitIdle(device);
        for (cached_image_view *v = e->views, *n; v; v = n) { n = v->next; vkDestroyImageView(device, v->view, NULL); free(v); }
        vkDestroyImage(device, e->image, NULL); vkFreeMemory(device, e->memory.memory, NULL);
    }
    memset(e, 0, sizeof *e);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(device, &ci, NULL, &e->image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, e->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < memory_properties.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) && (memory_properties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    if (type == UINT32_MAX) die("No Vulkan memory for frame generation");
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VK_CHECK(vkAllocateMemory(device, &mi, NULL, &e->memory.memory)); VK_CHECK(vkBindImageMemory(device, e->image, e->memory.memory, 0));
    e->format = format; e->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    e->texture.d.width = w; e->texture.d.height = h; e->texture.d.depth = 1; e->texture.d.levels = levels; e->texture.d.target = NVN_TEXTURE_TARGET_2D; e->texture.L.layers = 1;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* TOTK_FG_TRACE=1: when the canvas changes, the next two frames are logged pass by pass (colour target, depth, the first
   draws' shaders), to see what the game draws between its canvas and the presented texture. */
static void fg_trace_start(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_FG_TRACE"); on = e && atoi(e) > 0; }
    /* the rest of this frame, then two whole ones */
    if (on) fg.trace = 3;
}
static void fg_trace_pass(graphics_frame_entry *frame, const image_entry *c0) {
    const graphics_pass_key *k = &frame->key.pass->key;
    const image_entry *d = k->depth != VK_ATTACHMENT_UNUSED && k->depth < k->attachments ? frame->images[k->depth] : NULL;
    LOG("[fg-trace] frame %llu pass %ux%u colours %u: c0 %llu fmt %d %ux%u%s, depth %llu%s%s", (unsigned long long)presents, frame->key.width, frame->key.height, k->colors,
        c0 ? (unsigned long long)c0->serial : 0ull, c0 ? (int)c0->format : 0, c0 ? c0->texture.d.width : 0, c0 ? c0->texture.d.height : 0,
        c0 && c0->serial == fg.canvas ? " (canvas)" : "", d ? (unsigned long long)d->serial : 0ull, fg.last_c0 && c0 && c0->serial == fg.last_c0 ? " (last full-screen)" : "",
        ({ int p = 0; for (int i = 0; i < fg.nswap; i++) p |= c0 && fg.swap[i] == c0->serial; p; }) ? " (presented)" : "");
    fg.trace_draws = 0;
}
static void fg_trace_draw(const shader_entry *vs, const shader_entry *fs) {
    if (fg.trace <= 0 || !vs) return;
    if (fg.trace_draws++ < 4 || fg_is_ui(vs->game_id))
        LOG("[fg-trace]   draw %u vs %.16s fs %.16s into %llu%s", fg.trace_draws, vs->game_id[0] ? vs->game_id : vs->key, fs && fs->module ? (fs->game_id[0] ? fs->game_id : fs->key) : "-",
            state.rt.n && state.rt.color[0] ? (unsigned long long)state.rt.color[0]->serial : 0ull, fg_is_ui(vs->game_id) ? " UI" : "");
}
/* At the start of every render pass: learn the canvas and note the G-buffer depth. */
static void fg_pass_begin(graphics_frame_entry *frame) {
    if (!fg_track()) return;
    const graphics_pass_key *k = &frame->key.pass->key;
    /* slot 0 may be unused */
    image_entry *c0 = k->colors && k->color_indices[0] < k->attachments ? frame->images[k->color_indices[0]] : NULL;
    if (fg.trace > 0) fg_trace_pass(frame, c0);
    if (c0 && fg.swap_w) {
        int to_swap = 0; for (int i = 0; i < fg.nswap; i++) to_swap |= fg.swap[i] == c0->serial;
        if (to_swap) {
            if (fg.last_c0 && fg.canvas != fg.last_c0) { fg.canvas = fg.last_c0; fg_trace_start(); LOG("[framegen] canvas: texture %llu (%ux%u), copied into presented texture %llu", (unsigned long long)fg.canvas, frame->key.width, frame->key.height, (unsigned long long)c0->serial); }
        } else if (frame->key.width >= 640 && (k->depth == VK_ATTACHMENT_UNUSED || k->colors == 1) && fabs((double)frame->key.width * fg.swap_h / ((double)frame->key.height * fg.swap_w) - 1) < 0.01)
            { fg.last_c0 = c0->serial; fg.last_c0_img = c0; }
    }
    image_entry *d = c0 && k->depth != VK_ATTACHMENT_UNUSED && k->depth < k->attachments && k->colors >= 3 ? frame->images[k->depth] : NULL;
    fg.in_gbuffer = d && (d->format == VK_FORMAT_D32_SFLOAT || d->format == VK_FORMAT_D32_SFLOAT_S8_UINT) && d->texture.d.width == c0->texture.d.width && d->texture.d.height == c0->texture.d.height && d->texture.d.width >= 320;
    if (fg.in_gbuffer && !fg.depth) { fg.depth = d; fg.gbuffer = frame; fg.want_cam = 1; }
}
/* A frame that does not draw the world: the pause menu (only Link in the G-buffer, ~40 draws, against thousands in
   play), loading screens. The game keeps the paused world in the canvas then and re-lights only Link, so the canvas
   carries over from frame to frame: anti-aliasing it in place each frame feeds back (dark dashes along the menu's bottom
   bar), and frame generation would only hold the menu's own animations (tab swipes) at the rendered rate. Neither runs
   in such frames, and in menu mode (three in a row) the frame cap goes up to the pictures shown (fg_shown_hz). */
#define FG_MENU_GBUFFER_DRAWS 200
int nvn_frame_cap_get(void) __attribute__((weak)); void nvn_frame_cap_set(int hz) __attribute__((weak));
static int fg_menu_frame(void) { return fg.gdraws < FG_MENU_GBUFFER_DRAWS; }
/* TOTK_FRAMEGEN: 0, <rate> (built-in, x2) or lsfg<rate>[x<multiplier>]: the game's rendered rate, and the pictures shown
   (rate x multiplier, the player's frame rate cap; host/app.c frame_settings). fg_shown_hz: those pictures a second, 0
   without frame generation; the in-game menu changes it (ov_apply). */
/* fg_adaptive: lsfg<rate>xa, the pictures shown match the display (nvn_vulkan_lsfg.inc); mult 1 then: the game renders
   at the cap */
static int fg_shown = -1, fg_adaptive;
static void fg_parse(const char *v, int *use_lsfg, int *hz, int *mult) {
    *use_lsfg = v && !strncmp(v, "lsfg", 4); fg_fsr = v && !strncmp(v, "fsr", 3); *hz = v ? atoi(*use_lsfg ? v + 4 : fg_fsr ? v + 3 : v) : 0; *mult = 2; fg_adaptive = 0;
    const char *x = *use_lsfg ? strchr(v, 'x') : NULL;
    if (x && x[1] == 'a') { *mult = 1; fg_adaptive = 1; } else if (x && atoi(x + 1) >= 2) *mult = atoi(x + 1);
}
static int fg_shown_hz(void) { if (fg_shown < 0) { int l, hz, m; fg_parse(getenv("TOTK_FRAMEGEN"), &l, &hz, &m); fg_shown = hz > 0 ? hz * m : 0; } return fg_shown; }
/* the overlay's frame cap: kept for after menu mode while it lasts */
static void fg_cap_set(int hz) { if (fg.menu_cap) fg.menu_cap = hz; else if (nvn_frame_cap_set) nvn_frame_cap_set(hz); }
static void fg_menu_update(void) {
    int low = fg_menu_frame(); fg.low_frames = low ? fg.low_frames + 1 : 0;
    { static unsigned lo = ~0u, hi, n; lo = fg.gdraws < lo ? fg.gdraws : lo; hi = fg.gdraws > hi ? fg.gdraws : hi;
      if (++n == 600) { LOG("[framegen] G-buffer draws a frame over 600 frames: %u..%u (menu mode below %d)", lo, hi, FG_MENU_GBUFFER_DRAWS); lo = ~0u; hi = n = 0; } }
    int menu = fg.low_frames >= 3;
    if (menu == fg.menu) return;
    fg.menu = menu;
    /* menus run at every picture shown */
    int cap = nvn_frame_cap_get ? nvn_frame_cap_get() : 0, hz = fg_shown_hz();
    if (menu) {
        fg.menus++;
        if ((fg.on > 0 || fg.lsfg > 0) && nvn_frame_cap_set && hz > cap) { fg.menu_cap = cap; nvn_frame_cap_set(hz); }
        LOG("[framegen] menu mode (%u G-buffer draws): no anti-aliasing or frame generation, frame cap %d", fg.gdraws, fg.menu_cap ? hz : cap);
    } else {
        if (fg.menu_cap && nvn_frame_cap_set) nvn_frame_cap_set(fg.menu_cap);
        LOG("[framegen] menu mode off (%u G-buffer draws), frame cap %d", fg.gdraws, fg.menu_cap ? fg.menu_cap : cap);
        fg.menu_cap = 0;
    }
}
static int fg_scene_copy(image_entry *c);
static int fg_hud_generated(void);
/* Before a draw opens its pass: the first UI draw into the canvas ends the scene; copy it. The UI shaders also draw
   panels into small textures of their own (480x300, 380x70 in handheld mode) before compositing them: those are not the
   canvas. In both console modes the canvas is R11G11B10F (1280x720 handheld, 1920x1080 docked) with display values. */
static void fg_before_draw(const shader_entry *vs) {
    if (!fg_track() || fg.scene_done || !vs || !state.rt.n || !state.rt.color[0]) return;
    int presented = 0; for (int i = 0; i < fg.nswap; i++) presented |= fg.swap[i] == state.rt.color[0]->serial;
    if (!fg_is_ui(vs->game_id)) {
        /* a frame without UI (the HUD hidden: diving, ...): the canvas anti-aliased at the game's copy into the presented
           texture. That draw's barrier for sampling it is already recorded, so it is made again after ours. */
        image_entry *c = fg.last_c0_img;
        if (presented && !fg.aa_done && smaa_on() && !smaa_scene_done() && !fg_menu_frame() && c && c->serial == fg.canvas) {
            fg.aa_done = 1; if (smaa_apply(c)) fg.aa_hudless++, image_access(c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); }
        return;
    }
    if (!presented && (!fg.canvas || state.rt.color[0]->serial != fg.canvas)) return;
    image_entry *c = image_get(state.rt.color[0]);
    /* the composite and the encodes take the scene as linear values: a UI draw straight into an 8-bit presented
       texture (no canvas yet) gives no scene, and the HUD stays in this frame's generated pictures */
    if (c->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && c->format != VK_FORMAT_R16G16B16A16_SFLOAT) return;
    graphics_pass_end();
    /* anti-aliasing before the UI (and before the scene snapshot, which gets it too) */
    int touched = smaa_on() && !smaa_scene_done() && !fg_menu_frame() && smaa_apply(c);
    touched |= fg_scene_copy(c);
    /* the UI draw's barrier for its colour target was recorded before the anti-aliasing and the copy: made again */
    if (touched) image_access(c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}
/* this frame's scene snapshot for frame generation (none for anti-aliasing alone) */
static int fg_scene_copy(image_entry *c) {
    /* TOTK_FG_HUD=generated: no scene is read (fg_scene) */
    if ((!fg_enabled() && !fg.lsfg) || fg_hud_generated()) { fg.scene_done = 1; return 0; }
    image_entry *s = &fg.scene[fg.cur];
    fg_image(s, c->format, c->texture.d.width, c->texture.d.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    image_access(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_access(s, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {c->texture.d.width, c->texture.d.height, 1}};
    vkCmdCopyImage(command, c->image, VK_IMAGE_LAYOUT_GENERAL, s->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    fg.scene_done = 1; fg.scene_valid[fg.cur] = 1; fg.snapshots++;
    return 1;
}
/* After a draw opened its pass: the camera from the G-buffer pass's first draw. */
/* TOTK_MV_SURVEY=1 (diagnostic): whether last frame's copy of each object block can be found, for per-object motion
   vectors (docs/recomp-spike.md). The game writes an object's vertex-stage block into two copies that take turns, at
   addr and addr +- step (its size rounded up to 256), as totk-pc-recomp's vk_mvprev.c describes. Per G-buffer draw and
   bank (binding index = bank - 3: c5 bones, c6, c7 place, c9, c11, c14), once per block a frame, last frame's copy is:
     self    bound at the same address last frame
     side    one neighbour was bound in that bank last frame
     nearer  both were: the one with more of its first 16 words equal to this frame's
     none    neither
   and the copy found is compared with this frame's block by totk-pc-recomp's mo_similar rule (every float within 10 %,
   at least 1.0 and at most 4.0 units). The neighbours of c5 and c7 are captured at the submit call (mtl_mv_banks), so
   the copy is read from there as the game left it; the other banks' neighbours are read from guest memory at replay,
   where the game may already be writing the next frame. Every 600 frames a summary per bank (with the share read from
   the capture), the bytes a capture of both neighbours of every bank would add, and the survey's own time. */
#define MVS_TABLE (1u << 15)
/* prev, memo, changed: mv_prev_block's answer for the block this frame (memo 1 last frame's copy at prev, -1 none) */
typedef struct { uint64_t addr, prev; uint32_t bank, size, memo_size; int8_t memo, changed; } mvs_seen;
typedef struct { uint64_t blocks, self, side, nearer, none, similar, changed, different, still, small, big, captured; } mvs_stat;
/* on: the tables are kept this frame (the survey or object motion); survey: the summaries are logged. motion
   (TOTK_OBJ_MOTION): -1 when a consumer of object motion runs (the built-in or FSR 3.1 frame generation, Arm ASR;
   not LSFG, which takes only pictures), the default; 0 off; 1 always; 2 still objects too (diagnostic). Settled once a
   frame (mvs_frame), so it follows the in-game menu. */
static struct { int on, survey, motion, mo; mvs_seen *t[2]; uint64_t frames, extra_bytes, extra_blocks, ns, full; mvs_stat st[14]; } mvs = {.on = -1};
static int mvs_settle(void) {
    if (mvs.on < 0) { const char *e = getenv("TOTK_MV_SURVEY"), *m = getenv("TOTK_OBJ_MOTION"); mvs.survey = e && atoi(e) > 0; mvs.motion = m && *m ? atoi(m) : -1; if (mvs.motion < -1) mvs.motion = -1;
        if (mvs.survey) LOG("[mv-survey] on: last frame's object blocks in G-buffer draws (c5 c6 c7 c9 c11 c14), a summary every 600 frames"); }
    int motion = mvs.motion < 0 ? fg_enabled() > 0 || taa_enabled() == 2 : mvs.motion > 0;
    int on = mvs.survey || motion;
    if (on && !mvs.t[0]) { mvs.t[0] = calloc(MVS_TABLE, sizeof(mvs_seen)); mvs.t[1] = calloc(MVS_TABLE, sizeof(mvs_seen)); if (!mvs.t[0] || !mvs.t[1]) die("motion survey tables"); }
    /* coming on: no blocks of this frame or a last frame yet */
    if (on && mvs.on <= 0) { memset(mvs.t[0], 0, MVS_TABLE * sizeof(mvs_seen)); memset(mvs.t[1], 0, MVS_TABLE * sizeof(mvs_seen)); }
    if (motion != mvs.mo && (mvs.on >= 0 || motion)) LOG("[motion] object motion %s", motion ? "on" : "off");
    mvs.on = on; mvs.mo = motion;
    return on;
}
static int mvs_on(void) { return mvs.on >= 0 ? mvs.on : mvs_settle(); }
/* object motion this frame (not the survey alone) */
static int mvs_motion(void) { return mvs_on() && mvs.mo; }
static mvs_seen *mvs_find(mvs_seen *t, uint64_t addr, uint32_t bank, int add) {
    uint32_t i = (uint32_t)(((addr ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull) >> 40) & (MVS_TABLE - 1);
    for (uint32_t k = 0; k < MVS_TABLE; k++, i = (i + 1) & (MVS_TABLE - 1)) {
        if (t[i].addr == addr && t[i].bank == bank) return &t[i];
        if (!t[i].addr) { if (!add) return NULL; t[i].addr = addr; t[i].bank = bank; return &t[i]; }
    }
    mvs.full++; return NULL;
}
/* The vertex binding indices whose two-copy neighbours the submit capture takes (hle_nvn.c flatten_recording): c5 bones
   and c7 place, while the survey or object motion is on. Object motion takes last frame's copy of these two only (the
   camera's is in its own block, mo_note_camera); other banks keep this frame's. */
uint32_t mtl_mv_banks(void) { return !mvs_on() ? 0 : 1u << 2 | 1u << 4; }
/* a block's bytes from the submit capture when it holds them, else from guest memory */
static uint8_t mvs_buf[65536];
static const uint8_t *mvs_bytes(uint64_t addr, uint64_t size, int *captured) {
    *captured = ucap_covers((uintptr_t)addr, (uint32_t)size);
    if (!*captured) return (const uint8_t *)(uintptr_t)addr;
    ucap_apply((uintptr_t)addr, mvs_buf, (uint32_t)size); return mvs_buf;
}
/* totk-pc-recomp runtime/vk_exec_draw.c mo_similar (MIT, Copyright (c) 2026 Valiantsin Dzerakh) */
static int mvs_similar(const uint8_t *cur, const uint8_t *old, uint64_t n, int *changed) {
    *changed = 0;
    if (!memcmp(cur, old, n)) return 1;
    const uint32_t *x = (const uint32_t *)cur, *y = (const uint32_t *)old;
    for (uint64_t w = 0; w < n / 4; w++) {
        if (x[w] == y[w]) continue;
        uint32_t ex = x[w] >> 23 & 0xff, ey = y[w] >> 23 & 0xff;
        if (!ex || ex == 0xff || !ey || ey == 0xff) continue;
        float fx, fy; memcpy(&fx, &x[w], 4); memcpy(&fy, &y[w], 4);
        float dd = fabsf(fx - fy), tol = 0.1f * fabsf(fy);
        if (dd > (tol < 1.0f ? 1.0f : tol > 4.0f ? 4.0f : tol)) return 0;
    }
    *changed = 1; return 1;
}
static void mvs_draw(void) {
    /* the survey's banks, or c5 and c7 for object motion alone, which only needs the blocks noted */
    static const uint32_t survey_banks[] = {2, 3, 4, 6, 8, 11}, motion_banks[] = {2, 4};
    const uint32_t *banks = mvs.survey ? survey_banks : motion_banks; unsigned nbanks = mvs.survey ? 6 : 2;
    uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    for (unsigned b = 0; b < nbanks; b++) {
        uint32_t bank = banks[b]; const p_bind_buf *u = &state.ubo[0][bank];
        if (!u->addr || !u->size || u->size > 65536) continue;
        mvs_seen *e = mvs_find(mvs.t[0], u->addr, bank, 1);
        if (!e || e->size) continue;
        e->size = (uint32_t)u->size;
        if (!mvs.survey) continue;
        mvs_stat *st = &mvs.st[bank]; st->blocks++;
        uint64_t step = (u->size + 255) & ~(uint64_t)255;
        mvs.extra_bytes += 2 * u->size; mvs.extra_blocks += 2;
        if (mvs_find(mvs.t[1], u->addr, bank, 0)) { st->self++; continue; }
        int up = mvs_find(mvs.t[1], u->addr + step, bank, 0) != NULL, down = u->addr >= step && mvs_find(mvs.t[1], u->addr - step, bank, 0) != NULL;
        if (!up && !down) { st->none++; continue; }
        if (uniform_read_current((uintptr_t)u->addr, (uint32_t)u->size)) { st->none++; continue; }
        const uint8_t *cur = uniform_read_scratch;
        uint64_t prev = up ? u->addr + step : u->addr - step;
        int captured;
        if (up && down) {
            /* the first 16 words of each: the up neighbour copied aside, as mvs_bytes reuses its buffer */
            uint32_t a[16] = {0}; size_t words = u->size < 64 ? (size_t)u->size / 4 : 16;
            memcpy(a, mvs_bytes(u->addr + step, u->size, &captured), words * 4);
            const uint32_t *c = (const uint32_t *)cur, *d = (const uint32_t *)mvs_bytes(u->addr - step, u->size, &captured);
            int na = 0, nd = 0; for (size_t w = 0; w < words; w++) na += c[w] == a[w], nd += c[w] == d[w];
            if (nd > na) prev = u->addr - step;
            st->nearer++;
        } else st->side++;
        int changed; const uint8_t *old = mvs_bytes(prev, u->size, &captured); st->captured += (uint64_t)captured;
        if (!mvs_similar(cur, old, u->size, &changed)) { st->different++; continue; }
        st->similar++; st->changed += (uint64_t)changed;
        if ((bank == 2 || bank == 4) && u->size >= 48) {
            const float *x = (const float *)cur, *y = (const float *)old;
            float dx = x[3] - y[3], dy = x[7] - y[7], dz = x[11] - y[11], m = sqrtf(dx * dx + dy * dy + dz * dz);
            if (m == 0) st->still++; else if (m < 1) st->small++; else st->big++;
        }
    }
    mvs.ns += dm_android_clock_ns(CLOCK_MONOTONIC) - t0;
}
/* Last frame's copy of the vertex block at binding index `index` (c5 or c7; the other banks keep this frame's), by the
   two-copy rule and the similarity check (as mvs_draw counts them), read from the submit capture where it holds the
   copy: *out with that address, or this frame's block when there is no copy (bound at the same address last frame, a
   new object, another object's block). */
/* set by mv_prev_block: the last block it looked up had a copy of last frame that differs from this frame's */
static int mv_prev_changed;
static const p_bind_buf *mv_prev_lookup(unsigned stage, unsigned index, p_bind_buf *out) {
    mv_prev_changed = 0;
    const p_bind_buf *u = index < 16 ? &state.ubo[stage][index] : NULL;
    if (!u || stage != 0 || mvs.on <= 0 || (index != 2 && index != 4) || !u->addr || !u->size || u->size > 65536 || !mvs.t[1]) return u;
    if (mvs_find(mvs.t[1], u->addr, index, 0)) return u;
    uint64_t step = (u->size + 255) & ~(uint64_t)255;
    int up = mvs_find(mvs.t[1], u->addr + step, index, 0) != NULL, down = u->addr >= step && mvs_find(mvs.t[1], u->addr - step, index, 0) != NULL;
    if (!up && !down) return u;
    if (uniform_read_current((uintptr_t)u->addr, (uint32_t)u->size)) return u;
    static uint8_t cur[65536]; memcpy(cur, uniform_read_scratch, u->size);
    uint64_t prev = up ? u->addr + step : u->addr - step; int captured;
    if (up && down) {
        uint32_t a[16] = {0}; size_t words = u->size < 64 ? (size_t)u->size / 4 : 16;
        memcpy(a, mvs_bytes(u->addr + step, u->size, &captured), words * 4);
        const uint32_t *c = (const uint32_t *)cur, *d = (const uint32_t *)mvs_bytes(u->addr - step, u->size, &captured);
        int na = 0, nd = 0; for (size_t w = 0; w < words; w++) na += c[w] == a[w], nd += c[w] == d[w];
        if (nd > na) prev = u->addr - step;
    }
    int changed;
    if (!mvs_similar(cur, mvs_bytes(prev, u->size, &captured), u->size, &changed)) return u;
    mv_prev_changed = changed; *out = *u; out->addr = prev; return out;
}
/* mv_prev_lookup once a frame per block: an object's draws share their blocks */
static const p_bind_buf *mv_prev_block(unsigned stage, unsigned index, p_bind_buf *out) {
    const p_bind_buf *u = index < 16 ? &state.ubo[stage][index] : NULL;
    mvs_seen *e = u && stage == 0 && mvs.on > 0 && (index == 2 || index == 4) && u->addr && mvs.t[0] ? mvs_find(mvs.t[0], u->addr, index, 1) : NULL;
    if (e && e->memo && e->memo_size == u->size) {
        mv_prev_changed = e->changed;
        if (e->memo < 0) return u;
        *out = *u; out->addr = e->prev; return out; }
    const p_bind_buf *r = mv_prev_lookup(stage, index, out);
    if (e) { e->memo_size = (uint32_t)u->size; e->changed = (int8_t)mv_prev_changed; e->memo = r == out ? 1 : -1; e->prev = r->addr; }
    return r;
}
static void mvs_frame(void) {
    int was = mvs.on > 0;
    if (!mvs_settle() || !was) return;
    mvs_seen *t = mvs.t[1]; mvs.t[1] = mvs.t[0]; mvs.t[0] = t; memset(t, 0, MVS_TABLE * sizeof *t);
    if (++mvs.frames % 600 || !mvs.survey) return;
    for (int b = 0; b < 14; b++) { const mvs_stat *s = &mvs.st[b]; if (!s->blocks) continue; char name[8]; snprintf(name, sizeof name, "c%d", b + 3);
        double n = (double)s->blocks, found = (double)(s->side + s->nearer), moved = (double)(s->still + s->small + s->big);
        LOG("[mv-survey] %s: %.0f blocks a frame: self %.1f%%, side %.1f%%, nearer %.1f%%, none %.1f%%; of the neighbours found %.1f%% from the capture, %.1f%% similar (%.1f%% changed), %.1f%% different; moved: still %.1f%%, under 1 unit %.1f%%, more %.1f%%",
            name, n / 600, 100 * s->self / n, 100 * s->side / n, 100 * s->nearer / n, 100 * s->none / n,
            found ? 100 * s->captured / found : 0, found ? 100 * s->similar / found : 0, found ? 100 * s->changed / found : 0, found ? 100 * s->different / found : 0,
            moved ? 100 * s->still / moved : 0, moved ? 100 * s->small / moved : 0, moved ? 100 * s->big / moved : 0); }
    LOG("[mv-survey] capturing both neighbours would add %.1f KiB in %.0f blocks a frame; survey %.3f ms a frame%s",
        mvs.extra_bytes / 600.0 / 1024, mvs.extra_blocks / 600.0, mvs.ns / 600.0 / 1e6, mvs.full ? " (table full)" : "");
    memset(mvs.st, 0, sizeof mvs.st); mvs.extra_bytes = mvs.extra_blocks = mvs.ns = mvs.full = 0;
}
static void fg_after_pass(void) {
    fg.gdraws += (unsigned)fg.in_gbuffer;
    if (fg.in_gbuffer && mvs.on > 0) mvs_draw();
    if ((fg.on <= 0 && !taa_enabled()) || !fg.want_cam || graphics_active_frame != fg.gbuffer) return;
    fg.want_cam = 0;
    const p_bind_buf *u = &state.ubo[0][1];
    if (u->addr && u->size == 2368 && !uniform_read_current((uintptr_t)u->addr, sizeof fg.cam)) { memcpy(fg.cam, uniform_read_scratch, sizeof fg.cam); fg.cam_ok = 1; }
    else { static int said; if (said++ < 5) LOG("[framegen] G-buffer pass: vertex uniform 1 is %#llx / %u bytes, not the 2368-byte camera block", (unsigned long long)u->addr, (unsigned)u->size); }
}

/* row-major, Gauss-Jordan with partial pivoting */
static int fg_invert4(const double a[16], double o[16]) {
    double m[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) m[r][c] = c < 4 ? a[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c; for (int r = c + 1; r < 4; r++) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { double t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
        double d = m[c][c]; for (int k = 0; k < 8; k++) m[c][k] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = m[r][c]; for (int k = 0; k < 8; k++) m[r][k] -= f * m[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = m[r][c + 4];
    return 1;
}

static void fg_pipeline(void) {
    if (fg.pipeline) return;
    VkDescriptorSetLayoutBinding b[5];
    for (int i = 0; i < 5; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &fg.set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 112};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &fg.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &fg.layout));
    static const uint32_t code[] = {
#include "framegen_interp_spv.inc"
    };
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = fg.layout};
    VK_CHECK(pipeline_create_compute(&pi, &fg.pipeline)); vkDestroyShaderModule(device, module, NULL);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &fg.linear));
}

/* TOTK_FG_HUD: "real" (default) puts this frame's HUD on the generated pictures, sharp but moving at the rendered rate
   (labels that follow things in the world step across the screen); "generated" interpolates the finished pictures, HUD
   included: smooth labels, text may smear while moving. Live from the in-game menu (ov_apply). */
static int fsr_fg_enabled(void);
static int fg_hud_generated(void) {
    if (fg.hud_generated < 0) { const char *e = getenv("TOTK_FG_HUD"); fg.hud_generated = e && !strcmp(e, "generated"); }
    /* FSR moves everything with the camera: a HUD in its input comes out doubled */
    return fg.hud_generated && !(fg.on > 0 && fsr_fg_enabled());
}
/* This frame's scene (the canvas before the UI) when it was copied and has the picture's size, else NULL; prev: the
   previous frame's instead (NULL unless both frames have one). */
static image_entry *fg_scene(uint32_t w, uint32_t h, int prev) {
    if (!fg.scene_done || fg_hud_generated()) return NULL;
    image_entry *s = &fg.scene[prev ? fg.cur ^ 1 : fg.cur];
    if (prev && !fg.scene_valid[fg.cur ^ 1]) return NULL;
    if (s->texture.d.width != w || s->texture.d.height != h) {
        static int said; if (said++ < 3) LOG("[framegen] scene %ux%u is not the picture's size %ux%u: the HUD stays in generated pictures", s->texture.d.width, s->texture.d.height, w, h);
        return NULL;
    }
    return s;
}
/* The HUD over a generated picture made from the scenes (mode 0), or a scene encoded (mode 1): recorded into the
   current command buffer. */
static void fg_hud_pass(image_entry *dst, image_entry *fin, image_entry *scene, image_entry *scene_prev, image_entry *fin_prev, uint32_t w, uint32_t h, int mode) {
    if (!fg.hud_pipeline) {
        /* for the sampler */
        fg_pipeline();
        VkDescriptorSetLayoutBinding b[5];
        for (int i = 0; i < 5; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = i != 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &fg.hud_set_layout));
        VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 20};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &fg.hud_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
        VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &fg.hud_layout));
        static const uint32_t code[] = {
#include "framegen_hud_spv.inc"
        };
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = fg.hud_layout};
        VK_CHECK(pipeline_create_compute(&pi, &fg.hud_pipeline)); vkDestroyShaderModule(device, module, NULL);
    }
    image_access(fin, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(scene, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(dst, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    /* the previous frame's scene and finished picture: the see-through share of translucent UI (framegen_hud.comp) */
    int has_prev = scene_prev && fin_prev;
    if (has_prev) { image_access(scene_prev, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(fin_prev, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); }
    /* stand-ins, unused */
    else scene_prev = scene, fin_prev = fin;
    VkDescriptorImageInfo img[5] = {
        {fg.linear, attachment_view(fin, NULL, fin->format), VK_IMAGE_LAYOUT_GENERAL}, {fg.linear, attachment_view(scene, NULL, scene->format), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, attachment_view(dst, NULL, dst->format), VK_IMAGE_LAYOUT_GENERAL},
        {fg.linear, attachment_view(scene_prev, NULL, scene_prev->format), VK_IMAGE_LAYOUT_GENERAL}, {fg.linear, attachment_view(fin_prev, NULL, fin_prev->format), VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorSet set = descriptor_allocate(fg.hud_set_layout);
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++) wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = i != 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img[i]};
    vkUpdateDescriptorSets(device, 5, wr, 0, NULL);
    struct { int32_t w, h; float threshold; int32_t mode, has_prev; } push = {(int32_t)w, (int32_t)h, 2.5f / 255.0f, mode, has_prev};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, fg.hud_pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, fg.hud_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, fg.hud_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1);
    if (!mode) fg.hud_less++;
}
static void fg_hud_composite(image_entry *dst, image_entry *fin, image_entry *scene, uint32_t w, uint32_t h) {
    /* the previous frame's finished picture (fg_keep_picture) and scene, when both are that frame's at this size */
    image_entry *scene_prev = fg_scene(w, h, 1), *fin_prev = fg.last_ok && fg.last_w == w && fg.last_h == h ? &fg.last : NULL;
    fg_hud_pass(dst, fin, scene, scene_prev, fin_prev, w, h, 0);
}
/* The scene sRGB-encoded into dst (an RGBA8 storage image), as the game's last pass would write it without the UI. */
static void fg_scene_encode(image_entry *dst, image_entry *scene, uint32_t w, uint32_t h) { fg_hud_pass(dst, scene, scene, NULL, NULL, w, h, 1); }

/* At the game's present, before the real picture: the generated picture (an image_entry to present), or NULL. */
#include "nvn_vulkan_fsrfg.inc"
static image_entry *mo_image(void);
static image_entry *fg_prepare(const tex_t *t, const int crop[4]) {
    if (fg.on <= 0) return NULL;
    fg.frames++;
    /* behind the boot logo: about 300 ms the first time, then from the pipeline cache */
    if (fsr_fg_enabled()) fsr_pipelines();
    /* from the first menu frame on, not three frames in */
    if (fg.menu || fg_menu_frame()) return NULL;
    /* the panel has no room for it (pace_refresh_poll) */
    if (pace_pictures_fit(nvn_frame_cap_get ? nvn_frame_cap_get() : 0) < 2) { fg.panel_limited++; return NULL; }
    if (!fg.cam_ok || !fg.prev_ok || !fg.depth || !fg.last_ok) { fg.no_input++; return NULL; }
    double dx = fg.cam[47] - fg.prev_cam[47], dy = fg.cam[51] - fg.prev_cam[51], dz = fg.cam[55] - fg.prev_cam[55];
    double turn = fg.cam[8] * fg.prev_cam[8] + fg.cam[9] * fg.prev_cam[9] + fg.cam[10] * fg.prev_cam[10];
    if (dx * dx + dy * dy + dz * dz > 64.0 || turn < cos(25.0 * M_PI / 180.0)) { fg.cuts++; return NULL; }
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = fg.cam[12 + i]; vp[i] = fg.prev_cam[12 + i]; }
    if (!fg_invert4(vc, ic)) { fg.no_input++; return NULL; }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += vp[r * 4 + k] * ic[k * 4 + c]; m[r * 4 + c] = s; }
    uint32_t sw = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, sh = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    if ((crop && (crop[0] || crop[1])) || fg.last.texture.d.width != t->d.width || fg.last.texture.d.height != t->d.height || fg.last_w != sw || fg.last_h != sh) { fg.no_input++; return NULL; }
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    fg_pipeline();
    image_entry *fin = image_get(t);
    fg_image(&fg.out, VK_FORMAT_R8G8B8A8_UNORM, sw, sh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    /* the scenes of both frames when there are (no HUD to warp; it goes on top from this frame's picture), else the
       finished pictures */
    image_entry *scene = fg_scene(sw, sh, 0), *scene_prev = scene ? fg_scene(sw, sh, 1) : NULL;
    image_entry *a = scene_prev ? scene_prev : &fg.last, *b = scene_prev ? scene : fin;
    image_access(a, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(fin, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(fg.depth, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(&fg.out, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkImageView depth_view = image_view_formatted(fg.depth, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, (VkComponentMapping){0}, fg.depth->format)->view;
    float push[28];
    /* GLSL mat4 is column-major */
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) push[c * 4 + r] = (float)m[r * 4 + c];
    push[16] = (float)sw; push[17] = (float)sh; push[18] = 1.0f / sw; push[19] = 1.0f / sh;
    static int debug; static uint64_t debug_next; uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC);
    /* <files>/fgdebug: tint by decision */
    if (now > debug_next) { debug_next = now + 500000000ull; const char *d = getenv("TOTK_FILES_DIR"); char path[600];
        if (d) { snprintf(path, sizeof path, "%s/fgdebug", d); debug = !access(path, F_OK); } }
    push[20] = 1.0f; push[21] = 0.08f; push[22] = debug ? 1.0f : 0.0f; push[23] = scene_prev ? 1.0f : 0.0f;
    /* the picture is the top-left of the presented texture (the scenes are its size) */
    image_entry *obj = mo_image();
    push[24] = (float)sw / b->texture.d.width; push[25] = (float)sh / b->texture.d.height; push[26] = obj ? 1.0f : 0.0f; push[27] = 0;
    if (fsr_fg_enabled()) fsr_fg_run(b, depth_view, push, sw, sh, &fg.out, fg.cam, fg.frames);
    else {
    VkDescriptorImageInfo img[5] = {
        {fg.linear, attachment_view(a, NULL, a->format), VK_IMAGE_LAYOUT_GENERAL}, {fg.linear, attachment_view(b, NULL, b->format), VK_IMAGE_LAYOUT_GENERAL},
        obj ? (VkDescriptorImageInfo){fg.linear, attachment_view(obj, NULL, obj->format), VK_IMAGE_LAYOUT_GENERAL} : (VkDescriptorImageInfo){fg.linear, attachment_view(fin, NULL, fin->format), VK_IMAGE_LAYOUT_GENERAL},
        {fg.linear, depth_view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, attachment_view(&fg.out, NULL, fg.out.format), VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorSet set = descriptor_allocate(fg.set_layout);
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++) w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img[i]};
    vkUpdateDescriptorSets(device, 5, w, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, fg.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, fg.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, fg.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(command, (sw + 7) / 8, (sh + 7) / 8, 1);
    }
    if (scene_prev) fg_hud_composite(&fg.out, fin, scene, sw, sh);
    mtl_sub_commit();
    fg.made++;
    if (fg_dump_state == 1) { LOG("[framegen] dump: final %ux%u format %d (nvn %#x), depth %ux%u format %d, picture %ux%u",
            fin->texture.d.width, fin->texture.d.height, (int)fin->format, t->d.format,
            fg.depth->texture.d.width, fg.depth->texture.d.height, (int)fg.depth->format, sw, sh);
        fg_dump_image(image_get(t), sw, sh, "final"); fg_dump_image(&fg.last, sw, sh, "prev");
        if (scene) fg_dump_image(scene, sw, sh, "scene"); if (scene_prev) fg_dump_image(scene_prev, sw, sh, "scene-prev"); }
    return &fg.out;
}

/* TOTK_FRAMEGEN_DUMP: when <files>/fgdump.trigger exists, the next generated picture and the real picture after it are
   written to <files>/fgdump/<frame>-mid.ppm / -real.ppm (and the real one before as -prev.ppm) for inspection. */
/* fg_dump_state: 0 idle, 1 armed (dump the next generated + real), 2 real still to dump */
/* The w x h corner of an image read back (a full queue wait), 4 bytes a pixel in malloc'd memory the caller frees; NULL
   for a format other than RGBA8 / BGRA8 / R11G11B10. */
static uint8_t *image_read_back(image_entry *e, uint32_t w, uint32_t h) {
    int raw = e->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (!raw && e->format != VK_FORMAT_R8G8B8A8_UNORM && e->format != VK_FORMAT_R8G8B8A8_SRGB && e->format != VK_FORMAT_B8G8R8A8_UNORM) return NULL;
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    vk_buffer b = buffer_create((uint64_t)w * h * 4);
    image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(command, e->image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &c);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    uint64_t ticket = mtl_sub_commit(); mtl_wait(ticket, UINT64_MAX);
    uint8_t *px = malloc((size_t)w * h * 4); if (px) memcpy(px, b.mapped, (size_t)w * h * 4);
    buffer_destroy(&b);
    return px;
}
/* An RGBA8 / BGRA8 image's w x h corner as a binary PPM; 1 when written. */
static int image_save_ppm(image_entry *e, uint32_t w, uint32_t h, const char *path) {
    if (e->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) return 0;
    uint8_t *px = image_read_back(e, w, h); if (!px) return 0;
    FILE *f = fopen(path, "wb"); if (!f) { free(px); return 0; }
    uint8_t *rgb = malloc((size_t)w * h * 3); int bgr = e->format == VK_FORMAT_B8G8R8A8_UNORM;
    if (rgb) for (uint64_t i = 0; i < (uint64_t)w * h; i++) { rgb[i * 3] = px[i * 4 + (bgr ? 2 : 0)]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + (bgr ? 0 : 2)]; }
    int ok = rgb && fprintf(f, "P6\n%u %u\n255\n", w, h) > 0 && fwrite(rgb, 3, (size_t)w * h, f) == (size_t)w * h;
    free(rgb); free(px); ok = !fclose(f) && ok;
    return ok;
}
static void fg_dump_image(image_entry *e, uint32_t w, uint32_t h, const char *tag) {
    /* a scene: written as is (<tag>.r11g11b10) */
    const char *dir = getenv("DM_VK_SHADERS"); char path[1200]; int raw = e->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (!dir) return;
    snprintf(path, sizeof path, "%s/../fgdump", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/../fgdump/%06llu-%s.%s", dir, (unsigned long long)presents, tag, raw ? "r11g11b10" : "ppm");
    if (!raw) { if (image_save_ppm(e, w, h, path)) LOG("[framegen] dump: %s", path); else LOG("[framegen] dump: %s has format %d, not RGBA8", tag, (int)e->format); return; }
    uint8_t *px = image_read_back(e, w, h); FILE *f = px ? fopen(path, "wb") : NULL; if (!f) { free(px); return; }
    fwrite(px, 4, (size_t)w * h, f); fclose(f); free(px); LOG("[framegen] dump: %s (%ux%u)", path, w, h);
}
/* The debug files, every 250 ms as input.now is: each is two access() and an fopen, too much per frame on the
   translation thread. */
static void fg_dump_poll(void) {
    if (fg_dump_state) return;
    static uint64_t next; uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC); if (now < next) return; next = now + 250000000ull;
    const char *dir = getenv("DM_VK_SHADERS"); char path[1200]; if (!dir) return;
    snprintf(path, sizeof path, "%s/../fgdump.trigger", dir);
    if (!access(path, F_OK)) { unlink(path); fg_dump_state = 1; LOG("[framegen] dump armed"); }
    /* <files>/aa.toggle: anti-aliasing on/off, for A/B screenshots of a frozen frame (frz.sh) */
    snprintf(path, sizeof path, "%s/../aa.toggle", dir);
    if (!access(path, F_OK)) { unlink(path); smaa_toggle(); }
    /* live experiments (nvn_vulkan_forward.inc) */
    snprintf(path, sizeof path, "%s/../exp.live", dir);
    { FILE *f = fopen(path, "r"); char now[sizeof live_exp] = "";
      if (f) { if (!fgets(now, sizeof now, f)) now[0] = 0; fclose(f); now[strcspn(now, "\n")] = 0; }
      else { const char *e = getenv("TOTK_VK_LIVE"); if (e) snprintf(now, sizeof now, "%s", e); }
      if (strcmp(now, live_exp)) { snprintf(live_exp, sizeof live_exp, "%s", now); live_log_frame = 0; LOG("[exp] live: '%s'", live_exp); } }
}
/* Keep this frame's finished picture for the next generated one: recorded into the current batch (LSFG records it after
   its HUD composites, which read the previous one: fg.kept_at, so fg_frame_end needs no batch of its own). */
static void fg_keep_record(image_entry *fin, uint32_t sw, uint32_t sh) {
    fg_image(&fg.last, fin->format, fin->texture.d.width, fin->texture.d.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    image_access(fin, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_access(&fg.last, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {sw, sh, 1}};
    vkCmdCopyImage(command, fin->image, VK_IMAGE_LAYOUT_GENERAL, fg.last.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    fg.last_ok = 1; fg.last_w = sw; fg.last_h = sh; fg.kept_at = presents + 1;
}
static void fg_keep_picture(const tex_t *t, const int crop[4]) {
    uint32_t sw = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, sh = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    if (crop && (crop[0] || crop[1])) { fg.last_ok = 0; return; }
    /* LSFG kept it already this frame */
    if (fg.kept_at == presents + 1) return;
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    fg_keep_record(image_get(t), sw, sh);
    mtl_sub_commit();
}
/* TOTK_DYNRES_STATS=1 (diagnostic): the scene size the game's dynamic resolution picks, from the viewport of the main
   scene passes (3+ colour targets and depth, 1024+ wide): every 300 frames, how many frames used each size. */
static struct { int on; float w, h; uint32_t tw, th; unsigned n; struct { uint16_t w, h; unsigned count; } sizes[16]; } dynres = {.on = -1};
static void dynres_note(float w, float h, uint32_t tw, uint32_t th) {
    if (dynres.on < 0) { const char *e = getenv("TOTK_DYNRES_STATS"); dynres.on = e && atoi(e) > 0; }
    /* the share of the depth target the scene drew */
    if (fg.in_gbuffer && tw && th) { fg.view[0] = w / (float)tw; fg.view[1] = h / (float)th; }
    if (dynres.on && tw >= 1024 && w * h > dynres.w * dynres.h) { dynres.w = w; dynres.h = h; dynres.tw = tw; dynres.th = th; }
}
static void dynres_frame(void) {
    if (dynres.on <= 0) return;
    if (dynres.w > 0) {
        unsigned i = 0; while (i < 16 && dynres.sizes[i].count && (dynres.sizes[i].w != (uint16_t)dynres.w || dynres.sizes[i].h != (uint16_t)dynres.h)) i++;
        if (i < 16) { dynres.sizes[i].w = (uint16_t)dynres.w; dynres.sizes[i].h = (uint16_t)dynres.h; dynres.sizes[i].count++; }
    }
    if (++dynres.n % 300 == 0) {
        char b[600]; int k = snprintf(b, sizeof b, "[dynres] last 300 frames, scene viewport on a %ux%u target:", dynres.tw, dynres.th);
        for (unsigned i = 0; i < 16 && dynres.sizes[i].count; i++) k += snprintf(b + k, sizeof b - k, " %ux%u x%u", dynres.sizes[i].w, dynres.sizes[i].h, dynres.sizes[i].count);
        LOG("%s", b); memset(dynres.sizes, 0, sizeof dynres.sizes);
    }
    dynres.w = dynres.h = 0;
}
static void taa_frame_end(void);
static void fg_frame_end(const tex_t *t, const int crop[4]) {
    dynres_frame(); taa_frame_end(); mvs_frame(); mo_frame();
    /* the debug files, once a frame (per draw would be ~7,600 clock reads a frame) */
    fg_dump_poll(); magenta_poll(); shader_override_poll();
    live_frame_base = draw_count;
    if (!fg_track()) return;
    if (fg.trace > 0 && !--fg.trace) LOG("[fg-trace] end");
    /* LSFG: for the HUD composite's translucent UI */
    if (fg.on > 0 || (fg.lsfg > 0 && fg.scene_done && !fg_hud_generated())) fg_keep_picture(t, crop);
    else fg.last_ok = 0;
    if (fg_dump_state == 2) { fg_dump_image(image_get(t), fg.out.texture.d.width, fg.out.texture.d.height, "real"); fg_dump_state = 0; }
    else if (fg_dump_state == 1 && fg.made && fg.out.image) { fg_dump_image(&fg.out, fg.out.texture.d.width, fg.out.texture.d.height, "mid"); fg_dump_state = 2; }
    int known = 0; for (int i = 0; i < fg.nswap; i++) known |= fg.swap[i] == t->serial;
    if (!known) fg.swap[fg.nswap < 4 ? fg.nswap++ : (int)(t->serial % 4)] = t->serial;
    fg.swap_w = t->d.width; fg.swap_h = t->d.height;
    fg_menu_update();
    /* no picture generated from a menu frame */
    memcpy(fg.prev_cam, fg.cam, sizeof fg.cam); fg.prev_ok = fg.cam_ok && !fg_menu_frame();
    if (fg.scene_done) fg.cur ^= 1;
    /* no scene this frame (no UI drawn): the older one is not the previous frame's */
    else fg.scene_valid[fg.cur ^ 1] = 0;
    fg.view[0] = fg.view[1] = 0; fg.scene_done = 0; fg.aa_done = 0; fg.last_c0_img = NULL; fg.cam_ok = 0; fg.depth = NULL; fg.gbuffer = NULL; fg.want_cam = 0; fg.in_gbuffer = 0; fg.gdraws = 0;
    if (fg.frames && !(fg.frames % 300)) LOG("[framegen] %llu game frames: %llu generated, %llu without room on the panel, %llu without inputs, %llu camera cuts, %llu scene snapshots, %llu with the HUD on top; canvas %llu; %llu menus; %llu anti-aliased without UI",
        (unsigned long long)fg.frames, (unsigned long long)fg.made, (unsigned long long)fg.panel_limited, (unsigned long long)fg.no_input, (unsigned long long)fg.cuts, (unsigned long long)fg.snapshots, (unsigned long long)fg.hud_less, (unsigned long long)fg.canvas, (unsigned long long)fg.menus, (unsigned long long)fg.aa_hudless);
}
