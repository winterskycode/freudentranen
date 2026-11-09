/* Temporal anti-aliasing with Arm ASR (TOTK_TAA=asr): Arm Accuracy Super Resolution, FSR 2.2's method for mobile GPUs
   (MIT, third_party/asr), its Quality path, in place of the game's own upscale (asr_upscale_draw), with the camera's
   matrices jittered (taa_uniform, nvn_vulkan_taa.inc) and the camera's motion. FSR 2 has what Snapdragon GSR 2 lacks
   here: locks on thin features, carried along with the motion, and its luma-instability and disocclusion tests.
   Passes, all compute (shaders/build_asr.sh): our motion vectors (asr_motion.comp), ASR's luminance pyramid (SPD, without
   subgroup operations), reconstruct-previous-depth, depth clip, lock, accumulate (our compute entries for its fragment
   passes: the same functions, the render targets stored as images). Compiled with FFXM_HALF=0 (no shaderFloat16 on the
   device). The reactive and transparency masks are empty.
   TOTK_TAA_TIME=1 logs the GPU time of the passes every 300 frames. */
#include "third_party/asr/host/ffxm_fsr2_maximum_bias.h"

enum { ASR_MOTION, ASR_LUMA, ASR_RECON, ASR_CLIP, ASR_LOCK, ASR_ACCUM, ASR_PASSES };
/* Each pass's set 1, by binding: T a sampled image, U a storage image (the entries' FSR2_BIND_* numbers) */
static const char *const asr_set1[ASR_PASSES] = {
    /* colour; SPD counter, luma mip 4, mip 5, auto exposure */
    [ASR_LUMA] = "TUUUU",
    /* motion, depth, colour, exposure; previous depth, dilated depth, dilated motion, lock luma */
    [ASR_RECON] = "TTTTUUUU",
    /* previous depth, dilated motion, dilated depth, reactive, T&C, last dilated motion, motion, colour, depth,
       exposure; dilated masks, prepared colour */
    [ASR_CLIP] = "TTTTTTTTTTUU",
    /* lock luma; new locks, previous depth */
    [ASR_LOCK] = "TUU",
    /* exposure, masks, dilated motion, history, lock status, prepared, Lanczos, max bias, luma mips, auto exposure,
       luma history, temporal reactive; new locks, history, lock status, luma history, output */
    [ASR_ACCUM] = "TTTTTTTTTTTTUUUUU",
};
static struct {
    VkDescriptorSetLayout set0, set1[ASR_PASSES]; VkPipelineLayout layout[ASR_PASSES]; VkPipeline pipe[ASR_PASSES], debug;
    VkSampler point, linear;
    image_entry mv, dd, dmv[2], luma, recon, newlocks, masks, prepared, hist[2], lock[2], lumah[2], out;
    image_entry expmips, atomic, autoexp, lanczos, maxbias, zero_r8, zero_exposure;
    /* render (the scene) and display (the canvas) sizes */
    uint32_t w, h, dw, dh, levels; int fresh, luts;
    unsigned frame_index; uint64_t last_ns;
    VkQueryPool timer; uint64_t timer_n, timer_frames; double timer_ms;
} asr;

