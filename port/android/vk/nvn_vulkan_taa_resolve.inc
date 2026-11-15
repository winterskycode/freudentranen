/* Temporal anti-aliasing (TOTK_AA=taa, docked; nvn_vulkan_taa.inc's mode 4 jitters the camera by a Halton (2, 3) step
   and calls this on compose's output before anything reads it): shaders/taa_resolve.comp accumulates the jittered
   frames with object motion (nvn_vulkan_motion.inc, on while this runs) or the camera's, history dropped where last
   frame's depth disagrees, clipped to this frame's local colour spread. Compose's output is copied first (the resolve
   reads a 3x3 of it and writes it back); the history (tonemapped, RGBA16F) and last frame's depth ping-pong.
   TOTK_TAA_SOFTNESS (sharp, balanced, soft; the menu sets it live) picks the filter width and the clip box. Live words
   (<files>/exp.live) for tuning: taagamma=, taamin=, taamax=, taawidth= override the look's numbers, taadebug=1 shows
   the history's use (blue dropped off screen, red disoccluded, green object motion, grey the current frame's weight x4),
   taadebug=2 the motion, taanodepth leaves out the depth test, taajflip turns the jitter's sign. TOTK_TAA_TIME=1 logs
   the resolve's GPU time. */
enum { TAA_SHARP, TAA_BALANCED, TAA_SOFT, TAA_LOOKS };
static const char *const taa_look_values[TAA_LOOKS] = {"sharp", "balanced", "soft"};
/* per look: clip box gamma, least and most weight of this frame, filter width (pixels) */
static const float taa_looks[TAA_LOOKS][4] = {{1.0f, 0.08f, 0.25f, 0.85f}, {1.25f, 0.06f, 0.2f, 1.0f}, {1.5f, 0.05f, 0.18f, 1.25f}};
static struct { VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline pipe; VkSampler point, linear;
    image_entry copy, hist[2], depth[2]; int i, look; VkQueryPool stamps; uint64_t frames, stamp_n; double gpu_ms; } tr = {.look = -1};
static int taa_look_of(const char *v) { for (int m = 0; v && m < TAA_LOOKS; m++) if (!strcmp(v, taa_look_values[m])) return m; return TAA_BALANCED; }
static int tr_storage(int i) { return i >= 5; }
static void taa_resolve_pipeline(void) {
    if (tr.pipe) return;
    VkDescriptorSetLayoutBinding b[8];
    for (int i = 0; i < 8; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .descriptorType = tr_storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &tr.set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 128};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &tr.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &tr.layout));
    static const uint32_t code[] = {
#include "taa_resolve_spv.inc"
    };
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = tr.layout};
    VK_CHECK(pipeline_create_compute(&pi, &tr.pipe)); vkDestroyShaderModule(device, module, NULL);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &tr.point));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VK_CHECK(vkCreateSampler(device, &si, NULL, &tr.linear));
}
static float taa_live(const char *word, float fallback) {
    const char *w = live_exp[0] ? exp_word(live_exp, word, 0) : NULL;
    return w ? (float)atof(w) : fallback;
}
/* compose's output (target) resolved in place; reproj (row-major), the last frame's view-projection times this one's
   inverse; reset: no earlier frame to use. 0 when not run. */
