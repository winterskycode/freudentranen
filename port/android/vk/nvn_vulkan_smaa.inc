/* Anti-aliasing (TOTK_AA=smaa): SMAA 1x, high preset (third_party/smaa, MIT; shaders/smaa_*.comp; ultra: TOTK_SMAA_ULTRA=1,
   the menu's SMAA quality). Docked, on the scene at its render size: compose's output (0ac9a549 and the passes after it
   into its target, 1600x900 by default), before the game's FXAA and its upscale to the 1920x1080 canvas, which softened
   the stair-steps before SMAA saw them (taa_compose / taa_before_draw: the first draw that reads that target). Handheld (and docked when compose's output was not caught), on the
   game's canvas at the first UI draw into it (fg_before_draw): the scene is finished there (lit, bloomed, tonemapped to linear
   display values in R11G11B10F) and the HUD is not drawn yet, so text and icons stay sharp and our upscale works on an
   anti-aliased picture. In handheld mode the game has no anti-aliasing of its own; docked it runs FXAA before this.
   Three compute passes: luma edges (sqrt as the gamma) plus a copy of the canvas, blending weights, and neighbourhood
   blending from the copy straight into the canvas as an R11G11B10F storage image (Turnip has the format; a device
   without it gets no anti-aliasing). Still scene, 1280x720: ~0.5 ms of GPU time. As Ryujinx does (its Vulkan backend's
   SMAA effect), but before the UI instead of on the presented picture. A frame without UI (the HUD hidden) is
   anti-aliased at the game's copy of the canvas into the presented texture; one that does not draw the world is not (the
   pause menu keeps the paused world in the canvas: fg_menu_frame). */
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"
static struct {
    int on;   /* -1 unread */
    VkDescriptorSetLayout set_layout[3]; VkPipelineLayout layout[3]; VkPipeline pipeline[3];
    VkSampler linear, nearest;
    image_entry edges, blend, copy, area, search;
    int usable;   /* -1 unchecked: the canvas format as a storage image */
    VkPipeline ultra[3];
    float sub[4];     /* the blending weights' subsample indices: 0 (SMAA 1x), T2x's (1,1,1,0) / (2,2,2,0) */
    int scene_done;   /* this frame's scene was anti-aliased at its render size (the canvas is then left alone) */
    uint64_t frames;
} smaa = {.on = -1, .usable = -1};
static int smaa_on(void) {
    if (smaa.on < 0) { const char *e = getenv("TOTK_AA"); smaa.on = e && (!strcmp(e, "smaa") || !strcmp(e, "smaat2x"));
        LOG("[aa] %s", smaa.on ? (!strcmp(e, "smaat2x") ? "SMAA T2x (docked; SMAA 1x in handheld mode)" : "SMAA 1x before the UI") : "off"); }
    return smaa.on;
}
static void smaa_toggle(void) { smaa_on(); smaa.on = !smaa.on; LOG("[aa] toggled %s", smaa.on ? "on" : "off"); }
/* SMAA on compose's output: docked (handheld composes straight into the canvas, which is the scene's size) */
static int smaa_scene(void) {
    static int docked = -1; if (docked < 0) { const char *e = getenv("TOTK_OPERATION_MODE"); docked = e && !strcmp(e, "docked"); }
    return docked && smaa_on();
}
static int smaa_scene_done(void) { return smaa.scene_done; }
static void smaa_scene_mark(int done) { smaa.scene_done = done; }   /* taa_before_draw: done; taa_frame_end: a new frame */
static int smaa_ultra_on = -1;   /* TOTK_SMAA_ULTRA; the menu sets it live (ov_apply) */
static int smaa_ultra(void) { if (smaa_ultra_on < 0) { const char *e = getenv("TOTK_SMAA_ULTRA"); smaa_ultra_on = e && atoi(e) > 0; } return smaa_ultra_on; }
static void smaa_pipeline(int i, const uint32_t *code, size_t bytes, unsigned samplers, unsigned storages) {
    VkDescriptorSetLayoutBinding b[5];
    for (unsigned j = 0; j < samplers + storages; j++) b[j] = (VkDescriptorSetLayoutBinding){.binding = j, .descriptorType = j < samplers ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = samplers + storages, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &smaa.set_layout[i]));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 32};   /* metrics, subsample indices */
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &smaa.set_layout[i], .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &smaa.layout[i]));
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = smaa.layout[i]};
    VK_CHECK(pipeline_create_compute(&pi, &smaa.pipeline[i])); vkDestroyShaderModule(device, module, NULL);
}
/* a lookup texture uploaded once (its staging buffer is held until the batch completes) */
static void smaa_lookup(image_entry *e, VkFormat format, uint32_t w, uint32_t h, const unsigned char *bytes, size_t size) {
    fg_image(e, format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    post_op *held = post_new(3); held->source = buffer_create(size); memcpy(held->source.mapped, bytes, size);
    image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(command, held->source.buffer, e->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}
static void smaa_setup(void) {
    static const uint32_t edge[] = {
#include "smaa_edge_spv.inc"
    }, blend[] = {
#include "smaa_blend_spv.inc"
    }, neighbor[] = {
#include "smaa_neighbor_spv.inc"
    }, edge_u[] = {
#include "smaa_edge_ultra_spv.inc"
    }, blend_u[] = {
#include "smaa_blend_ultra_spv.inc"
    }, neighbor_u[] = {
#include "smaa_neighbor_ultra_spv.inc"
    };
    smaa_pipeline(0, edge, sizeof edge, 1, 2); smaa_pipeline(1, blend, sizeof blend, 3, 1); smaa_pipeline(2, neighbor, sizeof neighbor, 2, 1);
    const uint32_t *u[3] = {edge_u, blend_u, neighbor_u}; size_t ub[3] = {sizeof edge_u, sizeof blend_u, sizeof neighbor_u};
    for (int i = 0; i < 3; i++) {   /* the ultra preset (glslang -DSMAA_PRESET_ULTRA): the same layouts */
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = ub[i], .pCode = u[i]};
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = smaa.layout[i]};
        VK_CHECK(pipeline_create_compute(&pi, &smaa.ultra[i])); vkDestroyShaderModule(device, module, NULL);
    }
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &smaa.linear));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; VK_CHECK(vkCreateSampler(device, &si, NULL, &smaa.nearest));
    smaa_lookup(&smaa.area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, sizeof areaTexBytes);
    smaa_lookup(&smaa.search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, sizeof searchTexBytes);
}
/* one pass: its samplers (image, sampler), then its storage outputs, a thread per pixel */
static void smaa_pass(int i, unsigned n, image_entry *const *in, const VkSampler *samplers, image_entry *const *out, unsigned nout, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo img[5]; VkWriteDescriptorSet wr[5];
    for (unsigned j = 0; j < n; j++) { image_access(in[j], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); img[j] = (VkDescriptorImageInfo){samplers[j], attachment_view(in[j], NULL, in[j]->format), VK_IMAGE_LAYOUT_GENERAL}; }
    for (unsigned j = 0; j < nout; j++) { image_access(out[j], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT); img[n + j] = (VkDescriptorImageInfo){VK_NULL_HANDLE, attachment_view(out[j], NULL, out[j]->format), VK_IMAGE_LAYOUT_GENERAL}; }
    VkDescriptorSet set = descriptor_allocate(smaa.set_layout[i]);
    for (unsigned j = 0; j < n + nout; j++) wr[j] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = j, .descriptorCount = 1,
        .descriptorType = j < n ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img[j]};
    vkUpdateDescriptorSets(device, n + nout, wr, 0, NULL);
    float metrics[8] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h, smaa.sub[0], smaa.sub[1], smaa.sub[2], smaa.sub[3]};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, smaa_ultra() ? smaa.ultra[i] : smaa.pipeline[i]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, smaa.layout[i], 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, smaa.layout[i], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof metrics, metrics);
    vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1);
}
/* The canvas (R11G11B10F) anti-aliased in place; recorded into the current command buffer outside any pass. 0 when not. */
static int smaa_apply(image_entry *canvas) {
    if (smaa.usable < 0) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp);
        smaa.usable = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);   /* image_get gives the canvas storage usage then */
        if (!smaa.usable) LOG("[aa] R11G11B10F is not a storage format on this device: no anti-aliasing"); }
    if (!smaa.usable || canvas->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        static int said; if (!said++ && smaa.usable) LOG("[aa] canvas format %d: not anti-aliased", canvas->format); return 0; }
    uint32_t w = canvas->texture.d.width, h = canvas->texture.d.height;
    graphics_pass_end();
    if (!smaa.pipeline[0]) smaa_setup();
    fg_image(&smaa.edges, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    fg_image(&smaa.blend, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    fg_image(&smaa.copy, VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    smaa_pass(0, 1, (image_entry *[]){canvas}, (VkSampler[]){smaa.linear}, (image_entry *[]){&smaa.edges, &smaa.copy}, 2, w, h);
    smaa_pass(1, 3, (image_entry *[]){&smaa.edges, &smaa.area, &smaa.search}, (VkSampler[]){smaa.linear, smaa.linear, smaa.nearest}, (image_entry *[]){&smaa.blend}, 1, w, h);
    smaa_pass(2, 2, (image_entry *[]){&smaa.copy, &smaa.blend}, (VkSampler[]){smaa.linear, smaa.linear}, (image_entry *[]){canvas}, 1, w, h);
    image_gpu_written(canvas);
    if (!(smaa.frames++ % 600)) LOG("[aa] SMAA frame %llu: canvas %ux%u", (unsigned long long)smaa.frames, w, h);
    return 1;
}

/* SMAA T2x (TOTK_AA=smaat2x, docked; nvn_vulkan_taa.inc's mode 3 jitters the camera between two positions, a quarter
   pixel each way diagonally): SMAA on each jittered frame with that position's subsample indices, into a picture of
   our own, then shaders/smaa_t2x.comp blends it half and half with the last frame's (the other position), reprojected
   with the camera's motion, into compose's output. The two positions average out what the game's lighting does with
   each (the lamps' halos alternate between two states instead of pulsing through sixteen), and only one earlier frame
   is ever used, so what the camera's motion gets wrong (Link moving by himself) leaves at most a one-frame trace. */
static struct { VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline pipe; VkSampler point; image_entry cur[2], vel[2], depth[2]; int i; uint64_t frames; } t2x;
static int t2x_storage(int i) { return i == 4 || i == 5 || i == 7; }   /* the resolve's outputs */
static void smaa_t2x_pipeline(void) {
    if (t2x.pipe) return;
    VkDescriptorSetLayoutBinding b[8];
    for (int i = 0; i < 8; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .descriptorType = t2x_storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &t2x.set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 96};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &t2x.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &t2x.layout));
    static const uint32_t code[] = {
#include "smaa_t2x_spv.inc"
    };
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = t2x.layout};
    VK_CHECK(pipeline_create_compute(&pi, &t2x.pipe)); vkDestroyShaderModule(device, module, NULL);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &t2x.point));
}
/* compose's output (target) anti-aliased: sub, this frame's jitter position (0, 1); reproj (row-major), the last
   frame's view-projection times this one's inverse; reset: no earlier frame to use. 0 when not run. */