static void asr_pipelines(void) {
    if (asr.pipe[ASR_ACCUM]) return;
    VkDescriptorSetLayoutBinding b0[4] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b0};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &asr.set0));
    static const uint32_t motion[] = {
#include "asr_motion_spv.inc"
    }, luma[] = {
#include "asr_luminance_pyramid_spv.inc"
    }, recon[] = {
#include "asr_reconstruct_spv.inc"
    }, clip[] = {
#include "asr_depth_clip_spv.inc"
    }, lock[] = {
#include "asr_lock_spv.inc"
    }, accum[] = {
#include "asr_accumulate_spv.inc"
    };
    static const uint32_t debug[] = {
#include "asr_debug_spv.inc"
    };
    const uint32_t *code[ASR_PASSES] = {motion, luma, recon, clip, lock, accum};
    size_t size[ASR_PASSES] = {sizeof motion, sizeof luma, sizeof recon, sizeof clip, sizeof lock, sizeof accum};
    for (int p = 0; p < ASR_PASSES; p++) {
        VkDescriptorSetLayoutBinding b[24]; unsigned n = 0;
        /* the depth (combined), the motion out, object motion (combined); its push constants */
        if (p == ASR_MOTION) {
            b[n++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            b[n++] = (VkDescriptorSetLayoutBinding){1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            b[n++] = (VkDescriptorSetLayoutBinding){2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        } else for (const char *t = asr_set1[p]; *t; t++, n++)
            b[n] = (VkDescriptorSetLayoutBinding){n, *t == 'U' ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        li.bindingCount = n; li.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &asr.set1[p]));
        VkDescriptorSetLayout sets[2] = {asr.set0, asr.set1[p]};
        VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = p == ASR_MOTION ? 1 : 2,
            .pSetLayouts = p == ASR_MOTION ? &asr.set1[p] : sets, .pushConstantRangeCount = p == ASR_MOTION, .pPushConstantRanges = &push};
        VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &asr.layout[p]));
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[p], .pCode = code[p]};
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = asr.layout[p]};
        VK_CHECK(pipeline_create_compute(&pi, &asr.pipe[p])); vkDestroyShaderModule(device, module, NULL);
        /* the debug view (asr_show): the motion pass's layout */
        if (p == ASR_MOTION) {
            mi.codeSize = sizeof debug; mi.pCode = debug; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module)); pi.stage.module = module;
            VK_CHECK(pipeline_create_compute(&pi, &asr.debug)); vkDestroyShaderModule(device, module, NULL);
        }
    }
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &asr.point));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VK_CHECK(vkCreateSampler(device, &si, NULL, &asr.linear));
    const char *t = getenv("TOTK_TAA_TIME");
    if (t && atoi(t) > 0) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 8}; VK_CHECK(vkCreateQueryPool(device, &qi, NULL, &asr.timer)); }
}

static VkImageView asr_view(image_entry *e, uint32_t level, uint32_t levels) {
    VkImageAspectFlags aspect = e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return image_view_formatted(e, VK_IMAGE_VIEW_TYPE_2D, (VkImageSubresourceRange){aspect, level, levels, 0, 1}, (VkComponentMapping){0}, e->format)->view;
}
static void asr_clear(image_entry *e, VkClearColorValue v) {
    image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e->texture.d.levels, 0, 1};
    vkCmdClearColorImage(command, e->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
}
/* A constant image: its bytes through a staging buffer of its own (once) */
static void asr_upload(image_entry *e, const void *data, uint32_t bytes) {
    vk_buffer s = buffer_create(bytes); memcpy(s.mapped, data, bytes);
    image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {e->texture.d.width, e->texture.d.height, 1}};
    vkCmdCopyBufferToImage(command, s.buffer, e->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}
