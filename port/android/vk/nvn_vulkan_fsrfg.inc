/* Frame generation with AMD FSR 3.1 (TOTK_FRAMEGEN=fsr<rate>, the "FSR 3.1 x2" setting): frame interpolation and optical
   flow from the FidelityFX SDK v1.1.4 (third_party/fsr3, MIT) in place of the built-in framegen_interp.comp. The SDK's host
   library takes its shaders from blobs a Windows compiler makes, so the passes are run from here: tools/gen_fsrfg.py
   compiles them and reads each pass's bindings, and each binding's resource is found by name. Order and sizes follow
   ffx_opticalflow.cpp dispatch() and ffx_frameinterpolation.cpp Prepare / Dispatch.
   Inputs: fsrfg_prep.comp gives FSR its colour (the scene encoded for display, RGBA8), camera motion and a depth copy, in
   images of our own, so every other descriptor set is written once per frame parity. No HUD input: the HUD goes on top
   afterwards as with the built-in pass (fg_hud_generated). Constants live in a ring of mapped slots (dynamic uniform buffers).
   Motion work at half size: on Adreno the image atomics of the motion passes cost 2.5 ms at 720p, and optical flow's search
   1.4; FSR's render size below its display size (LOW_RES_MOTION_VECTORS) and half-size flow bring a picture from 7.3 to
   3.1 ms with no visible loss (Odin). TOTK_FSRFG_MOTION=1 and TOTK_FSRFG_FLOW=1 run them at full size;
   TOTK_FSRFG_TIMER=1 logs GPU time a picture, =2 by pass; TOTK_FSRFG_NONE=1 acts as a device without what it needs. */
/* kind: 0 sampled, 1 storage, 2 uniform, 3 storage buffer, 4 sampler */
typedef struct { uint16_t binding; uint8_t kind; const char *name; } fsr_binding;
typedef struct { const char *name; const uint32_t *code; size_t size; const fsr_binding *bindings; uint32_t count; } fsr_pass_source;
#include "fsrfg_passes_spv.inc"
enum { FSR_SLOT = 4096, FSR_SLOTS = 8, FSR_CB_FI = 0, FSR_CB_PYR = 256, FSR_CB_OFSPD = 512, FSR_CB_OF = 768, FSR_OF_COMMON = 7, FSR_LEVELS = 7, FSR_CB_PYR_RENDER = 2816, FSR_MAX_STEPS = 64, FSR_STAMPS = 6, FSR_TIMER_RING = 4 };
/* kind: 0 dispatch, 1 clear image to value, 2 time stamp x */
typedef struct { int kind, pass; VkDescriptorSet set; uint32_t x, y, z, ubos; int skip_on_reset; image_entry *image; uint32_t value; } fsr_step;
typedef struct { image_entry *in_cur, *in_prev, *flow_rw, *flow_prev, *flow_r, *next; int par, level; } fsr_ctx;
typedef struct { int32_t render[2], display[2]; float display_rcp[2], cam_near, cam_far; int32_t upscaler[2], mode, reset; float depth_to_view[4]; float delta; int32_t hudless, distortion[2];
    float of_scale[2]; int32_t of_block; uint32_t flags; int32_t max_render[2], of_half, instances; int32_t rect_base[2], rect_size[2]; float debug_bar[3]; uint32_t transfer; float luminance[2], tan_half_fov; int32_t pad;
    float jitter[2], mv_scale[2]; } fsr_fi_constants;
static struct {
    /* w, h: the picture; rw, rh: the motion passes (TOTK_FG_FSR_SCALE=2: FSR's render size below its display size) */
    int timer, failed, ready; uint32_t w, h, rw, rh, scale, ow, oh, of_scale, levels;
    uint64_t runs, last_frames, frame_index;
    VkDescriptorSetLayout set_layout[FSR_PASSES], prep_set_layout; VkPipelineLayout layout[FSR_PASSES], prep_layout; VkPipeline pipeline[FSR_PASSES], prep; VkDescriptorPool pool;
    image_entry src[2], src_half[2], mv, depth, dmv, dd, rdp, rdi, gx, gy, ox, oy, dm, pyr, dist, in[2][FSR_LEVELS], flow[2][FSR_LEVELS], ofv, hist, hist_prev, scd_temp, scd_out, *out;
    vk_buffer counters, cb; fsr_step steps[2][FSR_MAX_STEPS]; int nsteps[2];
    VkQueryPool stamps, fine; double ms[FSR_STAMPS], total, fine_ms[FSR_PASSES]; uint32_t fine_n[FSR_PASSES]; uint64_t timed; float cam_near, cam_far, tan_half; int cam_logged;
} fsr = {.timer = -1};

