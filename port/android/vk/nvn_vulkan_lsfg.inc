/* LSFG frame generation (TOTK_LSFG=1): Lossless Scaling's frame generation through the lsfg-vk framegen library
   (lsfg_bridge.cpp), on this device and queue. Each game frame's picture (before upscaling) is copied into a source
   image; LSFG copies it into an input of its own at the start of its work (copy-in mode), generates multiplier - 1
   pictures between the previous and this one, and they are presented, through the same upscaler as real frames, before
   the real one. Everything is ordered by submission order on the queue: the copy ends with a full barrier (the source
   is written before LSFG reads it, earlier presents have read the outputs before LSFG writes them again), LSFG's work
   follows under the queue lock, and the presents of its pictures come after. Options: the multiplier (2..4, default 2:
   TOTK_FRAMEGEN=lsfg<rate>x<multiplier>, else TOTK_LSFG_MULT), TOTK_LSFG_PERF (performance mode, default 1),
   TOTK_LSFG_FLOW (flow scale 0.25..1, lower is cheaper, default 0.75). The in-game menu changes them live
   (lsfg_configure): LSFG is ended and started again before the next frame's generation (lsfg_bridge_finalize; our
   images and their adoptions are kept, and only missing outputs are made). Shaders: <files>/lsfg/<name>.spv
   (tools/lsfg_extract). */
#include "lsfg_bridge.h"
#define LSFG_MAX_OUT 3
static struct {
    int on, failed, ctx, generated; uint32_t w, h;
    image_entry source, out[LSFG_MAX_OUT];
    /* the images made so far and LSFG's handles for them */
    int source_id, out_id[LSFG_MAX_OUT], nout;
    /* wanted (mult 0: not read yet) */
    int mult, perf; float flow;
    /* in use; restart: the wanted ones differ */
    int active_mult, active_perf; float active_flow; int restart;
    uint64_t frames, shown, hud, limited;
    /* frames were skipped (menu mode): the next generation has no previous picture */
    int gap;
    /* lsfg<cap>xa: the pictures shown a second match the display (lsfg_adaptive_count) */
    int adaptive;
    /* adaptive state and statistics */
    uint64_t ad_last; double ad_due, ad_interval; uint64_t ad_hist[LSFG_MAX_OUT + 2], ad_frames, ad_pictures;
    /* the GPU floor: our batches' GPU time a frame and LSFG's (lsfg_time), smoothed */
    double ad_gpu_prev, ad_gpu_ours, ad_gpu_lsfg, ad_floor_sum; uint64_t ad_floored;
    /* the previous frame's input was a scene (HUD-less), not the finished picture */
    int last_scene;
} lsfg = {.on = -1, .ctx = -1};

static int lsfg_enabled(void) {
    if (lsfg.on < 0) { lsfg.on = lsfg_requested(); if (lsfg.on) LOG("[lsfg] frame generation through Lossless Scaling requested"); }
    return lsfg.on && !lsfg.failed;
}
/* LSFG off for good: the game was capped at a part of the pictures shown for it, so the cap goes back to all of them */
static void lsfg_fail(const char *why) {
    lsfg.failed = 1; LOG("[lsfg] off: %s", why);
    int shown = fg_shown_hz(); if (shown > 0) fg_cap_set(shown);
}

/* A colour image of this device that LSFG uses too, as an image_entry (barrier tracking and views work on it), left in
   GENERAL. Recorded into the current command buffer. */
static int lsfg_image(image_entry *e, uint32_t w, uint32_t h, VkFormat format) {
    memset(e, 0, sizeof *e);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(device, &ci, NULL, &e->image)) return -1;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, e->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < memory_properties.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) && (memory_properties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(device, &mi, NULL, &e->memory.memory) || vkBindImageMemory(device, e->image, e->memory.memory, 0)) return -1;
    e->format = format; e->aspect = VK_IMAGE_ASPECT_COLOR_BIT; e->populated = 1;
    e->texture.d.width = w; e->texture.d.height = h; e->texture.d.depth = 1; e->texture.d.levels = 1; e->texture.d.target = NVN_TEXTURE_TARGET_2D; e->texture.L.layers = 1;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    return 0;
}

