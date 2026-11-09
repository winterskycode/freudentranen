/* Object motion (TOTK_OBJ_MOTION=1): last frame's position of what the G-buffer draws, for frame interpolation, after
   totk-pc-recomp's per-object motion (MIT, Copyright (c) 2026 Valiantsin Dzerakh). A qualifying G-buffer draw (indexed,
   depth tested and written, full size, its position reading the scene camera c4 and an object block c5 or c7) is listed
   with the descriptor set of its vertex shader's motion variant (program_motion_shader: last frame's position at location
   15, its banks bound to last frame's blocks, mv_prev_block) and its buffers. When the G-buffer pass ends, in the same
   batch, so the sets and buffers are still the batch's, the list is drawn again into mo.img against the G-buffer depth
   (the draw's own test, or-equal, no write) with shaders/motion_obj.frag: per pixel last frame's normalised position and
   this frame's window position, (-10, -10) where no listed draw covers the pixel. The image is cleared at the first
   flush of a frame. <files>/mvdump.trigger: the next frame's image is written to <files>/mvdump/ (RGBA32F). */
#define MO_DYN 64
typedef struct { shader_entry *vs; pipeline_key key; ds_st depth; poly_st polygon; VkDescriptorSet sets[2]; uint32_t dyn[MO_DYN]; unsigned ndyn;
    VkBuffer vb[16]; VkDeviceSize vo[16]; VkBuffer ib; VkDeviceSize io; VkIndexType it; uint32_t count, instances, base_instance; int32_t base_vertex;
    VkViewport viewport; VkRect2D scissor; } mo_draw;
typedef struct mo_pipeline { pipeline_key key; graphics_pass_entry *pass; VkPipeline pipeline; struct mo_pipeline *next; } mo_pipeline;
static struct {
    mo_draw *d; unsigned n, cap;
    image_entry img; shader_entry fs; uint64_t cleared_at;
    mo_pipeline *pipes[256];
    /* per 600 frames: draws listed, skipped (no variant, not location 15), still (no object block changed), motion draws
       drawn, pipelines made */
    uint64_t listed, skipped, still, drawn, pipes_made, frames;
    int dump;
} mo;
static int mo_on(void) { return mvs_on() && mvs.motion; }
/* Last frame's camera. The scene camera block (c4, 2,368 bytes) holds this frame's view (rows 0-2, 3x4) and
   view-projection (rows 3-6) and, at rows 21-23 and 24-27, last frame's: its copy for last frame is this frame's block
   with those moved up, the projection (rows 7-10) kept. A block whose last frame's view is not close to this frame's
   (a cut, mvs_similar) gets none, and its draws keep this frame's camera. Noted from the submit capture, once a frame
   per block address; the copy goes in the command buffer's uniform arena once per block and command buffer
   (chunk_epoch). */