/* ASR's resources: render size (w x h) for the scene's, display size (dw x dh) for the history's; the constant ones once */
static void asr_images(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!asr.luts) {
        asr.luts = 1;
        fg_image(&asr.lanczos, VK_FORMAT_R16_SNORM, 128, 1, u); fg_image(&asr.maxbias, VK_FORMAT_R16_SNORM, FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH, FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT, u);
        fg_image(&asr.zero_r8, VK_FORMAT_R8_UNORM, 1, 1, u); fg_image(&asr.zero_exposure, VK_FORMAT_R32G32_SFLOAT, 1, 1, u);
        /* the shader's rgba32f qualifier */
        fg_image(&asr.atomic, VK_FORMAT_R32_UINT, 1, 1, u); fg_image(&asr.autoexp, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, u);
        int16_t lanczos[128], bias[FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT];
        /* ASR's lanczos2 table (ffxm_fsr2.cpp) */
        for (int i = 0; i < 128; i++) {
            double x = 2.0 * i / 127.0, y = x < 1e-7 ? 1.0 : sin(M_PI * x) / (M_PI * x) * (sin(0.5 * M_PI * x) / (0.5 * M_PI * x));
            lanczos[i] = (int16_t)lround(y * 32767.0);
        }
        for (int i = 0; i < FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFXM_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT; i++) bias[i] = (int16_t)lround(ffxmFsr2MaximumBias[i] / 2.0 * 32767.0);
        asr_upload(&asr.lanczos, lanczos, sizeof lanczos); asr_upload(&asr.maxbias, bias, sizeof bias);
        /* an exposure of 0 reads as 1 */
        asr_clear(&asr.zero_r8, (VkClearColorValue){.float32 = {0}}); asr_clear(&asr.zero_exposure, (VkClearColorValue){.float32 = {0}});
        asr_clear(&asr.atomic, (VkClearColorValue){.uint32 = {0}});
    }
    if (asr.w == w && asr.h == h && asr.dw == dw && asr.dh == dh) return;
    asr.w = w; asr.h = h; asr.dw = dw; asr.dh = dh; asr.fresh = 1;
    uint32_t hw = w / 2 > 1 ? w / 2 : 1, hh = h / 2 > 1 ? h / 2 : 1, m = hw > hh ? hw : hh; asr.levels = 1; while (m >>= 1) asr.levels++;
    fg_image(&asr.mv, VK_FORMAT_R16G16_SFLOAT, w, h, u); fg_image(&asr.dd, VK_FORMAT_R32_SFLOAT, w, h, u);
    fg_image(&asr.dmv[0], VK_FORMAT_R16G16_SFLOAT, w, h, u); fg_image(&asr.dmv[1], VK_FORMAT_R16G16_SFLOAT, w, h, u);
    fg_image(&asr.luma, VK_FORMAT_R16_SFLOAT, w, h, u); fg_image(&asr.recon, VK_FORMAT_R32_UINT, w, h, u);
    fg_image(&asr.newlocks, VK_FORMAT_R8_UNORM, dw, dh, u); fg_image(&asr.masks, VK_FORMAT_R8G8_UNORM, w, h, u);
    fg_image(&asr.prepared, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u); fg_image(&asr.out, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh, u);
    for (int i = 0; i < 2; i++) {
        fg_image(&asr.hist[i], VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh, u); fg_image(&asr.lock[i], VK_FORMAT_R16G16_SFLOAT, dw, dh, u);
        fg_image(&asr.lumah[i], VK_FORMAT_R8G8B8A8_UNORM, dw, dh, u);
    }
    fg_image_levels(&asr.expmips, VK_FORMAT_R16_SFLOAT, hw, hh, asr.levels, u);
}

static void asr_write(VkWriteDescriptorSet *w, VkDescriptorImageInfo *ii, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) {
    *ii = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    *w = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding, .descriptorCount = 1, .descriptorType = type, .pImageInfo = ii};
}
/* One pass: its images (views in its set 1 order; stage access: reads for T, read-write for U), set 0, the dispatch */
static void asr_pass(int p, VkDescriptorSet set0, image_entry *const *img, const VkImageView *views, uint32_t gx, uint32_t gy) {
    VkDescriptorSet set = descriptor_allocate(asr.set1[p]);
    VkWriteDescriptorSet w[24]; VkDescriptorImageInfo ii[24]; unsigned n = 0;
    for (const char *t = asr_set1[p]; *t; t++, n++) {
        int storage = *t == 'U';
        image_access(img[n], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, storage ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
        asr_write(&w[n], &ii[n], set, n, storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, views[n]);
    }
    vkUpdateDescriptorSets(device, n, w, 0, NULL);
    VkDescriptorSet sets[2] = {set0, set};
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.layout[p], 0, 2, sets, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.pipe[p]);
    vkCmdDispatch(command, gx, gy, 1);
}

/* Live word asrshow=<what> (measurement): one of ASR's values in grey instead of the picture. clip: the depth clip (the
   prepared colour's w), weight: the history's accumulated weight (w), lock: the lock lifetime, react / mask: the dilated
   reactive and accumulation masks, motion: the dilated motion's length (x10 per pixel), luma: the lock input luma. */