static int taa_resolve_apply(image_entry *target, image_entry *depth, const double *reproj, int reset) {
    if (smaa.usable < 0) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp); smaa.usable = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT); }
    if (!smaa.usable || target->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return 0;
    uint32_t w = target->texture.d.width, h = target->texture.d.height;
    if (depth->texture.d.width != w || depth->texture.d.height != h) {
        static int told; if (!told++) LOG("[taa] not run: compose's output %ux%u, the depth %ux%u", w, h, depth->texture.d.width, depth->texture.d.height);
        return 0; }
    if (tr.look < 0) tr.look = taa_look_of(getenv("TOTK_TAA_SOFTNESS"));
    graphics_pass_end();
    taa_resolve_pipeline();
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    fg_image(&tr.copy, VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, u | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (int i = 0; i < 2; i++) { if (tr.hist[i].texture.d.width != w || tr.hist[i].texture.d.height != h) reset = 1;
        fg_image(&tr.hist[i], VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u); fg_image(&tr.depth[i], VK_FORMAT_R32_SFLOAT, w, h, u); }
    if (!tr.stamps) { const char *e = getenv("TOTK_TAA_TIME");
        if (e && atoi(e) > 0) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8}; VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &tr.stamps)); } }
    uint32_t q = (uint32_t)(tr.stamp_n % 4) * 2;
    if (tr.stamps) { uint64_t ts[2];
        if (tr.stamp_n >= 4 && vkGetQueryPoolResults(device, tr.stamps, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) tr.gpu_ms += (double)(ts[1] - ts[0]) * gpu_limits.timestampPeriod / 1e6;
        vkCmdResetQueryPool(command, tr.stamps, q, 2); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tr.stamps, q); }
    image_access(target, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_access(&tr.copy, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy cr = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(command, target->image, VK_IMAGE_LAYOUT_GENERAL, tr.copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cr);
    image_gpu_written(&tr.copy);
    image_entry *obj = mo_image();
    image_entry *hp = &tr.hist[tr.i ^ 1], *hn = &tr.hist[tr.i], *dp = &tr.depth[tr.i ^ 1], *dn = &tr.depth[tr.i];
    image_access(&tr.copy, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(hp, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(depth, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT); image_access(dp, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(target, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT); image_access(hn, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    image_access(dn, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkImageView dv = image_view_formatted(depth, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, (VkComponentMapping){0}, depth->format)->view;
    VkDescriptorImageInfo ii[8] = {
        {tr.point, attachment_view(&tr.copy, NULL, tr.copy.format), VK_IMAGE_LAYOUT_GENERAL}, {tr.linear, attachment_view(hp, NULL, hp->format), VK_IMAGE_LAYOUT_GENERAL},
        {tr.point, dv, VK_IMAGE_LAYOUT_GENERAL}, {tr.point, attachment_view(dp, NULL, dp->format), VK_IMAGE_LAYOUT_GENERAL},
        {tr.point, obj ? attachment_view(obj, NULL, obj->format) : dv, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, attachment_view(target, NULL, target->format), VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, attachment_view(hn, NULL, hn->format), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, attachment_view(dn, NULL, dn->format), VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorSet set = descriptor_allocate(tr.set_layout); VkWriteDescriptorSet wr[8];
    for (int i = 0; i < 8; i++) wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
        .descriptorType = tr_storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(device, 8, wr, 0, NULL);
    float push[32];
    /* GLSL mat4 is column-major */
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) push[c * 4 + r] = (float)reproj[r * 4 + c];
    push[16] = (float)w; push[17] = (float)h; push[18] = 1.0f / w; push[19] = 1.0f / h;
    float flip = live_exp_has("taajflip") ? -1.0f : 1.0f;
    push[20] = flip * taa.jx; push[21] = flip * taa.jy; push[22] = reset ? 1.0f : 0.0f; push[23] = 1.0f;
    const float *look = taa_looks[tr.look];
    push[24] = taa_live("taagamma=", look[0]); push[25] = taa_live("taamin=", look[1]); push[26] = taa_live("taamax=", look[2]); push[27] = taa_live("taawidth=", look[3]);
    push[28] = obj ? 1.0f : 0.0f; push[29] = taa_live("taadebug=", 0); push[30] = live_exp_has("taanodepth") ? 1.0f : 0.0f; push[31] = 0;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, tr.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, tr.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, tr.pipe);
    vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1);
    image_gpu_written(target); image_gpu_written(hn); image_gpu_written(dn);
    if (tr.stamps) { vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tr.stamps, q + 1); tr.stamp_n++; }
    tr.i ^= 1;
    if (!(tr.frames++ % 600)) {
        LOG("[taa] frame %llu: %ux%u, %s, object motion %s", (unsigned long long)tr.frames, w, h, taa_look_values[tr.look], obj ? "in" : "not in");
        if (tr.stamps && tr.frames > 1) LOG("[taa] GPU %.3f ms a frame (copy and resolve)", tr.gpu_ms / 600.0);
        tr.gpu_ms = 0; }
    return 1;
}