static int smaa_t2x_apply(image_entry *target, image_entry *depth, int sub, const double *reproj, int reset) {
    if (smaa.usable < 0) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp); smaa.usable = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT); }
    if (!smaa.usable || target->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return 0;
    uint32_t w = target->texture.d.width, h = target->texture.d.height;
    if (depth->texture.d.width != w || depth->texture.d.height != h) return 0;
    graphics_pass_end();
    if (!smaa.pipeline[0]) smaa_setup();
    smaa_t2x_pipeline();
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    fg_image(&smaa.edges, VK_FORMAT_R8G8B8A8_UNORM, w, h, u); fg_image(&smaa.blend, VK_FORMAT_R8G8B8A8_UNORM, w, h, u);
    fg_image(&smaa.copy, VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, u);
    for (int i = 0; i < 2; i++) { if (t2x.cur[i].texture.d.width != w || t2x.cur[i].texture.d.height != h) reset = 1;
        fg_image(&t2x.cur[i], VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, u); fg_image(&t2x.vel[i], VK_FORMAT_R16_SFLOAT, w, h, u); fg_image(&t2x.depth[i], VK_FORMAT_R32_SFLOAT, w, h, u); }
    float s = sub ? 2.0f : 1.0f; smaa.sub[0] = smaa.sub[1] = smaa.sub[2] = s; smaa.sub[3] = 0;
    image_entry *now = &t2x.cur[t2x.i], *before = &t2x.cur[t2x.i ^ 1];
    smaa_pass(0, 1, (image_entry *[]){target}, (VkSampler[]){smaa.linear}, (image_entry *[]){&smaa.edges, &smaa.copy}, 2, w, h);
    smaa_pass(1, 3, (image_entry *[]){&smaa.edges, &smaa.area, &smaa.search}, (VkSampler[]){smaa.linear, smaa.linear, smaa.nearest}, (image_entry *[]){&smaa.blend}, 1, w, h);
    smaa_pass(2, 2, (image_entry *[]){&smaa.copy, &smaa.blend}, (VkSampler[]){smaa.linear, smaa.linear}, (image_entry *[]){now}, 1, w, h);
    smaa.sub[0] = smaa.sub[1] = smaa.sub[2] = 0;
    image_access(now, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(before, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(depth, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(&t2x.vel[t2x.i ^ 1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(target, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT); image_access(&t2x.vel[t2x.i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    image_access(&t2x.depth[t2x.i ^ 1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(&t2x.depth[t2x.i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkImageView dv = image_view_formatted(depth, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, (VkComponentMapping){0}, depth->format)->view;
    VkDescriptorImageInfo ii[8] = {{t2x.point, attachment_view(now, NULL, now->format), VK_IMAGE_LAYOUT_GENERAL}, {t2x.point, attachment_view(before, NULL, before->format), VK_IMAGE_LAYOUT_GENERAL},
        {t2x.point, dv, VK_IMAGE_LAYOUT_GENERAL}, {t2x.point, attachment_view(&t2x.vel[t2x.i ^ 1], NULL, t2x.vel[0].format), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, attachment_view(target, NULL, target->format), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, attachment_view(&t2x.vel[t2x.i], NULL, t2x.vel[0].format), VK_IMAGE_LAYOUT_GENERAL},
        {t2x.point, attachment_view(&t2x.depth[t2x.i ^ 1], NULL, t2x.depth[0].format), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, attachment_view(&t2x.depth[t2x.i], NULL, t2x.depth[0].format), VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorSet set = descriptor_allocate(t2x.set_layout); VkWriteDescriptorSet wr[8];
    for (int i = 0; i < 8; i++) wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = t2x_storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(device, 8, wr, 0, NULL);
    float push[24];
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) push[c * 4 + r] = (float)reproj[r * 4 + c];   /* GLSL mat4 is column-major */
    push[16] = (float)w; push[17] = (float)h; push[18] = 1.0f / w; push[19] = 1.0f / h;
    push[20] = reset ? 1.0f : 0.0f; push[21] = 1.0f; push[22] = live_exp_has("t2xclamp") ? 1.0f : 0.0f; push[23] = live_exp_has("t2xnodepth") ? 1.0f : 0.0f;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, t2x.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, t2x.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, t2x.pipe);
    vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1);
    image_gpu_written(target);
    t2x.i ^= 1;
    if (!(t2x.frames++ % 600)) LOG("[aa] SMAA T2x frame %llu: %ux%u", (unsigned long long)t2x.frames, w, h);
    return 1;
}