static void lsfg_clamp(void) {
    if (lsfg.flow < 0.25f) lsfg.flow = 0.25f; if (lsfg.flow > 1.0f) lsfg.flow = 1.0f;
    if (lsfg.mult < 2) lsfg.mult = 2; if (lsfg.mult > LSFG_MAX_OUT + 1) lsfg.mult = LSFG_MAX_OUT + 1;
}
/* The settings from the environment, once */
static void lsfg_settings(void) {
    if (lsfg.mult) return;
    const char *p = getenv("TOTK_LSFG_PERF"), *f = getenv("TOTK_LSFG_FLOW"), *m = getenv("TOTK_LSFG_MULT"), *g = getenv("TOTK_FRAMEGEN");
    const char *x = g && !strncmp(g, "lsfg", 4) ? strchr(g, 'x') : NULL;
    lsfg.perf = !(p && !atoi(p)); lsfg.flow = f ? (float)atof(f) : 0.75f;
    /* fg_target_hz (adaptive's slots) comes from the shared parse */
    fg_shown_hz();
    if (x && x[1] == 'a') { lsfg.adaptive = 1; lsfg.mult = LSFG_MAX_OUT + 1; } else lsfg.mult = x ? atoi(x + 1) : m ? atoi(m) : 2;
    lsfg_clamp();
}
/* New settings (0 / < 0: unchanged) from the in-game menu; applied before the next generation (lsfg_prepare). */
static void lsfg_configure(int mult, int perf, float flow) {
    lsfg_settings();
    if (mult > 0) lsfg.mult = mult;
    if (perf >= 0) lsfg.perf = perf;
    if (flow > 0) lsfg.flow = flow;
    lsfg_clamp(); lsfg.restart = lsfg.ctx >= 0;
}
static int lsfg_setup(uint32_t w, uint32_t h) {
    if (!lsfg_device_ok) { lsfg_fail("the device was created without what LSFG needs (see [lsfg] above)"); return -1; }
    const char *files = getenv("TOTK_FILES_DIR");
    if (!files) { lsfg_fail("no files directory"); return -1; }
    char dir[600]; snprintf(dir, sizeof dir, "%s/lsfg", files);
    lsfg_settings();
    int perf = lsfg.perf; float flow = lsfg.flow; int mult = lsfg.mult;
    /* the images: the source at the first start, outputs as a multiplier first needs them */
    if (!lsfg.w || lsfg.nout < mult - 1) {
        mtl_sub_begin();
        int bad = !lsfg.w && lsfg_image(&lsfg.source, w, h, VK_FORMAT_R8G8B8A8_UNORM);
        for (int i = lsfg.nout; i < mult - 1 && !bad; i++) bad = lsfg_image(&lsfg.out[i], w, h, VK_FORMAT_R8G8B8A8_UNORM);
        mtl_wait(mtl_sub_commit(), UINT64_MAX);
        if (bad) { lsfg_fail("images"); return -1; }
        if (!lsfg.w) { lsfg.source_id = lsfg_bridge_adopt(lsfg.source.image); lsfg.w = w; lsfg.h = h; }
        for (; lsfg.nout < mult - 1; lsfg.nout++) lsfg.out_id[lsfg.nout] = lsfg_bridge_adopt(lsfg.out[lsfg.nout].image);
    }
    /* LSFG submits (its images are cleared at creation) */
    pthread_mutex_lock(&queue_lock);
    int ctx = -1;
    if (lsfg_bridge_init(vk_api.vkGetInstanceProcAddr, instance, physical, device, queue, queue_family, perf, flow, mult - 1, dir) == 0)
        /* the same image twice: copy-in mode */
        ctx = lsfg_bridge_create(lsfg.source_id, lsfg.source_id, lsfg.out_id, mult - 1, w, h, VK_FORMAT_R8G8B8A8_UNORM);
    pthread_mutex_unlock(&queue_lock);
    if (ctx < 0) { lsfg_fail("the library did not start (see [lsfg] above)"); return -1; }
    lsfg.ctx = ctx; lsfg.generated = mult - 1; lsfg.active_mult = mult; lsfg.active_perf = perf; lsfg.active_flow = flow;
    LOG("[lsfg] on: %ux%u, %s, %s mode, flow scale %.2f", w, h, lsfg.adaptive ? "adaptive (up to x4)" : mult == 2 ? "x2" : mult == 3 ? "x3" : "x4", perf ? "performance" : "quality", flow);
    return 0;
}
/* Settings changed: LSFG ends here and starts again with them (lsfg_setup); the next frame has no previous picture. */
static void lsfg_restart(void) {
    lsfg.restart = 0;
    if (lsfg.ctx < 0 || (lsfg.mult == lsfg.active_mult && lsfg.perf == lsfg.active_perf && lsfg.flow == lsfg.active_flow)) return;
    pthread_mutex_lock(&queue_lock);
    /* our recorded work first; finalize waits for the device */
    dv_drain();
    lsfg_bridge_finalize();
    pthread_mutex_unlock(&queue_lock);
    LOG("[lsfg] restarting with new settings (x%d -> x%d)", lsfg.active_mult, lsfg.mult);
    lsfg.ctx = -1; lsfg.frames = 0; lsfg.ad_last = 0; lsfg.ad_due = 0;
}