#define MO_CAMS 32
#define MO_CAM_SIZE 2368
typedef struct { uint64_t addr; int ok; uint64_t epoch; VkDescriptorBufferInfo info; uint8_t bytes[MO_CAM_SIZE]; } mo_cam;
static mo_cam mo_cams[MO_CAMS]; static unsigned mo_ncams;
/* per summary: camera bindings given last frame's camera, and without one */
static uint64_t mo_cam_hits, mo_cam_misses;
static void mo_note_camera(void) {
    const p_bind_buf *u = &state.ubo[0][1];
    if (!u->addr || u->size != MO_CAM_SIZE) return;
    for (unsigned i = 0; i < mo_ncams; i++) if (mo_cams[i].addr == u->addr) return;
    if (mo_ncams == MO_CAMS || uniform_read_current((uintptr_t)u->addr, MO_CAM_SIZE)) return;
    mo_cam *c = &mo_cams[mo_ncams++]; c->addr = u->addr; c->epoch = UINT64_MAX;
    memcpy(c->bytes, uniform_read_scratch, MO_CAM_SIZE);
    int changed; c->ok = mvs_similar(c->bytes + 21 * 16, c->bytes, 3 * 16, &changed);
    if (c->ok) memcpy(c->bytes, c->bytes + 21 * 16, 7 * 16);
}
static int mo_prev_camera_bind(uint64_t addr, VkDescriptorBufferInfo *info) {
    for (unsigned i = 0; i < mo_ncams; i++) if (mo_cams[i].addr == addr) {
        mo_cam *c = &mo_cams[i];
        if (!c->ok) break;
        if (c->epoch != chunk_epoch) { uint8_t *mapped; c->info = uniform_allocate_overwrite(MO_CAM_SIZE, &mapped); memcpy(mapped, c->bytes, MO_CAM_SIZE); c->epoch = chunk_epoch; }
        *info = c->info; mo_cam_hits++; return 1; }
    mo_cam_misses++; return 0;
}
static shader_entry *mo_fs(void) {
    if (!mo.fs.module) {
        static const uint32_t code[] = {
#include "motion_obj_frag_spv.inc"
        };
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
        VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &mo.fs.module));
        VkDescriptorSetLayoutCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &empty, NULL, &mo.fs.layout));
        snprintf(mo.fs.key, sizeof mo.fs.key, "motionobjfs00000");
        mo.fs.stage = 1; mo.fs.set = 1; mo.fs.frag_out_mask = 1; mo.fs.io_mask = 1u << 15; mo.fs.io[15] = (spirv_io){1, 4};
    }
    return &mo.fs;
}
/* After a G-buffer draw: list it when it qualifies (see the top). */
static void mo_record(shader_entry *vs, const pipeline_key *key, ds_st depth, poly_st polygon, const VkBuffer *vb, const VkDeviceSize *vo,
                      VkBuffer ib, VkDeviceSize io, VkIndexType it, uint32_t count, const p_draw_idx *indexed, const VkViewport *viewport, const VkRect2D *scissor) {
    if (!fg.in_gbuffer) return;
    mo_note_camera();
    if (!indexed || !count || !depth.dt || !depth.dw || !fg.depth || (uint32_t)viewport->width != fg.depth->texture.d.width) return;
    unsigned index = 0; while (index < state.program->nstage && program_shader(state.program, index) != vs) index++;
    if (index == state.program->nstage) return;
    shader_entry *m = program_motion_shader(&state, state.program, index);
    if (!m || m->mv_location != 15 || !(m->mv_banks >> 4 & 1) || !(m->mv_banks & (1u << 5 | 1u << 7))) { mo.skipped++; return; }
    /* only what moved itself: an object whose bones (c5) and place (c7) are as last frame's has camera motion, which the
       consumers compute from the depth (wind on foliage, from the clock block c14, is left to that too) */
    { int moved = 0; for (unsigned n = 5; n <= 7 && !moved; n += 2) if (m->mv_banks >> n & 1) { p_bind_buf tmp; mv_prev_block(0, n - 3, &tmp); moved = mv_prev_changed; }
      /* TOTK_OBJ_MOTION=2 (diagnostic): still objects too, to check the camera half against the depth */
      if (!moved && mvs.motion < 2) { mo.still++; return; } }
    if (mo.n == mo.cap) { mo.cap = mo.cap ? mo.cap * 2 : 256; mo.d = realloc(mo.d, mo.cap * sizeof *mo.d); if (!mo.d) die("object motion list"); }
    mo_draw *d = &mo.d[mo.n];
    unsigned ndyn = 0; uint32_t dyn[256];
    d->sets[0] = bind_shader(m, NULL, dyn, &ndyn);
    d->sets[1] = bind_shader(mo_fs(), NULL, dyn, &ndyn);
    if (ndyn > MO_DYN) { mo.skipped++; return; }
    memcpy(d->dyn, dyn, ndyn * sizeof *dyn); d->ndyn = ndyn;
    d->vs = m; d->key = *key; d->depth = depth; d->polygon = polygon;
    memcpy(d->vb, vb, sizeof d->vb); memcpy(d->vo, vo, sizeof d->vo);
    d->ib = ib; d->io = io; d->it = it; d->count = count; d->instances = indexed->instances; d->base_vertex = indexed->base_vertex; d->base_instance = indexed->base_instance;
    d->viewport = *viewport; d->scissor = *scissor;
    mo.n++; mo.listed++;
}
static VkPipeline mo_pipeline_get(const mo_draw *d, graphics_pass_entry *pass, VkPipelineLayout layout) {
    pipeline_key key = d->key;
    key.vs = d->vs; key.fs = mo_fs(); key.colors = 1; key.attachments = 2; key.depth_attachment = 1;
    memset(key.formats, 0, sizeof key.formats); memset(key.color_indices, 0, sizeof key.color_indices); memset(key.blend, 0, sizeof key.blend);
    key.formats[0] = VK_FORMAT_R32G32B32A32_SFLOAT; key.formats[8] = pass->key.formats[1]; key.color_indices[0] = 0;
    key.color.blend_en = 0; key.color.logic_op = 0; key.mask.mask = 0xf; key.multisample = (ms_st){0}; key.raster_discard = 0;
    if (!eds_on) { key.depth = d->depth; key.depth.dw = 0; key.depth.st = 0;
        /* the draw's own test, or equal: the motion variant computes the position the same way */
        if (key.depth.df == VK_COMPARE_OP_LESS + 1) key.depth.df = VK_COMPARE_OP_LESS_OR_EQUAL + 1;
        else if (key.depth.df == VK_COMPARE_OP_GREATER + 1) key.depth.df = VK_COMPARE_OP_GREATER_OR_EQUAL + 1; }
    unsigned bucket = (unsigned)(graphics_hash(&key, sizeof key) ^ (uintptr_t)pass) & 255;
    for (mo_pipeline *p = mo.pipes[bucket]; p; p = p->next) if (p->pass == pass && !memcmp(&p->key, &key, sizeof key)) return p->pipeline;
    mo_pipeline *p = calloc(1, sizeof *p); if (!p) die("object motion pipelines");
    p->key = key; p->pass = pass;
    if (graphics_pipeline_create(&key, d->vs, mo_fs(), pass, layout, &p->pipeline, 0) != VK_SUCCESS) p->pipeline = VK_NULL_HANDLE;
    p->next = mo.pipes[bucket]; mo.pipes[bucket] = p; mo.pipes_made++;
    return p->pipeline;
}
/* mvdump: the image read back after the frame's last flush (16 bytes a pixel) */
static void mo_dump_write(void) {
    uint32_t w = mo.img.texture.d.width, h = mo.img.texture.d.height;
    const char *dir = getenv("DM_VK_SHADERS"); if (!dir || !w || !h) return;
    if (async_recording_owned) mtl_sub_commit();
    mtl_sub_begin();
    vk_buffer b = buffer_create((uint64_t)w * h * 16);
    image_access(&mo.img, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(command, mo.img.image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &c);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    uint64_t ticket = mtl_sub_commit(); mtl_wait(ticket, UINT64_MAX);
    char path[1200]; snprintf(path, sizeof path, "%s/../mvdump", dir); mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/../mvdump/%06llu-%ux%u.f32", dir, (unsigned long long)presents, w, h);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(b.mapped, 16, (size_t)w * h, f); fclose(f); LOG("[motion] dump: %s", path); }
    buffer_destroy(&b);
}
/* At the end of a G-buffer pass (graphics_pass_end): the listed draws into mo.img. */
static void mo_flush(graphics_frame_entry *ended) {
    if (!mo.n) return;
    const graphics_pass_key *k = &ended->key.pass->key;
    if (k->depth == VK_ATTACHMENT_UNUSED || k->depth >= k->attachments) { mo.n = 0; return; }
    image_entry *depth = ended->images[k->depth]; uint32_t w = ended->key.width, h = ended->key.height;
    fg_image(&mo.img, VK_FORMAT_R32G32B32A32_SFLOAT, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    int clear = mo.cleared_at != presents; mo.cleared_at = presents;
    VkAttachmentDescription att[2] = {
        {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.format = k->formats[k->depth], .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_GENERAL}, zref = {1, VK_IMAGE_LAYOUT_GENERAL};
    graphics_pass_entry *pass = graphics_pass_get(graphics_pass_key_make(2, 1, att, &color, zref), att, &color, zref, 1);
    VkImageView views[2] = {attachment_view(&mo.img, NULL, VK_FORMAT_R32G32B32A32_SFLOAT), ended->key.views[k->depth]};
    image_entry *images[2] = {&mo.img, depth};
    graphics_frame_entry *frame = graphics_frame_get(pass, views, images, w, h);
    image_access(&mo.img, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    image_access(depth, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkClearValue clears[2] = {{.color = {.float32 = {-10, -10, 0, 0}}}, {.depthStencil = {0, 0}}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass->pass, .framebuffer = frame->frame, .renderArea = {{0, 0}, {w, h}}, .clearValueCount = 2, .pClearValues = clears};
    vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const float zero[4] = {0};
    for (unsigned i = 0; i < mo.n; i++) {
        const mo_draw *d = &mo.d[i];
        VkPipelineLayout layout = graphics_layout_get(d->vs, mo_fs());
        VkPipeline pipeline = mo_pipeline_get(d, pass, layout); if (!pipeline) continue;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (bl_set) vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bl_set, 0, NULL);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, bl_set ? 1 : 0, 2, d->sets, d->ndyn, d->dyn);
        vkCmdSetViewport(command, 0, 1, &d->viewport); vkCmdSetScissor(command, 0, 1, &d->scissor);
        vkCmdSetBlendConstants(command, zero); vkCmdSetDepthBias(command, 0, 0, 0);
        vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0); vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        if (eds_on) { ds_st z = d->depth; z.dw = 0; z.st = 0;
            if (z.df == VK_COMPARE_OP_LESS + 1) z.df = VK_COMPARE_OP_LESS_OR_EQUAL + 1; else if (z.df == VK_COMPARE_OP_GREATER + 1) z.df = VK_COMPARE_OP_GREATER_OR_EQUAL + 1;
            dv_eds_depth_test(command, z.dt); dv_eds_depth_write(command, 0); dv_eds_depth_compare(command, z.df ? z.df - 1 : VK_COMPARE_OP_ALWAYS); dv_eds_stencil_test(command, 0);
            dv_eds_stencil_op(command, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
            dv_eds_cull(command, d->polygon.cull); dv_eds_front(command, d->polygon.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
            if (eds2_on) dv_eds_bias_enable(command, 0); }
        for (unsigned s = 0; s < 16; s++) if (d->vb[s]) vkCmdBindVertexBuffers(command, s, 1, &d->vb[s], &d->vo[s]);
        vkCmdBindIndexBuffer(command, d->ib, d->io, d->it);
        vkCmdDrawIndexed(command, d->count, d->instances, 0, d->base_vertex, d->base_instance);
        mo.drawn++;
    }
    vkCmdEndRenderPass(command);
    image_gpu_written(&mo.img);
    /* the game's next draw binds everything again */
    memset(&graphics_commands, 0, sizeof graphics_commands);
    mo.n = 0;
}
/* Once a frame (fg_frame_end): the dump, the summary */
static void mo_frame(void) {
    if (!mo_on()) return;
    mo.n = 0;
    if (mo.dump == 2) { mo_dump_write(); mo.dump = 0; }
    mo_ncams = 0;
    if (mo.dump == 1) mo.dump = 2;
    { static uint64_t next; uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC);
      if (now >= next) { next = now + 250000000ull; const char *dir = getenv("DM_VK_SHADERS"); char path[1200];
          if (dir) { snprintf(path, sizeof path, "%s/../mvdump.trigger", dir); if (!access(path, F_OK)) { unlink(path); mo.dump = 1; LOG("[motion] dump armed"); } } } }
    if (++mo.frames % 600) return;
    LOG("[motion] a frame: %.1f draws listed, %.1f skipped (no motion variant at location 15), %.1f still (camera motion only), %.1f drawn; %llu pipelines",
        mo.listed / 600.0, mo.skipped / 600.0, mo.still / 600.0, mo.drawn / 600.0, (unsigned long long)mo.pipes_made);
    LOG("[motion] camera: %.1f bindings a frame given last frame's camera, %.1f without one", mo_cam_hits / 600.0, mo_cam_misses / 600.0);
    mo_cam_hits = mo_cam_misses = 0;
    mo.listed = mo.skipped = mo.still = mo.drawn = 0;
}
