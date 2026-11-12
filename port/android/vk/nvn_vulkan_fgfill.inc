/* Frame generation fill mode (TOTK_FRAMEGEN=fill<cap>[r<rate>], the menu's Homemade fill, x1.5, x3 and x4): the game
   renders up to the cap (or the lower rate), and a slot of the cap that no game frame reaches in time gets a picture
   extrapolated from the last two game frames (framegen_interp.comp past t = 1), at most FILL_RUN in a row (x4: three). Real frames on time are shown untouched, so it
   adds no delay; it costs GPU time only when the game falls behind, and it bridges short stalls.
   The inputs of game frame N are kept at its end (fill_keep): the game overwrites its depth, the object motion and the
   scene snapshots while it renders N+1, which is when a picture is needed. The fill thread (fill_thread, nvn_vulkan.c)
   decides and presents under present_serial, so it never interleaves with a game frame's presents. */
#define FILL_RUN 3
static struct {
    image_entry scene[2], depth, motion;
    /* scene[k] is the last kept frame's, scene[k ^ 1] the one before; n: frames kept in a row */
    int k, n, valid, has_motion;
    float cam[84], prev_cam[84];
    uint32_t w, h;
    /* the planned display times of the last two game frames, the fills since the last game frame */
    uint64_t real_last, real_prev; unsigned run;
    uint64_t made, run_limited, no_input, cuts, frames;
} fill;
static void fill_copy(image_entry *src, image_entry *dst, VkImageAspectFlags aspect, uint32_t w, uint32_t h) {
    fg_image(dst, src->format, src->texture.d.width, src->texture.d.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    image_access(src, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_access(dst, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region = {.srcSubresource = {aspect, 0, 0, 1}, .dstSubresource = {aspect, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(command, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}
/* a camera cut between two frames: more than 8 units or 25 degrees (as fg_prepare) */
static int fill_cut(const float *cam, const float *prev) {
    double dx = cam[47] - prev[47], dy = cam[51] - prev[51], dz = cam[55] - prev[55];
    double turn = cam[8] * prev[8] + cam[9] * prev[9] + cam[10] * prev[10];
    return dx * dx + dy * dy + dz * dz > 64.0 || turn < cos(25.0 * M_PI / 180.0);
}
static void fill_keep(const tex_t *t, const int crop[4], image_entry *mo) {
    fill.frames++;
    uint32_t sw = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, sh = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    image_entry *scene = fg_scene(sw, sh, 0);
    if (!scene || !fg.depth || !fg.cam_ok || !fg.last_ok || fg_menu_frame() || (crop && (crop[0] || crop[1]))) { fill.valid = 0; fill.n = 0; fill.no_input++; return; }
    if (fg.prev_ok && fill_cut(fg.cam, fg.prev_cam)) { fill.n = 0; fill.cuts++; }
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    fill.k ^= 1;
    fill_copy(scene, &fill.scene[fill.k], VK_IMAGE_ASPECT_COLOR_BIT, sw, sh);
    fill_copy(fg.depth, &fill.depth, VK_IMAGE_ASPECT_DEPTH_BIT, fg.depth->texture.d.width, fg.depth->texture.d.height);
    fill.has_motion = mo != NULL;
    if (mo) fill_copy(mo, &fill.motion, VK_IMAGE_ASPECT_COLOR_BIT, mo->texture.d.width, mo->texture.d.height);
    mtl_sub_commit();
    memcpy(fill.prev_cam, fill.cam, sizeof fill.cam); memcpy(fill.cam, fg.cam, sizeof fill.cam);
    fill.w = sw; fill.h = sh;
    fill.n = fg.prev_ok ? fill.n + 1 : 1;
    fill.valid = fill.n >= 2;
}
/* The picture at time tval (1: the last game frame; above: past it) into fg.out, with that frame's HUD; NULL without
   inputs. On any thread: mtl_sub_begin queues behind a recording another thread holds. */
static image_entry *fill_make(double tval) {
    if (!fill.valid) return NULL;
    /* <files>/filldump (diagnostic): this picture's inputs and its output before the HUD, once; TOTK_FILL_T forces t */
    int dump = 0; { const char *d = getenv("TOTK_FILES_DIR"); char path[600];
      if (d && snprintf(path, sizeof path, "%s/filldump", d) > 0 && !access(path, F_OK)) { unlink(path); dump = 1; } }
    { const char *e = getenv("TOTK_FILL_T"); if (e) tval = atof(e); }
    double vc[16], vp[16], ic[16], m[16];
    for (int i = 0; i < 16; i++) { vc[i] = fill.cam[12 + i]; vp[i] = fill.prev_cam[12 + i]; }
    if (!fg_invert4(vc, ic)) return NULL;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += vp[r * 4 + k] * ic[k * 4 + c]; m[r * 4 + c] = s; }
    uint32_t sw = fill.w, sh = fill.h;
    image_entry *a = &fill.scene[fill.k ^ 1], *b = &fill.scene[fill.k];
    mtl_sub_begin(); async_current->gen = 1;
    fg_pipeline();
    fg_image(&fg.out, VK_FORMAT_R8G8B8A8_UNORM, sw, sh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    image_access(a, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(&fill.depth, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(&fg.last, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (fill.has_motion) image_access(&fill.motion, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(&fg.out, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkImageView depth_view = image_view_formatted(&fill.depth, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, (VkComponentMapping){0}, fill.depth.format)->view;
    float push[32]; fg_tune(push, 1);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) push[c * 4 + r] = (float)m[r * 4 + c];
    push[16] = (float)sw; push[17] = (float)sh; push[18] = 1.0f / sw; push[19] = 1.0f / sh;
    push[20] = 1.0f; push[22] = 2.0f * fg_dilation(); push[23] = 1.0f;
    push[24] = (float)sw / b->texture.d.width; push[25] = (float)sh / b->texture.d.height; push[26] = fill.has_motion ? 1.0f : 0.0f; push[27] = (float)tval;
    fg_dispatch(a, b, fill.has_motion ? &fill.motion : &fg.last, depth_view, push, sw, sh);
    /* this frame's HUD alone: a fill picture shows the last game frame's HUD where it stands */
    if (!dump) fg_hud_pass(&fg.out, &fg.last, b, NULL, NULL, sw, sh, 0);
    mtl_sub_commit();
    fill.made++;
    if (dump) {
          LOG("[framegen] fill dump: t=%.3f k=%d n=%d motion=%d scene %ux%u fmt %d, last %ux%u fmt %d, depth %ux%u fmt %d", tval, fill.k, fill.n, fill.has_motion,
              b->texture.d.width, b->texture.d.height, (int)b->format, fg.last.texture.d.width, fg.last.texture.d.height, (int)fg.last.format,
              fill.depth.texture.d.width, fill.depth.texture.d.height, (int)fill.depth.format);
          fg_dump_image(b, sw, sh, "fill-scene-cur"); fg_dump_image(a, sw, sh, "fill-scene-prev"); fg_dump_image(&fg.last, sw, sh, "fill-last"); fg_dump_image(&fg.out, sw, sh, "fill-out");
          LOG("[framegen] fill dump push: scale %.3f %.3f %.0f %.3f size %.0f %.0f opt %.2f %.3f %.0f %.0f", push[24], push[25], push[26], push[27], push[16], push[17], push[20], push[21], push[22], push[23]); }
    return &fg.out;
}