/* <files>/lsfgdump.trigger: the previous and this frame's picture, the first generated picture between them and this
   frame's finished picture (HUD included) to <files>/fgdump/<frame>-lsfg-in-prev, -in-new, -gen and -final.ppm (fg_dump_image), e.g. for tools/lsfg_test on the PC */
static int lsfg_dump_armed(void) {
    const char *dir = getenv("DM_VK_SHADERS"); char path[1200];
    if (!dir || !lsfg.frames) return 0;
    /* polled every 250 ms */
    static uint64_t next; uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC); if (now < next) return 0; next = now + 250000000ull;
    snprintf(path, sizeof path, "%s/../lsfgdump.trigger", dir);
    if (access(path, F_OK)) return 0;
    unlink(path); return 1;
}

/* TOTK_FG_HUD_TIME=1 (diagnostic): the GPU time of a frame's HUD composites (timestamps around them, each written once
   the work before it is done), read back four frames later; the mean every 300 frames */
static struct { int on; VkQueryPool pool; uint64_t n, frames; double ms; } hud_time = {.on = -1};
static void hud_time_begin(void) {
    if (hud_time.on < 0) { const char *e = getenv("TOTK_FG_HUD_TIME"); hud_time.on = e && atoi(e) > 0;
        if (hud_time.on) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8}; VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &hud_time.pool)); } }
    if (!hud_time.on) return;
    unsigned q = (unsigned)(hud_time.frames % 4) * 2; uint64_t ts[2];
    if (hud_time.frames >= 4 && vkGetQueryPoolResults(device, hud_time.pool, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
        hud_time.ms += (double)(ts[1] - ts[0]) * gpu_limits.timestampPeriod / 1e6; hud_time.n++; }
    vkCmdResetQueryPool(command, hud_time.pool, q, 2); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hud_time.pool, q);
}
static void hud_time_end(void) {
    if (!hud_time.on) return;
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hud_time.pool, (unsigned)(hud_time.frames % 4) * 2 + 1);
    if (++hud_time.frames % 300 == 0 && hud_time.n) { LOG("[framegen] HUD composites: %.3f ms of GPU time a frame (%llu frames, x%d)", hud_time.ms / hud_time.n, (unsigned long long)hud_time.n, lsfg.generated + 1); hud_time.ms = 0; hud_time.n = 0; }
}

int mtl_display_hz(void);
/* LSFG's GPU time a frame (adaptive): a timestamp at the end of our source copy and one in a command buffer after
   LSFG's submissions, read back four frames later, smoothed. */