static void asr_show(int rd, int wr) {
    const char *w = live_exp[0] ? exp_word(live_exp, "asrshow=", 0) : NULL;
    if (!w) return;
    image_entry *src = NULL; float sel[4] = {0, 1, 0, 0};
    if (!strncmp(w, "clip", 4)) { src = &asr.prepared; sel[0] = 3; }
    else if (!strncmp(w, "weight", 6)) { src = &asr.hist[wr]; sel[0] = 3; }
    else if (!strncmp(w, "lock", 4)) { src = &asr.lock[wr]; sel[0] = 0; }
    else if (!strncmp(w, "react", 5)) { src = &asr.masks; sel[0] = 0; }
    else if (!strncmp(w, "mask", 4)) { src = &asr.masks; sel[0] = 1; }
    else if (!strncmp(w, "motion", 6)) { src = &asr.dmv[wr]; sel[1] = 10; sel[2] = 1; }
    else if (!strncmp(w, "luma", 4)) { src = &asr.luma; sel[0] = 0; }
    if (!src) return;
    VkDescriptorSet set = descriptor_allocate(asr.set1[ASR_MOTION]); VkWriteDescriptorSet wd[2]; VkDescriptorImageInfo ii[2];
    image_access(src, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    image_access(&asr.out, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    asr_write(&wd[0], &ii[0], set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, asr_view(src, 0, 1)); ii[0].sampler = asr.point;
    asr_write(&wd[1], &ii[1], set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, asr_view(&asr.out, 0, 1));
    vkUpdateDescriptorSets(device, 2, wd, 0, NULL);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.layout[ASR_MOTION], 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, asr.layout[ASR_MOTION], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof sel, sel);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.debug);
    vkCmdDispatch(command, (asr.dw + 7) / 8, (asr.dh + 7) / 8, 1);
}
/* The passes on the scene (the game's upscale's source, asr_upscale_draw), reconstructed at dw x dh: returns the
   result, NULL when not run */
