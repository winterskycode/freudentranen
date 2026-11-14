/* Picture softness, docked (TOTK_SOFTNESS: game, neutral, soft, softer; live word soften=): the game's upscale of its
   FXAA'd scene to the 1920x1080 canvas (a3a570eb, 1.4.3's ac58fc18: an EASU-style filter that sharpens, so edges crawl
   when the camera moves) replaced by one Mitchell-Netravali cubic (shaders/soften.comp) whose (B, C) sets the look.
   Same hook and conditions as asr_upscale_draw (the whole canvas, one target, no blending), but any valid region of the
   scene (dynamic resolution) and a shrink as well (a supersampled TOTK_RENDER_RES). Handheld has no such draw: the
   present's Upscaler setting plays this part there. Needs storage writes without a declared format (the canvas's format
   varies). */
enum { SOFT_GAME, SOFT_NEUTRAL, SOFT_SOFT, SOFT_SOFTER, SOFT_MODES };
static const char *const soften_values[SOFT_MODES] = {"game", "neutral", "soft", "softer"};
/* (B, C) of each look */
static const float soften_bc[SOFT_MODES][2] = {{0, 0}, {0.0f, 0.5f}, {1.0f / 3, 1.0f / 3}, {1.0f, 0.0f}};
static struct { int mode; VkPipeline pipe; image_entry out; uint64_t frames, region; } soften = {.mode = -1};
static int soften_mode_of(const char *v) { for (int m = 0; v && m < SOFT_MODES; m++) if (!strcmp(v, soften_values[m])) return m; return SOFT_GAME; }
static int soften_mode(void) {
    if (soften.mode < 0) soften.mode = soften_mode_of(getenv("TOTK_SOFTNESS"));
    /* the live word soften=<value> (<files>/exp.live) switches as the menu does, for side-by-side checks */
    const char *w = live_exp[0] ? exp_word(live_exp, "soften=", 0) : NULL;
    if (w) { char v[16] = ""; int n = 0; while (n < 15 && exp_word_char(w[n])) { v[n] = w[n]; n++; }
        int m = soften_mode_of(v); if (n && m != soften.mode) { soften.mode = m; LOG("[soften] %s (live)", soften_values[m]); } }
    return soften.mode;
}
static VkImageView soften_view(image_entry *e) {
    return image_view_formatted(e, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, (VkComponentMapping){0}, e->format)->view;
}
/* Returns 1 when the game's upscale draw was replaced (draw_graphics_inner, before its bindings). */
static int soften_upscale_draw(const shader_entry *fs) {
    int mode = soften_mode();
    if (mode == SOFT_GAME || !fs || !fs->module || !storage_write_any_format) return 0;
    if (strncmp(fs->game_id, "a3a570eb159202f1", 16) && strncmp(fs->game_id, "ac58fc18eda5f387", 16)) return 0;
    int why = 0;
    if (state.rt.n != 1 || !state.rt.color[0]) why = 1;
    if (!why && (state.color.blend_en & 1)) why = 2;
    uint32_t handle = 0; int found = 0;
    for (unsigned i = 0; !why && i < fs->count && !found; i++) { const shader_binding *b = &fs->bindings[i];
        if (b->kind == 3 && shader_texture_slot(b, 0) == 0) { handle = shader_handle(b, fs->stage, 0); found = 1; } }
    texture_descriptor d = found ? descriptor_texture(handle & 0xfffff) : (texture_descriptor){0};
    if (!why && !d.valid) why = 3;
    image_entry *src = why ? NULL : image_get(&d.texture), *dst = why ? NULL : image_get(state.rt.color[0]);
    if (!why && (!src || !dst || src->aspect != VK_IMAGE_ASPECT_COLOR_BIT)) why = 3;
    uint32_t dw = dst ? dst->texture.d.width : 0, dh = dst ? dst->texture.d.height : 0;
    if (!why) { float *v = state.viewports.v[0];
        if (!(v[0] == 0 && v[1] == 0 && (v[2] <= 0 || v[2] == (float)dw) && (v[3] <= 0 || v[3] == (float)dh))) why = 4; }
    /* fragment uniform 0: con0.xy = the scene's valid size over the output's, the output size as integers at floats 16/17 */
    float k[20] = {0}; uint32_t ow = 0, oh = 0; double sw = 0, sh = 0;
    if (!why) {
        const p_bind_buf *u = &state.ubo[1][0];
        if (!u->addr || u->size < sizeof k || uniform_read_current((uintptr_t)u->addr, sizeof k)) why = 5;
        else { memcpy(k, uniform_read_scratch, sizeof k); memcpy(&ow, &k[16], 4); memcpy(&oh, &k[17], 4);
            sw = (double)k[0] * ow; sh = (double)k[1] * oh;
            if (ow != dw || oh != dh || sw < 16 || sh < 16 || sw > src->texture.d.width + 0.5 || sh > src->texture.d.height + 0.5) why = 6; }
    }
    static int told[7]; if (!told[why]++)
        LOG("[soften] the game's upscale %.8s: %s (scene %.0fx%.0f of %ux%u -> canvas %ux%u)", fs->game_id, why ? "not replaceable here: the game's own" : "replaced",
            sw, sh, src ? src->texture.d.width : 0, src ? src->texture.d.height : 0, dw, dh);
    if (why) return 0;
    if (!soften.pipe) {
        upscale_ready();
        static const uint32_t code[] = {
#include "soften_spv.inc"
        };
        soften.pipe = upscale_pipeline(code, sizeof code);
    }
    graphics_pass_end();
    image_upload(src);
    /* straight into the canvas when it takes storage writes (RGBA8 on the Odin), else through an RGBA16F image and a copy */
    int direct = (dst->key.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    image_entry *out = direct ? dst : &soften.out;
    if (!direct) fg_image(&soften.out, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    image_access(src, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(out, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    double ratio = sw / dw > sh / dh ? sw / dw : sh / dh;
    float con[8] = {(float)sw, (float)sh, (float)dw, (float)dh, soften_bc[mode][0], soften_bc[mode][1], ratio > 1 ? (float)ratio : 1.0f, mode == SOFT_NEUTRAL ? 1.0f : 0.0f};
    uint32_t words[16] = {0}; memcpy(words, con, sizeof con);
    upscale_pass(soften.pipe, soften_view(src), soften_view(out), words, dw, dh);
    image_gpu_written(out);
    if (!direct) {
        image_access(&soften.out, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        image_access(dst, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}}};
        vkCmdBlitImage(command, soften.out.image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
        image_gpu_written(dst);
    }
    soften.frames++; if (sw < src->texture.d.width - 0.5 || sh < src->texture.d.height - 0.5) soften.region++;
    if (!(soften.frames % 1800)) LOG("[soften] %s: %llu frames, %llu of them a smaller region (dynamic resolution)", soften_values[mode], (unsigned long long)soften.frames, (unsigned long long)soften.region);
    return 1;
}