static struct { VkQueryPool pool; uint64_t frames; } lsfg_time;
static void lsfg_time_begin(void) {
    if (!lsfg_time.pool) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8}; VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &lsfg_time.pool)); }
    unsigned q = (unsigned)(lsfg_time.frames % 4) * 2; uint64_t ts[2];
    if (lsfg_time.frames >= 4 && vkGetQueryPoolResults(device, lsfg_time.pool, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
        double ms = (double)(ts[1] - ts[0]) * gpu_limits.timestampPeriod / 1e6; if (ms < 50) lsfg.ad_gpu_lsfg = lsfg.ad_gpu_lsfg > 0 ? lsfg.ad_gpu_lsfg * 0.8 + ms * 0.2 : ms; }
    vkCmdResetQueryPool(command, lsfg_time.pool, q, 2); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, lsfg_time.pool, q);
}
static void lsfg_time_end(void) { vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, lsfg_time.pool, (unsigned)(lsfg_time.frames % 4) * 2 + 1); lsfg_time.frames++; }
/* Adaptive (TOTK_FRAMEGEN=lsfg<rate>xa<cap>, the LSFG fill and x1.5 modes): the pictures shown a second match the frame
   rate cap (fg_target_hz), or the display's refresh rate when that is lower, whatever the game's rate. Each game frame
   gets as many pictures as its duration holds slots of that rate (refreshes, or every few at a lower cap), an accumulator
   carrying the fraction (a 90 fps game on a 120 Hz panel alternates 1 and 2), the generated ones at even fractions of
   the interval. The duration leaves out the time the translation thread spent blocked in the previous presents (the
   compositor pacing a burst): counted, our own throttling would feed back into the count and hold the game below
   what it can reach. Returns the pictures for this frame, 1..LSFG_MAX_OUT + 1 (1: the real one alone). */