static image_entry *asr_run(image_entry *scene, uint32_t dw, uint32_t dh) {
    uint32_t w = scene->texture.d.width, h = scene->texture.d.height;
    if (scene->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !fg.depth || fg.depth->texture.d.width != w || fg.depth->texture.d.height != h) {
        static int said; if (said++ < 5) LOG("[asr] scene %ux%u and G-buffer depth %ux%u differ: not run", w, h, fg.depth ? fg.depth->texture.d.width : 0, fg.depth ? fg.depth->texture.d.height : 0);
        taa.skipped++; return NULL;
    }
    graphics_pass_end();
    asr_pipelines();
    image_upload(scene);
    asr_images(w, h, dw, dh);
    double m[16]; int reset = taa_camera(w, h, m) || asr.fresh;
    int rd = taa.cur, wr = rd ^ 1;
    /* new images: what ASR's host clears at creation (and the previous depth the lock pass would have reset) */
    if (asr.fresh) {
        asr.fresh = 0;
        asr_clear(&asr.newlocks, (VkClearColorValue){.float32 = {0}}); asr_clear(&asr.recon, (VkClearColorValue){.uint32 = {0x3f800000}});
        for (int i = 0; i < 2; i++) { asr_clear(&asr.lock[i], (VkClearColorValue){.float32 = {0}}); asr_clear(&asr.dmv[i], (VkClearColorValue){.float32 = {0}}); }
        asr_clear(&asr.prepared, (VkClearColorValue){.float32 = {0}});
    }
    /* ASR's reset: the history read this frame, the luminance and its exposure */
    if (reset) {
        asr.frame_index = 0;
        asr_clear(&asr.hist[rd], (VkClearColorValue){.float32 = {0}}); asr_clear(&asr.lock[rd], (VkClearColorValue){.float32 = {0}});
        asr_clear(&asr.lumah[rd], (VkClearColorValue){.float32 = {0}}); asr_clear(&asr.expmips, (VkClearColorValue){.float32 = {0}});
        asr_clear(&asr.autoexp, (VkClearColorValue){.float32 = {-1.0f, 1e8f, 0, 0}});
    } else asr.frame_index++;
    int timed = asr.timer != VK_NULL_HANDLE; unsigned q = (unsigned)(asr.timer_frames % 4) * 2;
    if (timed) {
        uint64_t ts[2];
        if (asr.timer_frames >= 4 && vkGetQueryPoolResults(device, asr.timer, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
            asr.timer_ms += (double)(ts[1] - ts[0]) * gpu_limits.timestampPeriod / 1e6; asr.timer_n++; }
        vkCmdResetQueryPool(command, asr.timer, q, 2); vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, asr.timer, q);
    }

    { VkDescriptorSet set = descriptor_allocate(asr.set1[ASR_MOTION]); VkWriteDescriptorSet wd[3]; VkDescriptorImageInfo ii[3];
      image_access(fg.depth, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      image_access(&asr.mv, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
      /* object motion when there is this frame's, else the depth (unread) */
      image_entry *obj = mo_image();
      asr_write(&wd[0], &ii[0], set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, asr_view(fg.depth, 0, 1)); ii[0].sampler = asr.point;
      asr_write(&wd[1], &ii[1], set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, asr_view(&asr.mv, 0, 1));
      asr_write(&wd[2], &ii[2], set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, obj ? asr_view(obj, 0, 1) : asr_view(fg.depth, 0, 1)); ii[2].sampler = asr.point;
      vkUpdateDescriptorSets(device, 3, wd, 0, NULL);
      float push[24];
      /* GLSL mat4 is column-major */
      for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) push[c * 4 + r] = (float)m[r * 4 + c];
      push[16] = (float)w; push[17] = (float)h; push[18] = 1.0f / w; push[19] = 1.0f / h; push[20] = 1.0f; push[21] = obj ? 1.0f : 0.0f; push[22] = push[23] = 0;
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.layout[ASR_MOTION], 0, 1, &set, 0, NULL);
      vkCmdPushConstants(command, asr.layout[ASR_MOTION], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, asr.pipe[ASR_MOTION]);
      vkCmdDispatch(command, (w + 7) / 8, (h + 7) / 8, 1); }

    /* cbFSR2 (std140, ffxm_fsr2_callbacks_glsl.h) and cbSPD */
    uint8_t *cb, *spd;
    VkDescriptorBufferInfo cbi = uniform_allocate_overwrite(128, &cb), spdi = uniform_allocate_overwrite(32, &spd);
    memset(cb, 0, 128); memset(spd, 0, 32);
    int32_t *ci = (int32_t *)cb; float *cf = (float *)cb;
    /* render, max render, input colour */
    ci[0] = ci[2] = ci[6] = (int32_t)w; ci[1] = ci[3] = ci[7] = (int32_t)h;
    /* display */
    ci[4] = (int32_t)dw; ci[5] = (int32_t)dh;
    /* luma mip (level 4 of the half-size chain) */
    ci[8] = (int32_t)(w / 32); ci[9] = (int32_t)(h / 32); ci[10] = 4; ci[11] = (int32_t)asr.frame_index;
    double n = fg.cam[56] > 0 ? fg.cam[56] : 0.1, f = fg.cam[57] > n ? fg.cam[57] : 25000.0, fq = f / (n - f);
    /* view depth from the 0..1 depth: GL clip z to 0..1 is D3D's form */
    cf[12] = (float)-fq; cf[13] = (float)(fq * n);
    /* tan of the half fields of view */
    cf[14] = fabsf(fg.cam[28]) > 0 ? 1.0f / fabsf(fg.cam[28]) : 1.0f; cf[15] = fabsf(fg.cam[33]) > 0 ? 1.0f / fabsf(fg.cam[33]) : 1.0f;
    /* the content's step in pixels, x right, y down (what taa_uniform put in the matrices) */
    cf[16] = taa.jx; cf[17] = taa.jy;
    /* motion in texture UV already */
    cf[18] = cf[19] = 1.0f;
    cf[20] = (float)w / dw; cf[21] = (float)h / dh;
    /* pre-exposure, the last frame's */
    cf[24] = cf[25] = 1.0f;
    /* tan of the horizontal half field of view */
    cf[26] = cf[14];
    cf[27] = (float)TAA_PHASES;
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC); double dt = asr.last_ns ? (now - asr.last_ns) / 1e9 : 1.0 / 30; asr.last_ns = now;
    cf[28] = (float)(dt < 0 ? 0 : dt > 1 ? 1 : dt);
    /* view space to meters */
    cf[30] = 1.0f;
    /* the box scale's floor (our addition in accumulate.h) */
    { const char *bx = live_exp[0] ? exp_word(live_exp, "asrbox=", 0) : NULL;
      cf[29] = bx ? (float)atof(bx) : 1.0f; }
    uint32_t gx = (w - 1) / 64 + 1, gy = (h - 1) / 64 + 1, mips = 0, r = w > h ? w : h; while (r >>= 1) mips++; if (mips > 12) mips = 12;
    /* ffxSpdSetup over the whole scene */
    uint32_t *su = (uint32_t *)spd; su[0] = mips; su[1] = gx * gy; su[2] = su[3] = 0; su[4] = w; su[5] = h;
    VkDescriptorSet set0 = descriptor_allocate(asr.set0);
    { VkDescriptorImageInfo s[2] = {{asr.point, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}, {asr.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
      VkWriteDescriptorSet wd[4] = {
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set0, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &s[0]},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set0, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &s[1]},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set0, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cbi},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set0, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &spdi}};
      vkUpdateDescriptorSets(device, 4, wd, 0, NULL); }

    VkImageView color = asr_view(scene, 0, 1), depth = asr_view(fg.depth, 0, 1), zero = asr_view(&asr.zero_r8, 0, 1), exposure = asr_view(&asr.zero_exposure, 0, 1);
    uint32_t tx = (w + 7) / 8, ty = (h + 7) / 8;
    { image_entry *i[] = {scene, &asr.atomic, &asr.expmips, &asr.expmips, &asr.autoexp};
      VkImageView v[] = {color, asr_view(&asr.atomic, 0, 1), asr_view(&asr.expmips, 4, 1), asr_view(&asr.expmips, 5, 1), asr_view(&asr.autoexp, 0, 1)};
      asr_pass(ASR_LUMA, set0, i, v, gx, gy); }
    { image_entry *i[] = {&asr.mv, fg.depth, scene, &asr.zero_exposure, &asr.recon, &asr.dd, &asr.dmv[wr], &asr.luma};
      VkImageView v[] = {asr_view(&asr.mv, 0, 1), depth, color, exposure, asr_view(&asr.recon, 0, 1), asr_view(&asr.dd, 0, 1), asr_view(&asr.dmv[wr], 0, 1), asr_view(&asr.luma, 0, 1)};
      asr_pass(ASR_RECON, set0, i, v, tx, ty); }
    { image_entry *i[] = {&asr.recon, &asr.dmv[wr], &asr.dd, &asr.zero_r8, &asr.zero_r8, &asr.dmv[rd], &asr.mv, scene, fg.depth, &asr.zero_exposure, &asr.masks, &asr.prepared};
      VkImageView v[] = {asr_view(&asr.recon, 0, 1), asr_view(&asr.dmv[wr], 0, 1), asr_view(&asr.dd, 0, 1), zero, zero, asr_view(&asr.dmv[rd], 0, 1), asr_view(&asr.mv, 0, 1), color, depth, exposure,
          asr_view(&asr.masks, 0, 1), asr_view(&asr.prepared, 0, 1)};
      asr_pass(ASR_CLIP, set0, i, v, tx, ty); }
    { image_entry *i[] = {&asr.luma, &asr.newlocks, &asr.recon};
      VkImageView v[] = {asr_view(&asr.luma, 0, 1), asr_view(&asr.newlocks, 0, 1), asr_view(&asr.recon, 0, 1)};
      asr_pass(ASR_LOCK, set0, i, v, tx, ty); }
    { image_entry *i[] = {&asr.zero_exposure, &asr.masks, &asr.dmv[wr], &asr.hist[rd], &asr.lock[rd], &asr.prepared, &asr.lanczos, &asr.maxbias, &asr.expmips, &asr.autoexp, &asr.lumah[rd], &asr.zero_r8,
          &asr.newlocks, &asr.hist[wr], &asr.lock[wr], &asr.lumah[wr], &asr.out};
      VkImageView v[] = {exposure, asr_view(&asr.masks, 0, 1), asr_view(&asr.dmv[wr], 0, 1), asr_view(&asr.hist[rd], 0, 1), asr_view(&asr.lock[rd], 0, 1), asr_view(&asr.prepared, 0, 1),
          asr_view(&asr.lanczos, 0, 1), asr_view(&asr.maxbias, 0, 1), asr_view(&asr.expmips, 0, asr.levels), asr_view(&asr.autoexp, 0, 1), asr_view(&asr.lumah[rd], 0, 1), zero,
          asr_view(&asr.newlocks, 0, 1), asr_view(&asr.hist[wr], 0, 1), asr_view(&asr.lock[wr], 0, 1), asr_view(&asr.lumah[wr], 0, 1), asr_view(&asr.out, 0, 1)};
      asr_pass(ASR_ACCUM, set0, i, v, (dw + 7) / 8, (dh + 7) / 8); }

    if (timed) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, asr.timer, q + 1);
        if (++asr.timer_frames % 300 == 0 && asr.timer_n) { LOG("[asr] GPU time %.3f ms a frame (%ux%u, %llu frames)", asr.timer_ms / asr.timer_n, w, h, (unsigned long long)asr.timer_n); asr.timer_ms = 0; asr.timer_n = 0; }
    }
    asr_show(rd, wr);
    taa.cur ^= 1; taa.hist_ok = 1; taa.ran = 1; taa.done++;
    if (!(taa.done % 600)) LOG("[asr] %llu frames (%ux%u), %llu resets, %llu not run", (unsigned long long)taa.done, w, h, (unsigned long long)taa.resets, (unsigned long long)taa.skipped);
    return &asr.out;
}