/* Asked for and able to run; otherwise frame generation stays on with the built-in pass. */
static int fsr_fg_enabled(void) {
    if (fg_fsr && !fsr_device_ok && !fsr.failed) { fsr.failed = 1; LOG("[fsrfg] not available on this device: the built-in pass instead"); }
    return fg_fsr && !fsr.failed;
}
static void fsr_barrier(void) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
static VkImageView fsr_view(image_entry *e, int mip) {
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, mip < 0 ? 0 : (uint32_t)mip, mip < 0 ? e->texture.d.levels : 1, 0, 1};
    return image_view_formatted(e, VK_IMAGE_VIEW_TYPE_2D, r, (VkComponentMapping){0}, e->format)->view;
}
/* The resource of a pass's binding, by the name the shaders give it */
static image_entry *fsr_resource(const char *full, const fsr_ctx *c, int pass, int *mip) {
    int rw = !strncmp(full, "rw_", 3); const char *n = full + (rw ? 3 : 2); int p = c->par; *mip = -1;
    if (!strncmp(n, "inpainting_pyramid", 18)) { if (n[18]) { *mip = atoi(n + 18); if (*mip >= (int)fsr.levels) *mip = (int)fsr.levels - 1; } return &fsr.pyr; }
    if (!strncmp(n, "optical_flow_input_level_", 25)) return &fsr.in[p][atoi(n + 25)];
    static const struct { const char *name; size_t at; } fixed[] = {
        {"input_motion_vectors", offsetof(__typeof__(fsr), mv)}, {"input_depth", offsetof(__typeof__(fsr), depth)}, {"reconstructed_depth_previous_frame", offsetof(__typeof__(fsr), rdp)},
        {"dilated_motion_vectors", offsetof(__typeof__(fsr), dmv)}, {"dilated_depth", offsetof(__typeof__(fsr), dd)}, {"reconstructed_depth_interpolated_frame", offsetof(__typeof__(fsr), rdi)},
        {"game_motion_vector_field_x", offsetof(__typeof__(fsr), gx)}, {"game_motion_vector_field_y", offsetof(__typeof__(fsr), gy)}, {"optical_flow_motion_vector_field_x", offsetof(__typeof__(fsr), ox)},
        {"optical_flow_motion_vector_field_y", offsetof(__typeof__(fsr), oy)}, {"disocclusion_mask", offsetof(__typeof__(fsr), dm)}, {"input_distortion_field", offsetof(__typeof__(fsr), dist)},
        {"optical_flow_scd", offsetof(__typeof__(fsr), scd_out)}, {"optical_flow_scd_output", offsetof(__typeof__(fsr), scd_out)}, {"optical_flow_confidence", offsetof(__typeof__(fsr), scd_out)},
        {"optical_flow_scd_histogram", offsetof(__typeof__(fsr), hist)}, {"optical_flow_scd_previous_histogram", offsetof(__typeof__(fsr), hist_prev)}, {"optical_flow_scd_temp", offsetof(__typeof__(fsr), scd_temp)}};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++) if (!strcmp(n, fixed[i].name)) return (image_entry *)((char *)&fsr + fixed[i].at);
    if (!strcmp(n, "previous_interpolation_source")) return &fsr.src[p ^ 1];
    if (!strcmp(n, "current_interpolation_source") || !strcmp(n, "present_backbuffer")) return &fsr.src[p];
    if (!strcmp(n, "input_color")) return fsr.of_scale > 1 ? &fsr.src_half[p] : &fsr.src[p];
    if (!strcmp(n, "output")) return fsr.out;
    if (!strcmp(n, "optical_flow_input")) return c->in_cur;
    if (!strcmp(n, "optical_flow_previous_input")) return c->in_prev;
    if (!strcmp(n, "optical_flow")) return pass == FSR_FI_OF_FIELD ? &fsr.ofv : rw ? c->flow_rw : c->flow_r;
    if (!strcmp(n, "optical_flow_previous")) return c->flow_prev;
    if (!strcmp(n, "optical_flow_next_level")) return c->next;
    die("fsrfg: no resource for %s", full);
}
static void fsr_add(int par, int pass, const fsr_ctx *c, uint32_t x, uint32_t y, uint32_t z, int skip_on_reset) {
    const fsr_pass_source *s = &fsr_passes[pass];
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = fsr.pool, .descriptorSetCount = 1, .pSetLayouts = &fsr.set_layout[pass]};
    VkDescriptorSet set; VK_CHECK(vkAllocateDescriptorSets(device, &ai, &set));
    VkWriteDescriptorSet w[24]; VkDescriptorImageInfo img[24]; VkDescriptorBufferInfo buf[24]; uint32_t ubos = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        const fsr_binding *b = &s->bindings[i]; memset(&img[i], 0, sizeof img[i]); memset(&buf[i], 0, sizeof buf[i]);
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding, .descriptorCount = 1};
        if (b->kind <= 1) { int mip; image_entry *e = fsr_resource(b->name, c, pass, &mip); if (!e || !e->image) die("fsrfg: %s of %s not set", b->name, s->name);
            w[i].descriptorType = b->kind ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            img[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, fsr_view(e, b->kind && mip < 0 ? 0 : mip), VK_IMAGE_LAYOUT_GENERAL}; w[i].pImageInfo = &img[i]; }
        else if (b->kind == 2) { uint64_t at = !strcmp(b->name, "cbFI") ? FSR_CB_FI : !strcmp(b->name, "cbInpaintingPyramid") ? (pass == FSR_FI_GAME_PYR ? FSR_CB_PYR_RENDER : FSR_CB_PYR) : !strcmp(b->name, "cbOF_SPD") ? FSR_CB_OFSPD : FSR_CB_OF + 256u * (uint32_t)c->level;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; buf[i] = (VkDescriptorBufferInfo){fsr.cb.buffer, at, 256}; w[i].pBufferInfo = &buf[i]; ubos++; }
        else if (b->kind == 3) { w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; buf[i] = (VkDescriptorBufferInfo){fsr.counters.buffer, 0, 64}; w[i].pBufferInfo = &buf[i]; }
        else { w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; img[i].sampler = fg.linear; w[i].pImageInfo = &img[i]; }
    }
    vkUpdateDescriptorSets(device, s->count, w, 0, NULL);
    if (fsr.nsteps[par] >= FSR_MAX_STEPS) die("fsrfg: too many steps");
    fsr.steps[par][fsr.nsteps[par]++] = (fsr_step){.pass = pass, .set = set, .x = x, .y = y, .z = z, .ubos = ubos, .skip_on_reset = skip_on_reset};
}
static void fsr_add_clear(int par, image_entry *e, uint32_t value, int skip_on_reset) { fsr.steps[par][fsr.nsteps[par]++] = (fsr_step){.kind = 1, .image = e, .value = value, .skip_on_reset = skip_on_reset}; }
static void fsr_add_stamp(int par, uint32_t at) { fsr.steps[par][fsr.nsteps[par]++] = (fsr_step){.kind = 2, .x = at}; }
/* ffxSpdSetup: groups x, y, offsets, count, mips */
static void fsr_spd(uint32_t w, uint32_t h, int mips, uint32_t out[6]) {
    out[0] = (w - 1) / 64 + 1; out[1] = (h - 1) / 64 + 1; out[2] = out[3] = 0; out[4] = out[0] * out[1];
    if (mips >= 0) out[5] = (uint32_t)mips; else { uint32_t r = w > h ? w : h, m = 0; while (r > 1) { r >>= 1; m++; } out[5] = m > 12 ? 12 : m; }
}
static void fsr_build_steps(int p) {
    uint32_t w = fsr.w, h = fsr.h, rx = (w + 7) / 8, ry = (h + 7) / 8, mx = (fsr.rw + 7) / 8, my = (fsr.rh + 7) / 8, spd[6]; fsr_ctx c = {.par = p, .level = FSR_OF_COMMON, .in_cur = &fsr.in[p][0], .in_prev = &fsr.in[p ^ 1][0]};
    fsr.nsteps[p] = 0;
    uint32_t ow = fsr.ow, oh = fsr.oh, ox = (ow + 7) / 8, oy = (oh + 7) / 8;
    fsr_add_stamp(p, 1);
    fsr_add(p, FSR_OF_LUMA, &c, ((ow + 1) / 2 + 15) / 16, ((oh + 1) / 2 + 15) / 16, 1, 0);
    fsr_spd(ow, oh, 4, spd); fsr_add(p, FSR_OF_PYR, &c, spd[0], spd[1], 1, 0);
    fsr_add(p, FSR_OF_HIST, &c, ((ow / 4) / 3 + 31) / 32, 16, 9, 0);
    fsr_add(p, FSR_OF_DIV, &c, 9, 3, 1, 0);
    uint32_t tw[FSR_LEVELS], th[FSR_LEVELS]; tw[0] = ox; th[0] = oy;
    for (int i = 1; i < FSR_LEVELS; i++) { tw[i] = (tw[i - 1] + 1) / 2; th[i] = (th[i - 1] + 1) / 2; }
    for (int level = FSR_LEVELS - 1; level >= 0; level--) {
        int a = p != (level & 1) ? 1 : 0; image_entry *A = fsr.flow[a], *B = fsr.flow[a ^ 1];
        uint32_t lw = ow >> level ? ow >> level : 1, lh = oh >> level ? oh >> level : 1;
        c.level = level; c.in_cur = &fsr.in[p][level]; c.in_prev = &fsr.in[p ^ 1][level];
        c.flow_rw = &A[level];
        fsr_add(p, FSR_OF_SEARCH, &c, ((lw + 3) / 4 * 16 + 63) / 64, (lh + 15) / 16, 1, 0);
        c.flow_prev = &A[level]; c.flow_rw = level ? &B[level] : &fsr.ofv;
        fsr_add(p, FSR_OF_FILTER, &c, (tw[level] + 15) / 16, (th[level] + 3) / 4, 1, 0);
        if (level) { c.flow_r = &B[level]; c.next = &B[level - 1]; fsr_add(p, FSR_OF_SCALE, &c, (tw[level - 1] + 3) / 4, (th[level - 1] + 3) / 4, 1, 0); }
    }
    fsr_add_stamp(p, 2);
    c.level = FSR_OF_COMMON;
    /* 1.0f: the depths are kept as float bits, nearest wins */
    fsr_add_clear(p, &fsr.rdp, 0x3f800000u, 0);
    fsr_add(p, FSR_FI_DILATE, &c, mx, my, 1, 0);
    fsr_add(p, FSR_FI_SETUP, &c, mx, my, 1, 0);
    fsr_add_clear(p, &fsr.rdi, 0x3f800000u, 1);
    fsr_add(p, FSR_FI_PREV_DEPTH, &c, mx, my, 1, 1);
    fsr_add(p, FSR_FI_GAME_MV, &c, mx, my, 1, 1);
    fsr_spd(fsr.rw, fsr.rh, -1, spd); fsr_add(p, FSR_FI_GAME_PYR, &c, spd[0], spd[1], 1, 1);
    fsr_add(p, FSR_FI_OF_FIELD, &c, (ow / 8 + 7) / 8, (oh / 8 + 7) / 8, 1, 1);
    fsr_add(p, FSR_FI_DISOCC, &c, mx, my, 1, 1);
    fsr_add_stamp(p, 3);
    fsr_add(p, FSR_FI_INTERP, &c, rx, ry, 1, 0);
    fsr_add_stamp(p, 4);
    fsr_spd(w, h, -1, spd); fsr_add(p, FSR_FI_PYR, &c, spd[0], spd[1], 1, 0);
    fsr_add(p, FSR_FI_INPAINT, &c, rx, ry, 1, 0);
    fsr_add_stamp(p, 5);
}
static void fsr_pipelines(void) {
    if (fsr.prep || fsr.failed) return;
    uint64_t started = dm_android_clock_ns(CLOCK_MONOTONIC);
    { const char *e = getenv("TOTK_FSRFG_TIMER"); fsr.timer = e ? atoi(e) : 0; }
    for (int i = 0; i < FSR_PASSES; i++) {
        const fsr_pass_source *s = &fsr_passes[i]; VkDescriptorSetLayoutBinding b[24];
        for (uint32_t k = 0; k < s->count; k++) { static const VkDescriptorType t[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
            b[k] = (VkDescriptorSetLayoutBinding){.binding = s->bindings[k].binding, .descriptorType = t[s->bindings[k].kind], .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}; }
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = s->count, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &fsr.set_layout[i]));
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &fsr.set_layout[i]};
        VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &fsr.layout[i]));
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = s->size, .pCode = s->code};
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = fsr.layout[i]};
        VkResult made = pipeline_create_compute(&pi, &fsr.pipeline[i]); vkDestroyShaderModule(device, module, NULL);
        if (made != VK_SUCCESS) { fsr.failed = 1; LOG("[fsrfg] pipeline %s failed (%d): the built-in pass instead", s->name, (int)made); return; }
    }
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t k = 0; k < 6; k++) b[k] = (VkDescriptorSetLayoutBinding){.binding = k, .descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &fsr.prep_set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 112};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &fsr.prep_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &fsr.prep_layout));
    static const uint32_t code[] = {
#include "fsrfg_prep_spv.inc"
    };
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = fsr.prep_layout};
    VkResult made = pipeline_create_compute(&pi, &fsr.prep); vkDestroyShaderModule(device, module, NULL);
    if (made != VK_SUCCESS) { fsr.failed = 1; LOG("[fsrfg] pipeline prep failed (%d): the built-in pass instead", (int)made); return; }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32}, {VK_DESCRIPTOR_TYPE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * FSR_MAX_STEPS, .poolSizeCount = 5, .pPoolSizes = sizes};
    VK_CHECK(vkCreateDescriptorPool(device, &dp, NULL, &fsr.pool));
    fsr.cb = buffer_create(FSR_SLOT * FSR_SLOTS); fsr.counters = buffer_create(4096); memset(fsr.counters.mapped, 0, 64);
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = FSR_STAMPS * FSR_TIMER_RING};
    if (fsr.timer > 0) VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &fsr.stamps));
    if (fsr.timer > 1) { qi.queryCount = (FSR_MAX_STEPS + 1) * FSR_TIMER_RING; VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &fsr.fine)); }
    LOG("[fsrfg] %d pipelines in %.0f ms", FSR_PASSES + 1, (dm_android_clock_ns(CLOCK_MONOTONIC) - started) / 1e6);
}
static void fsr_setup(uint32_t w, uint32_t h, image_entry *out) {
    if (fsr.ready && fsr.w == w && fsr.h == h && fsr.out == out) return;
    if (fsr.ready) { vkDeviceWaitIdle(device); VK_CHECK(vkResetDescriptorPool(device, fsr.pool, 0)); }
    fsr.w = w; fsr.h = h; fsr.out = out;
    uint64_t started = dm_android_clock_ns(CLOCK_MONOTONIC);
    { const char *e = getenv("TOTK_FSRFG_MOTION"); fsr.scale = e && atoi(e) == 1 ? 1 : 2; } fsr.rw = w / fsr.scale; fsr.rh = h / fsr.scale; uint32_t rw = fsr.rw, rh = fsr.rh;
    { const char *e = getenv("TOTK_FSRFG_FLOW"); fsr.of_scale = e && atoi(e) == 1 ? 1 : 2; } fsr.ow = w / fsr.of_scale; fsr.oh = h / fsr.of_scale; uint32_t ow = fsr.ow, oh = fsr.oh;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int p = 0; p < 2; p++) {
        fg_image(&fsr.src[p], VK_FORMAT_R8G8B8A8_UNORM, w, h, u); fg_image(&fsr.src_half[p], VK_FORMAT_R8G8B8A8_UNORM, (w + 1) / 2, (h + 1) / 2, u);
        uint32_t tw = (ow + 7) / 8, th = (oh + 7) / 8;
        for (int l = 0; l < FSR_LEVELS; l++) {
            fg_image(&fsr.in[p][l], VK_FORMAT_R8_UINT, ow >> l ? ow >> l : 1, oh >> l ? oh >> l : 1, u);
            fg_image(&fsr.flow[p][l], VK_FORMAT_R16G16_SINT, tw, th, u); tw = (tw + 1) / 2; th = (th + 1) / 2;
        }
    }
    fg_image(&fsr.mv, VK_FORMAT_R16G16_SFLOAT, rw, rh, u); fg_image(&fsr.depth, VK_FORMAT_R32_SFLOAT, rw, rh, u);
    fg_image(&fsr.dmv, VK_FORMAT_R16G16_SFLOAT, rw, rh, u); fg_image(&fsr.dd, VK_FORMAT_R32_SFLOAT, rw, rh, u);
    fg_image(&fsr.rdp, VK_FORMAT_R32_UINT, rw, rh, u); fg_image(&fsr.rdi, VK_FORMAT_R32_UINT, rw, rh, u);
    fg_image(&fsr.gx, VK_FORMAT_R32_UINT, rw, rh, u); fg_image(&fsr.gy, VK_FORMAT_R32_UINT, rw, rh, u);
    fg_image(&fsr.ox, VK_FORMAT_R32_UINT, rw, rh, u); fg_image(&fsr.oy, VK_FORMAT_R32_UINT, rw, rh, u);
    fg_image(&fsr.dm, VK_FORMAT_R8G8_UNORM, rw, rh, u); fg_image(&fsr.dist, VK_FORMAT_R8G8_UNORM, 1, 1, u);
    fg_image(&fsr.ofv, VK_FORMAT_R16G16_SINT, (ow + 7) / 8, (oh + 7) / 8, u);
    fsr.levels = 0; for (uint32_t r = (w / 2 > h / 2 ? w / 2 : h / 2); r; r >>= 1) fsr.levels++;
    fg_image_levels(&fsr.pyr, VK_FORMAT_R16G16B16A16_SFLOAT, w / 2, h / 2, fsr.levels, u);
    fg_image(&fsr.hist, VK_FORMAT_R32_UINT, 256 * 9, 1, u); fg_image(&fsr.hist_prev, VK_FORMAT_R32_SFLOAT, 256 * 9, 1, u);
    fg_image(&fsr.scd_temp, VK_FORMAT_R32_UINT, 3, 1, u); fg_image(&fsr.scd_out, VK_FORMAT_R32_UINT, 3, 1, u);
    fsr_build_steps(0); fsr_build_steps(1);
    fsr.ready = 1; fsr.last_frames = 0;
    LOG("[fsrfg] %ux%u: motion %ux%u, flow %ux%u, %d steps, set up in %.1f ms", w, h, rw, rh, ow, oh, fsr.nsteps[0], (dm_android_clock_ns(CLOCK_MONOTONIC) - started) / 1e6);
}
/* near, far and the field of view from the view-projection (rows at cam[12..27]): a rigid view times a GL projection */
static void fsr_camera(const float *cam) {
    const float *r0 = cam + 12, *r1 = cam + 16, *r2 = cam + 20, *r3 = cam + 24;
    double p0 = sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]), p1 = sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    double a = -(r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]), b = r2[3] + a * r3[3], n = b / (a - 1), f = b / (a + 1);
    if (!(n > 0) || !(f > n)) { n = 0.1; f = 10000; }
    fsr.cam_near = (float)n; fsr.cam_far = (float)f; fsr.tan_half = p0 > 0 ? (float)(1 / p0) : 1;
    if (!fsr.cam_logged++) LOG("[fsrfg] camera: near %.3f far %.1f, cot half fov x %.3f y %.3f", n, f, p0, p1);
}
static void fsr_constants(void *slot, int reset) {
    double p1 = 1.0 / (fsr.tan_half * fsr.h / fsr.w); float n = fsr.cam_near, f = fsr.cam_far, q = f / (n - f); int32_t w = (int32_t)fsr.w, h = (int32_t)fsr.h, rw = (int32_t)fsr.rw, rh = (int32_t)fsr.rh;
    fsr_fi_constants c = {.render = {rw, rh}, .display = {w, h}, .display_rcp = {1.0f / w, 1.0f / h}, .cam_near = n, .cam_far = f, .upscaler = {w, h}, .reset = reset,
        .depth_to_view = {-q, q * n, fsr.tan_half, (float)(1 / p1)}, .delta = 16.6f, .distortion = {1, 1}, .of_scale = {1.0f / (float)fsr.ow, 1.0f / (float)fsr.oh}, .of_block = 8, .max_render = {rw, rh}, .instances = 1,
        .rect_size = {w, h}, .luminance = {0, 1}, .tan_half_fov = fsr.tan_half, .mv_scale = {1, 1}};
    memcpy((char *)slot + FSR_CB_FI, &c, sizeof c);
    uint32_t spd[6]; fsr_spd(fsr.w, fsr.h, -1, spd);
    uint32_t pyr[4] = {spd[5], spd[4], 0, 0}; memcpy((char *)slot + FSR_CB_PYR, pyr, sizeof pyr);
    fsr_spd(fsr.rw, fsr.rh, -1, spd); pyr[0] = spd[5]; pyr[1] = spd[4]; memcpy((char *)slot + FSR_CB_PYR_RENDER, pyr, sizeof pyr);
    fsr_spd(fsr.ow, fsr.oh, 4, spd);
    uint32_t ofspd[8] = {spd[5], spd[4], 0, 0, spd[4], 0, 0, 0}; memcpy((char *)slot + FSR_CB_OFSPD, ofspd, sizeof ofspd);
    for (int l = 0; l <= FSR_OF_COMMON; l++) {
        struct { int32_t res[2]; uint32_t level, count, frame, transfer; float lum[2]; } o = {{(int32_t)fsr.ow, (int32_t)fsr.oh}, (uint32_t)(l == FSR_OF_COMMON ? 0 : l), FSR_LEVELS, (uint32_t)fsr.frame_index, 0, {0, 1}};
        memcpy((char *)slot + FSR_CB_OF + 256 * l, &o, sizeof o);
    }
}
/* The generated picture into out, in place of the built-in pass. push: the built-in pass's constants (fsrfg_prep.comp). */
static void fsr_fg_run(image_entry *pic, VkImageView depth_view, const float *push, uint32_t sw, uint32_t sh, image_entry *out, const float *cam, uint64_t frames) {
    fsr_setup(sw, sh, out); fsr_camera(cam);
    int p = (int)(fsr.runs & 1), reset = !fsr.runs || fsr.last_frames + 1 != frames; fsr.last_frames = frames;
    fsr.frame_index = reset ? 0 : fsr.frame_index + 1;
    uint32_t slot = (uint32_t)(fsr.runs % FSR_SLOTS) * FSR_SLOT;
    fsr_constants((char *)fsr.cb.mapped + slot, reset);
    unsigned q = (unsigned)(fsr.runs % FSR_TIMER_RING) * FSR_STAMPS; uint64_t ts[FSR_STAMPS];
    if (fsr.stamps && fsr.runs >= FSR_TIMER_RING && vkGetQueryPoolResults(device, fsr.stamps, q, FSR_STAMPS, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[FSR_STAMPS - 1] > ts[0]) {
        for (int i = 1; i < FSR_STAMPS; i++) fsr.ms[i] += (double)(ts[i] - ts[i - 1]) * gpu_limits.timestampPeriod / 1e6;
        fsr.total += (double)(ts[FSR_STAMPS - 1] - ts[0]) * gpu_limits.timestampPeriod / 1e6; fsr.timed++;
        if (fsr.timed == 240) { LOG("[fsrfg] GPU %.3f ms a picture (%ux%u, %llu pictures): prep %.3f, optical flow %.3f, fields %.3f, interpolate %.3f, inpaint %.3f", fsr.total / fsr.timed, sw, sh, (unsigned long long)fsr.timed,
                fsr.ms[1] / fsr.timed, fsr.ms[2] / fsr.timed, fsr.ms[3] / fsr.timed, fsr.ms[4] / fsr.timed, fsr.ms[5] / fsr.timed);
            if (fsr.fine) { char line[900]; int at = 0; for (int i = 0; i < FSR_PASSES; i++) if (fsr.fine_n[i]) at += snprintf(line + at, sizeof line - (size_t)at, " %s %.3f", fsr_passes[i].name, fsr.fine_ms[i] / fsr.timed);
                LOG("[fsrfg] passes:%s", line); memset(fsr.fine_ms, 0, sizeof fsr.fine_ms); memset(fsr.fine_n, 0, sizeof fsr.fine_n); }
            memset(fsr.ms, 0, sizeof fsr.ms); fsr.total = 0; fsr.timed = 0; }
    }
    unsigned fq = (unsigned)(fsr.runs % FSR_TIMER_RING) * (FSR_MAX_STEPS + 1); static uint8_t fine_reset[FSR_TIMER_RING]; unsigned fslot = (unsigned)(fsr.runs % FSR_TIMER_RING);
    /* the pictures two parities apart have the same steps; this slot was recorded with its current parity */
    if (fsr.fine) {
        uint64_t t[FSR_MAX_STEPS + 1]; int n = 0; for (int i = 0; i < fsr.nsteps[p]; i++) if (fsr.steps[p][i].kind == 0) n++;
        if (fsr.runs >= FSR_TIMER_RING && !fine_reset[fslot] && vkGetQueryPoolResults(device, fsr.fine, fq, (uint32_t)n + 1, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            int k = 0; for (int i = 0; i < fsr.nsteps[p]; i++) if (fsr.steps[p][i].kind == 0) { fsr.fine_ms[fsr.steps[p][i].pass] += (double)(t[k + 1] - t[k]) * gpu_limits.timestampPeriod / 1e6; fsr.fine_n[fsr.steps[p][i].pass]++; k++; } }
        vkCmdResetQueryPool(command, fsr.fine, fq, FSR_MAX_STEPS + 1); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fsr.fine, fq); fine_reset[fslot] = (uint8_t)reset;
    }
    if (fsr.stamps) { vkCmdResetQueryPool(command, fsr.stamps, q, FSR_STAMPS); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fsr.stamps, q); }
    VkDescriptorImageInfo img[6] = {{fg.linear, attachment_view(pic, NULL, pic->format), VK_IMAGE_LAYOUT_GENERAL}, {fg.linear, depth_view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, fsr_view(&fsr.src[p], 0), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, fsr_view(&fsr.mv, 0), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, fsr_view(&fsr.depth, 0), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, fsr_view(&fsr.src_half[p], 0), VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorSet set = descriptor_allocate(fsr.prep_set_layout); VkWriteDescriptorSet w[6];
    for (int i = 0; i < 6; i++) w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img[i]};
    vkUpdateDescriptorSets(device, 6, w, 0, NULL);
    fsr_barrier();
    /* ffx_opticalflow.cpp: everything the flow keeps between frames */
    if (reset) {
        VkClearColorValue zero = {{0}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        image_entry *e[] = {&fsr.scd_temp, &fsr.scd_out, &fsr.hist, &fsr.hist_prev};
        for (size_t i = 0; i < 4; i++) vkCmdClearColorImage(command, e[i]->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        for (int i = 0; i < 2; i++) for (int l = 0; l < FSR_LEVELS; l++) vkCmdClearColorImage(command, fsr.in[i][l].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.prep);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.prep_layout, 0, 1, &set, 0, NULL);
    float pc[28]; memcpy(pc, push, sizeof pc); pc[21] = (float)fsr.scale; pc[22] = (float)fsr.of_scale;
    /* with dynamic resolution the scene is a corner of the depth target */
    pc[26] = fg.view[0] > 0 ? fg.view[0] : 1; pc[27] = fg.view[1] > 0 ? fg.view[1] : 1;
    vkCmdPushConstants(command, fsr.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 112, pc);
    vkCmdDispatch(command, (sw + 7) / 8, (sh + 7) / 8, 1);
    fsr_barrier();
    uint32_t dyn[4] = {slot, slot, slot, slot}, fine_at = fq;
    for (int i = 0; i < fsr.nsteps[p]; i++) {
        const fsr_step *s = &fsr.steps[p][i];
        if (s->skip_on_reset && reset) continue;
        if (s->kind == 2) { if (fsr.stamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fsr.stamps, q + s->x); continue; }
        if (s->kind == 1) { VkClearColorValue v = {.uint32 = {s->value, s->value, s->value, s->value}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command, s->image->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all); fsr_barrier(); continue; }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.pipeline[s->pass]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.layout[s->pass], 0, 1, &s->set, s->ubos, dyn);
        vkCmdDispatch(command, s->x, s->y, s->z);
        fsr_barrier();
        if (fsr.fine) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fsr.fine, ++fine_at);
    }
    fsr.runs++;
}