static int lsfg_adaptive_count(void) {
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC), block = present_block_ns; present_block_ns = 0;
    int hz = mtl_display_hz(); if (hz <= 0) { const char *e = getenv("TOTK_DISPLAY_HZ"); hz = e && atoi(e) > 0 ? atoi(e) : 60; }
    /* measured once presents have timings, else the rate asked for */
    double refresh = pace_refresh ? (double)pace_refresh : 1e9 / hz;
    if (fg_target_hz > 0 && 1e9 / fg_target_hz > refresh) refresh = 1e9 / fg_target_hz;
    int max = lsfg.generated + 1;
    if (!lsfg.ad_last) { lsfg.ad_last = now; return 1; }
    double d = (double)(now - lsfg.ad_last) - (double)block; lsfg.ad_last = now;
    /* the GPU floor: with the GPU saturated (the game's work and LSFG's), a frame cannot be shorter than its GPU
       time whatever the presents' blocking says (that blocking then holds pictures the GPU has not finished, not a
       burst the compositor paces), and an estimate below it under-delivers pictures (in the cave: 98 a second, 22%
       shown twice) */
    { double g = gpu_total_ns; double ours = g - lsfg.ad_gpu_prev; lsfg.ad_gpu_prev = g;
      if (ours > 0 && ours < 1e8) lsfg.ad_gpu_ours = lsfg.ad_gpu_ours > 0 ? lsfg.ad_gpu_ours * 0.8 + ours / 1e6 * 0.2 : ours / 1e6;
      double floor_ns = (lsfg.ad_gpu_ours + lsfg.ad_gpu_lsfg) * 1e6 * 1.02; lsfg.ad_floor_sum += floor_ns;
      if (d < floor_ns) { d = floor_ns; lsfg.ad_floored++; } }
    /* a menu, a load: nothing to catch up */
    if (d < 0) d = 0; if (d > refresh * (max + 1)) d = refresh * (max + 1);
    lsfg.ad_due += d / refresh; int k = (int)lsfg.ad_due; if (k < 1) k = 1; if (k > max) k = max; lsfg.ad_due -= k;
    if (lsfg.ad_due > 1.0) lsfg.ad_due = 1.0; if (lsfg.ad_due < 0) lsfg.ad_due = 0;
    lsfg.ad_hist[k]++; lsfg.ad_frames++; lsfg.ad_interval += d; lsfg.ad_pictures += (uint64_t)k;
    if (lsfg.ad_frames % 600 == 0) { LOG("[lsfg] adaptive: game frame %.2f ms (the presents' blocking left out; GPU floor %.2f = ours %.2f + LSFG %.2f, applied %llu times), %.1f pictures a second; pictures a frame 1/2/3/4: %llu %llu %llu %llu (600 frames)",
            lsfg.ad_interval / 600 / 1e6, lsfg.ad_floor_sum / 600 / 1e6, lsfg.ad_gpu_ours, lsfg.ad_gpu_lsfg, (unsigned long long)lsfg.ad_floored, lsfg.ad_pictures * 1e9 / (lsfg.ad_interval > 0 ? lsfg.ad_interval : 1), (unsigned long long)lsfg.ad_hist[1], (unsigned long long)lsfg.ad_hist[2], (unsigned long long)lsfg.ad_hist[3], (unsigned long long)lsfg.ad_hist[4]);
        memset(lsfg.ad_hist, 0, sizeof lsfg.ad_hist); lsfg.ad_interval = 0; lsfg.ad_pictures = 0; lsfg.ad_floor_sum = 0; lsfg.ad_floored = 0; }
    return k;
}
/* At the game's present: the generated pictures to show before the real one (count; 0 for none). */
static int lsfg_prepare(const tex_t *t, const int crop[4], image_entry **out) {
    if (!lsfg_enabled()) return 0;
    uint32_t sw = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, sh = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    /* LSFG's start (its images, the library, its context: ~300 ms) on the first boot frame that has the screen's
       size, behind the logo: on the first frame of the world it is a hitch */
    if (lsfg.ctx < 0 && !lsfg.w && !(crop && (crop[0] || crop[1])) && sw == screen_extent.width && sh == screen_extent.height) {
        uint64_t t0 = dm_android_clock_ns(CLOCK_MONOTONIC); if (lsfg_setup(sw, sh)) return 0;
        LOG("[lsfg] started at boot in %.0f ms", (dm_android_clock_ns(CLOCK_MONOTONIC) - t0) / 1e6); }
    /* menu mode (nvn_vulkan_framegen.inc): LSFG skips it */
    if (fg.menu || fg_menu_frame()) { lsfg.gap = 1; return 0; }
    /* the panel has room for fewer pictures than the multiplier (pace_refresh_poll): only the real one, without LSFG's work */
    int fit = lsfg.adaptive ? 16 : pace_pictures_fit(nvn_frame_cap_get ? nvn_frame_cap_get() : 0);
    if (fit < 2) { lsfg.gap = 1; lsfg.limited++; return 0; }
    if (crop && (crop[0] || crop[1])) return 0;
    if (lsfg.restart) lsfg_restart();
    /* the picture size changed (menus): no generation; LSFG keeps its context */
    if (lsfg.w && (sw != lsfg.w || sh != lsfg.h)) return 0;
    if (lsfg.ctx < 0 && lsfg_setup(sw, sh)) return 0;
    int dump = lsfg_dump_armed();
    /* pictures generated this frame */
    int n = lsfg.adaptive ? lsfg_adaptive_count() - 1 : lsfg.generated;
    float at[LSFG_MAX_OUT]; for (int i = 0; i < n; i++) at[i] = (float)(i + 1) / (float)(n + 1);
    /* still the previous picture */
    if (dump) fg_dump_image(&lsfg.source, sw, sh, "lsfg-in-prev");
    /* the translation thread's time in here (TOTK_LSFG_CPU=1) */
    static double cpu_copy, cpu_lib, cpu_hud; static uint64_t cpu_n; uint64_t cpu_t0 = dm_android_clock_ns(CLOCK_MONOTONIC);
    /* 1. this picture into the source image LSFG copies in, then everything before is done and visible for LSFG */
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    image_entry *fin = image_get(t);
    /* the scene (the canvas before the UI, nvn_vulkan_framegen.inc) when there is one: LSFG then never sees the HUD,
       which goes on top of its pictures from this frame's picture (fg_hud_composite) */
    image_entry *scene = fg_scene(sw, sh, 0);
    /* linear canvas values, sRGB-encoded as the game's last pass does */
    if (scene) fg_scene_encode(&lsfg.source, scene, sw, sh);
    else {
        image_access(fin, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        image_access(&lsfg.source, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (fin->format == lsfg.source.format) {
            VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {sw, sh, 1}};
            vkCmdCopyImage(command, fin->image, VK_IMAGE_LAYOUT_GENERAL, lsfg.source.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        } else {
            VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)sw, (int)sh, 1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)sw, (int)sh, 1}}};
            vkCmdBlitImage(command, fin->image, VK_IMAGE_LAYOUT_GENERAL, lsfg.source.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
        }
    }
    VkMemoryBarrier all = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0, NULL, 0, NULL);
    if (lsfg.adaptive) lsfg_time_begin();
    mtl_sub_commit();
    uint64_t cpu_t1 = dm_android_clock_ns(CLOCK_MONOTONIC);
    /* 2. LSFG's work for this frame, next on the queue */
    pthread_mutex_lock(&queue_lock);
    /* LSFG submits on its own: our recorded submits first */
    dv_drain();
    uint64_t cpu_t2 = dm_android_clock_ns(CLOCK_MONOTONIC);
    int r = lsfg.adaptive ? lsfg_bridge_present_at(lsfg.ctx, at, n) : lsfg_bridge_present(lsfg.ctx);
    pthread_mutex_unlock(&queue_lock);
    uint64_t cpu_t3 = dm_android_clock_ns(CLOCK_MONOTONIC);
    if (r < 0) { lsfg_fail("generation (see [lsfg] above)"); return 0; }
    /* LSFG read the source and wrote the outputs: the next use of either starts with a full barrier */
    lsfg.source.last_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; lsfg.source.last_access = VK_ACCESS_MEMORY_READ_BIT;
    for (int i = 0; i < n; i++) { lsfg.out[i].last_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; lsfg.out[i].last_access = VK_ACCESS_MEMORY_WRITE_BIT; out[i] = &lsfg.out[i]; }
    /* a scene after a finished picture or the other way round (menus, loading): LSFG's two inputs differ in the HUD and
       its pictures would show it half faded; this frame shows none (LSFG has moved on to this input) */
    int mixed = lsfg.frames && ((scene != NULL) != lsfg.last_scene || lsfg.gap); lsfg.last_scene = scene != NULL; lsfg.gap = 0;
    /* this frame's HUD over the generated pictures, after LSFG's work on the queue; then this picture kept for the
       next */
    if (scene) {
        mtl_sub_begin();
        if (lsfg.adaptive) lsfg_time_end();
        if (lsfg.frames && !mixed) { hud_time_begin(); for (int i = 0; i < n; i++) fg_hud_composite(&lsfg.out[i], fin, scene, sw, sh); hud_time_end(); lsfg.hud++; }
        fg_keep_record(fin, sw, sh);
        mtl_sub_commit();
    } else if (lsfg.adaptive) { mtl_sub_begin(); lsfg_time_end(); mtl_sub_commit(); }
    { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_LSFG_CPU"); on = e && atoi(e) > 0; }
      if (on) { uint64_t t4 = dm_android_clock_ns(CLOCK_MONOTONIC); cpu_copy += (cpu_t1 - cpu_t0) / 1e6; cpu_lib += (cpu_t3 - cpu_t2) / 1e6; cpu_hud += (t4 - cpu_t3) / 1e6;
        static double cpu_drain; cpu_drain += (cpu_t2 - cpu_t1) / 1e6;
        if (++cpu_n % 600 == 0) { LOG("[lsfg] translation-thread CPU a frame: source copy+commit %.2f ms, queue lock+drain %.2f, library present %.2f, HUD composite+keep %.2f", cpu_copy / 600, cpu_drain / 600, cpu_lib / 600, cpu_hud / 600); cpu_copy = cpu_drain = cpu_lib = cpu_hud = 0; } } }
    if (dump) { fg_dump_image(&lsfg.source, sw, sh, "lsfg-in-new"); fg_dump_image(&lsfg.out[0], sw, sh, "lsfg-gen"); fg_dump_image(fin, sw, sh, "lsfg-final"); }
    lsfg.frames++;
    /* the first frame has no previous picture: nothing to show yet */
    if (lsfg.frames == 1 || mixed) return 0;
    /* evenly spaced: the middle one of x4 on a 60 Hz panel at 30 */
    if (fit <= n) { int was = n; n = fit - 1; for (int i = 0; i < n; i++) out[i] = &lsfg.out[(i + 1) * (was + 1) / fit - 1]; }
    lsfg.shown += (uint64_t)n;
    if (!(lsfg.frames % 600)) LOG("[lsfg] %llu game frames, %llu generated pictures shown, %llu frames with the HUD on top, %llu frames without room on the panel", (unsigned long long)lsfg.frames, (unsigned long long)lsfg.shown, (unsigned long long)lsfg.hud, (unsigned long long)lsfg.limited);
    return n;
}