/* The game's upscale (docked: a3a570eb, 1.4.3's ac58fc18 keeps values above 1) draws its FXAA'd scene (fragment texture
   slot 0) into the 1920x1080 canvas, before the UI. ASR writes the canvas from that scene instead and the draw is
   skipped, when it overwrites the whole canvas (one target, no blending, the viewport over the target) and its constants
   (fragment uniform 0: con0.xy = the scene's valid size over the output's, the output size as integers at floats 16/17)
   say the whole scene texture is used (dynamic resolution draws a smaller region: the game's own upscale then, as the
   Mac's MetalFX upscale does). Returns 1 when the draw is replaced (draw_graphics_inner, before its bindings). */
static int asr_upscale_draw(const shader_entry *fs) {
    if (taa.on != 2 || !fs || !fs->module || fg.menu) return 0;
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
    uint32_t dw = dst ? dst->texture.d.width : 0, dh = dst ? dst->texture.d.height : 0;
    if (!why) {
        float *v = state.viewports.v[0];
        if (!(v[0] == 0 && v[1] == 0 && (v[2] <= 0 || v[2] == (float)dw) && (v[3] <= 0 || v[3] == (float)dh))) why = 4;
    }
    float k[20] = {0}; uint32_t ow = 0, oh = 0;
    if (!why) {
        const p_bind_buf *u = &state.ubo[1][0];
        if (!u->addr || u->size < sizeof k || uniform_read_current((uintptr_t)u->addr, sizeof k)) why = 5;
        else { memcpy(k, uniform_read_scratch, sizeof k); memcpy(&ow, &k[16], 4); memcpy(&oh, &k[17], 4);
            double cw = (double)k[0] * ow, ch = (double)k[1] * oh;
            if (ow != dw || oh != dh || fabs(cw - src->texture.d.width) > 0.5 || fabs(ch - src->texture.d.height) > 0.5) why = 6; }
    }
    static int told[7]; if (!told[why]++)
        LOG("[asr] the game's upscale %.8s: %s (scene %ux%u -> canvas %ux%u, constants %g %g, output %ux%u)", fs->game_id,
            why == 0 ? "ASR reconstructs the canvas" : why == 6 ? "the scene is a smaller region (dynamic resolution): the game's own" : "not replaceable here: the game's own",
            src ? src->texture.d.width : 0, src ? src->texture.d.height : 0, dw, dh, k[0], k[1], ow, oh);
    if (why) return 0;
    image_entry *out = asr_run(src, dw, dh);
    if (!out) return 0;
    image_access(out, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_access(dst, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}}};
    vkCmdBlitImage(command, out->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
    image_gpu_written(dst);
    /* anti-aliased: no SMAA on the canvas this frame */
    smaa_scene_mark(1);
    return 1;
}
