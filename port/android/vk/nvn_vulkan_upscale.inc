/* Present-time upscaling (TOTK_UPSCALER=fsr1 | sgsr | mmpx | bilinear; default fsr1). In handheld mode the game's
   finished picture is 1280x720 (UI included) and the present stretches it to the 1920x1080 panel with a linear blit.
   With FSR 1 the stretch is EASU (edge-adaptive upsampling) into a display-size image, then RCAS sharpening
   (TOTK_FSR_SHARPNESS, in stops: 0 is the strongest, default 0.25; off: none), then a same-size copy into the swapchain
   image. SGSR 1 upscales and sharpens in one pass into the display-size image. MMPX doubles the picture (2x2 per pixel,
   edges of equal colours reshaped), then a linear blit scales that to the screen. Shaders: shaders/fsr1_*.comp (AMD
   FidelityFX FSR 1, MIT, third_party/fsr1), sgsr1.comp (Qualcomm Snapdragon GSR 1, BSD-3-Clause, third_party/sgsr),
   mmpx.comp (McGuire & Gagiu, MIT, third_party/mmpx); tools/gen_shaders.sh. The picture is gamma-encoded RGBA8, as
   they expect. */
#include <math.h>
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t w, h; VkFormat format; } upscale_image;
/* The choices (TOTK_UPSCALER values), by mode number */
enum { UP_BILINEAR, UP_FSR1, UP_SGSR, UP_MMPX, UP_NEAREST, UP_BICUBIC, UP_LANCZOS, UP_CAS, UP_NIS, UP_SUPERXBR, UP_SCALEFX, UP_MODES };
static const char *const upscale_values[UP_MODES] = {"bilinear", "fsr1", "sgsr", "mmpx", "nearest", "bicubic", "lanczos", "cas", "nis", "superxbr", "scalefx"};
static const char *const upscale_names[UP_MODES] = {"bilinear blit", "FSR 1", "SGSR 1", "MMPX (2x, then a linear blit)", "nearest blit",
    "bicubic (Catmull-Rom)", "Lanczos-3", "AMD CAS", "NVIDIA Image Scaling", "Super-xBR (2x, then a linear blit)", "ScaleFX"};
/* The multi-pass choices (shaders/superxbr_p*.comp, scalefx_p*.comp): each pass's output size as a multiple of the
   picture's and its inputs at bindings 0-3 (-1 none, 0 the picture, n pass n-1's output); RGBA16F between passes */
#define UP_MP_PASSES 5
typedef struct { int scale, in[4]; } upscale_mp_pass;
static const struct { int n; upscale_mp_pass pass[UP_MP_PASSES]; } upscale_mp[2] = {
    {3, {{1, {0, -1, -1, -1}}, {2, {1, 0, -1, -1}}, {2, {2, -1, -1, -1}}}},
    {5, {{1, {0, -1, -1, -1}}, {1, {1, -1, -1, -1}}, {1, {2, 1, -1, -1}}, {1, {3, -1, -1, -1}}, {3, {4, 0, -1, -1}}}}};
static struct {
    /* -1 unread, else UP_* */
    int mode;
    float sharpness;
    /* TOTK_SCALEFX_RES: ScaleFX's output, 0 the screen's size, else that multiple of the picture's (3: its own 3x) */
    int sfx_res;
    /* pipe: the one-pass choices on the shared layout (a sampled source, a storage output, 64 bytes of constants) */
    VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline easu, rcas, pipe[UP_MODES]; VkSampler sampler;
    /* NVIDIA Image Scaling: its own layout (constants, sampler, source, output, two coefficient tables) */
    VkDescriptorSetLayout nis_set_layout; VkPipelineLayout nis_layout; VkPipeline nis; upscale_image nis_coef[2]; int nis_uploaded;
    /* the multi-pass choices: their layout (four sampled inputs, a storage output), pipelines, the picture copied to its
       own size (orig) and the passes' outputs */
    VkDescriptorSetLayout mp_set_layout; VkPipelineLayout mp_layout; VkPipeline mp_pipe[2][UP_MP_PASSES]; VkSampler point;
    upscale_image orig, mp_img[UP_MP_PASSES];
    /* big: MMPX's 2x picture */
    upscale_image mid, out, big;
    uint64_t frames, fallbacks;
} upscale = {.mode = -1};

static void upscale_image_make_as(upscale_image *i, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    if (i->image && i->w == w && i->h == h && i->format == format) return;
    if (i->image) { vkDeviceWaitIdle(device); vkDestroyImageView(device, i->view, NULL); vkDestroyImage(device, i->image, NULL); vkFreeMemory(device, i->memory, NULL); }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(device, &ci, NULL, &i->image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, i->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < memory_properties.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) && (memory_properties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    if (type == UINT32_MAX) die("No Vulkan memory for the upscaler");
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VK_CHECK(vkAllocateMemory(device, &mi, NULL, &i->memory)); VK_CHECK(vkBindImageMemory(device, i->image, i->memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(device, &vi, NULL, &i->view));
    i->w = w; i->h = h; i->format = format;
}
static void upscale_image_make(upscale_image *i, uint32_t w, uint32_t h) {
    upscale_image_make_as(i, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static VkPipeline upscale_pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = upscale.layout};
    VkPipeline pipeline; VK_CHECK(pipeline_create_compute(&pi, &pipeline)); vkDestroyShaderModule(device, module, NULL);
    return pipeline;
}

/* TOTK_FSR_SHARPNESS: RCAS's strength in stops below its strongest (0), or "off": EASU alone, no sharpening (-1) */
static float upscale_sharpness(const char *s) { return !s ? 0.25f : !strcmp(s, "off") ? -1.0f : (float)atof(s); }
static int upscale_sfx_res(const char *s) { return s && !strcmp(s, "3x") ? 3 : s && !strcmp(s, "2x") ? 2 : 0; }
static int upscale_mode_of(const char *v) { for (int m = 0; v && m < UP_MODES; m++) if (!strcmp(v, upscale_values[m])) return m; return UP_FSR1; }
static void upscale_nis_ready(void); static void upscale_mp_ready(void);
static int upscale_ready(void) {
    if (upscale.mode >= 0) return upscale.mode;
    const char *e = getenv("TOTK_UPSCALER"), *s = getenv("TOTK_FSR_SHARPNESS");
    upscale.mode = upscale_mode_of(e);
    upscale.sharpness = upscale_sharpness(s);
    upscale.sfx_res = upscale_sfx_res(getenv("TOTK_SCALEFX_RES"));
    /* all of them, so the menu can switch while playing */
    {
        VkDescriptorSetLayoutBinding b[2] = {
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &upscale.set_layout));
        VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 64};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &upscale.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
        VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &upscale.layout));
        static const uint32_t easu[] = {
#include "fsr1_easu_spv.inc"
        };
        static const uint32_t rcas[] = {
#include "fsr1_rcas_spv.inc"
        };
        static const uint32_t sgsr[] = {
#include "sgsr1_spv.inc"
        }, mmpx[] = {
#include "mmpx_spv.inc"
        }, bicubic[] = {
#include "bicubic_spv.inc"
        }, lanczos[] = {
#include "lanczos3_spv.inc"
        }, cas[] = {
#include "cas_spv.inc"
        };
        upscale.easu = upscale_pipeline(easu, sizeof easu); upscale.rcas = upscale_pipeline(rcas, sizeof rcas);
        upscale.pipe[UP_SGSR] = upscale_pipeline(sgsr, sizeof sgsr); upscale.pipe[UP_MMPX] = upscale_pipeline(mmpx, sizeof mmpx);
        upscale.pipe[UP_BICUBIC] = upscale_pipeline(bicubic, sizeof bicubic); upscale.pipe[UP_LANCZOS] = upscale_pipeline(lanczos, sizeof lanczos);
        upscale.pipe[UP_CAS] = upscale_pipeline(cas, sizeof cas);
        upscale_nis_ready();
        upscale_mp_ready();
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(device, &si, NULL, &upscale.sampler));
    }
    if (upscale.mode == UP_SCALEFX) LOG("[upscale] present upscaler: ScaleFX, output %s", upscale.sfx_res ? (upscale.sfx_res == 3 ? "3x" : "2x") : "screen size");
    else if (upscale.mode != UP_FSR1) LOG("[upscale] present upscaler: %s", upscale_names[upscale.mode]);
    else if (upscale.sharpness < 0) LOG("[upscale] present upscaler: FSR 1 (EASU, no sharpening)");
    else LOG("[upscale] present upscaler: FSR 1 (EASU + RCAS, sharpness %.2f stops)", upscale.sharpness);
    return upscale.mode;
}

static uint32_t upscale_float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void upscale_pass(VkPipeline pipeline, VkImageView src, VkImageView dst, const uint32_t con[16], uint32_t w, uint32_t h) {
    VkDescriptorSet set = descriptor_allocate(upscale.set_layout);
    VkDescriptorImageInfo in = {.sampler = upscale.sampler, .imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL}, out = {.imageView = dst, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w2[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &in},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &out}};
    vkUpdateDescriptorSets(device, 2, w2, 0, NULL);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, upscale.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, upscale.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, con);
    vkCmdDispatch(command, (w + 15) / 16, (h + 15) / 16, 1);
}

/* NVIDIA Image Scaling's coefficient tables as 2x64 RGBA32F textures (8 taps a phase), uploaded at the first use */
#include "nis_coef.inc"
static void upscale_nis_ready(void) {
    VkDescriptorSetLayoutBinding b[6];
    const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    for (uint32_t i = 0; i < 6; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = types[i], .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &upscale.nis_set_layout));
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &upscale.nis_set_layout};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &upscale.nis_layout));
    static const uint32_t nis[] = {
#include "nis_spv.inc"
    };
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof nis, .pCode = nis};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = upscale.nis_layout};
    VK_CHECK(pipeline_create_compute(&pi, &upscale.nis)); vkDestroyShaderModule(device, module, NULL);
    for (int i = 0; i < 2; i++) upscale_image_make_as(&upscale.nis_coef[i], 2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}
static void upscale_nis_upload(void) {
    if (upscale.nis_uploaded) return;
    vk_buffer b = buffer_create(2 * sizeof nis_coef_scale);
    memcpy(b.mapped, nis_coef_scale, sizeof nis_coef_scale); memcpy((char *)b.mapped + sizeof nis_coef_scale, nis_coef_usm, sizeof nis_coef_usm);
    for (int i = 0; i < 2; i++) {
        VkImageMemoryBarrier t = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = upscale.nis_coef[i].image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &t);
        VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)i * sizeof nis_coef_scale, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {2, 64, 1}};
        vkCmdCopyBufferToImage(command, b.buffer, upscale.nis_coef[i].image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        t.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; t.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; t.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &t);
    }
    /* the staging buffer stays: 4 KiB, once */
    upscale.nis_uploaded = 1;
}
/* NVScalerUpdateConfig (NIS_Config.h) for SDR input, sharpness 0.5: the scaler's constants */
static void upscale_nis_config(float *c, uint32_t *u, int sw, int sh, int iw, int ih, int dw, int dh) {
    float sharp = 0.5f - 0.5f, max_scale = sharp >= 0 ? 1.25f : 1.75f, min_scale = sharp >= 0 ? 1.25f : 1.0f, limit_scale = sharp >= 0 ? 1.25f : 1.0f;
    float detect_ratio = 2 * 1127.f / 1024.f, detect_thres = 64.0f / 1024.0f, min_contrast = 2.0f, max_contrast = 10.0f, start_y = 0.45f, end_y = 0.9f;
    float strength_min = fmaxf(0.0f, 0.4f + sharp * min_scale * 1.2f), strength_max = 1.6f + sharp * max_scale * 1.8f;
    float limit_min = fmaxf(0.1f, 0.14f + sharp * limit_scale * 0.32f), limit_max = 0.5f + sharp * limit_scale * 0.6f;
    c[0] = detect_ratio; c[1] = detect_thres; c[2] = min_contrast; c[3] = 1.0f / (max_contrast - min_contrast);
    c[4] = 1.0f; c[5] = 1.0f / 255.0f; c[6] = start_y; c[7] = 1.0f / (end_y - start_y);
    c[8] = strength_min; c[9] = strength_max - strength_min; c[10] = limit_min; c[11] = limit_max - limit_min;
    c[12] = (float)sw / (float)dw; c[13] = (float)sh / (float)dh; c[14] = 1.0f / (float)dw; c[15] = 1.0f / (float)dh; c[16] = 1.0f / (float)iw; c[17] = 1.0f / (float)ih;
    u[18] = 0; u[19] = 0; u[20] = (uint32_t)sw; u[21] = (uint32_t)sh; u[22] = 0; u[23] = 0; u[24] = (uint32_t)dw; u[25] = (uint32_t)dh; c[26] = c[27] = 0;
}
static VkPipeline upscale_pipeline_on(const uint32_t *code, size_t bytes, VkPipelineLayout layout) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
    VkPipeline pipeline; VK_CHECK(pipeline_create_compute(&pi, &pipeline)); vkDestroyShaderModule(device, module, NULL);
    return pipeline;
}
static void upscale_mp_ready(void) {
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++) b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &upscale.mp_set_layout));
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 64};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &upscale.mp_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &upscale.mp_layout));
    static const uint32_t x0[] = {
#include "superxbr_p0_spv.inc"
    }, x1[] = {
#include "superxbr_p1_spv.inc"
    }, x2[] = {
#include "superxbr_p2_spv.inc"
    }, f0[] = {
#include "scalefx_p0_spv.inc"
    }, f1[] = {
#include "scalefx_p1_spv.inc"
    }, f2[] = {
#include "scalefx_p2_spv.inc"
    }, f3[] = {
#include "scalefx_p3_spv.inc"
    }, f4[] = {
#include "scalefx_p4_spv.inc"
    };
    const uint32_t *code[2][UP_MP_PASSES] = {{x0, x1, x2}, {f0, f1, f2, f3, f4}};
    size_t size[2][UP_MP_PASSES] = {{sizeof x0, sizeof x1, sizeof x2}, {sizeof f0, sizeof f1, sizeof f2, sizeof f3, sizeof f4}};
    for (int m = 0; m < 2; m++) for (int p = 0; p < upscale_mp[m].n; p++) upscale.mp_pipe[m][p] = upscale_pipeline_on(code[m][p], size[m][p], upscale.mp_layout);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK_CHECK(vkCreateSampler(device, &si, NULL, &upscale.point));
}
/* Super-xBR or ScaleFX: the picture copied to an image of its own size, the passes, then a linear blit of the last
   (Super-xBR 2x; ScaleFX the screen's size, 2x or 3x) into dst's rect */
static int upscale_present_mp(image_entry *e, int sw, int sh, VkImage dst, int x, int y, int dw, int dh) {
    int m = upscale.mode == UP_SCALEFX;
    upscale_image_make_as(&upscale.orig, (uint32_t)sw, (uint32_t)sh, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = upscale.orig.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
    VkImageBlit scale = {.srcSubresource = copy.srcSubresource, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}}, .dstSubresource = copy.dstSubresource, .dstOffsets = {{0, 0, 0}, {sw, sh, 1}}};
    /* a plain copy keeps the bytes; a blit only when the formats differ in kind */
    if (e->format == VK_FORMAT_R8G8B8A8_UNORM || e->format == VK_FORMAT_R8G8B8A8_SRGB) vkCmdCopyImage(command, e->image, VK_IMAGE_LAYOUT_GENERAL, upscale.orig.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    else vkCmdBlitImage(command, e->image, VK_IMAGE_LAYOUT_GENERAL, upscale.orig.image, VK_IMAGE_LAYOUT_GENERAL, 1, &scale, VK_FILTER_NEAREST);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    int n = upscale_mp[m].n;
    for (int p = 0; p < n; p++) {
        const upscale_mp_pass *ps = &upscale_mp[m].pass[p];
        upscale_image *o = &upscale.mp_img[p];
        uint32_t ow = (uint32_t)(sw * ps->scale), oh = (uint32_t)(sh * ps->scale);
        /* ScaleFX's last pass filters its 3x subpixels to any size: the screen's spares writing the 3x picture */
        if (m && p == n - 1) { ow = upscale.sfx_res ? (uint32_t)(sw * upscale.sfx_res) : (uint32_t)dw; oh = upscale.sfx_res ? (uint32_t)(sh * upscale.sfx_res) : (uint32_t)dh; }
        upscale_image_make_as(o, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        b.image = o->image; b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkDescriptorSet set = descriptor_allocate(upscale.mp_set_layout);
        VkDescriptorImageInfo info[5]; VkWriteDescriptorSet w[5]; const upscale_image *src0 = &upscale.orig;
        for (int i = 0; i < 4; i++) {
            const upscale_image *in = ps->in[i] > 0 ? &upscale.mp_img[ps->in[i] - 1] : &upscale.orig;
            if (i == 0) src0 = in;
            info[i] = (VkDescriptorImageInfo){upscale.point, in->view, VK_IMAGE_LAYOUT_GENERAL};
        }
        info[4] = (VkDescriptorImageInfo){.imageView = o->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        for (int i = 0; i < 5; i++) w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1,
            .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &info[i]};
        vkUpdateDescriptorSets(device, 5, w, 0, NULL);
        float k[16] = {(float)src0->w, (float)src0->h, 1.0f / src0->w, 1.0f / src0->h, (float)sw, (float)sh, 1.0f / sw, 1.0f / sh,
            (float)ow, (float)oh, 1.0f / ow, 1.0f / oh, 0, 0, 0, 0};
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, upscale.mp_pipe[m][p]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, upscale.mp_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(command, upscale.mp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof k, k);
        vkCmdDispatch(command, (ow + 15) / 16, (oh + 15) / 16, 1);
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = p + 1 < n ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, p + 1 < n ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    const upscale_image *last = &upscale.mp_img[n - 1];
    VkImageBlit out = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)last->w, (int32_t)last->h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x, y, 0}, {x + dw, y + dh, 1}}};
    vkCmdBlitImage(command, last->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &out, VK_FILTER_LINEAR);
    upscale.frames++;
    return 1;
}
/* The other choices into the display-size image (MMPX: into the 2x one), then into dst's rect */
static int upscale_present_other(image_entry *e, int sw, int sh, VkImage dst, int x, int y, int dw, int dh) {
    int mode = upscale.mode, mmpx = mode == UP_MMPX;
    if (mode == UP_SUPERXBR || mode == UP_SCALEFX) return upscale_present_mp(e, sw, sh, dst, x, y, dw, dh);
    VkImageView src = attachment_view(e, NULL, e->format);
    if (mode == UP_NEAREST) {
        VkImageBlit copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x, y, 0}, {x + dw, y + dh, 1}}};
        image_access(e, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdBlitImage(command, e->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, VK_FILTER_NEAREST);
        upscale.frames++;
        return 1;
    }
    upscale_image *o = mmpx ? &upscale.big : &upscale.out;
    if (mmpx) upscale_image_make(&upscale.big, (uint32_t)sw * 2, (uint32_t)sh * 2);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = o->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    float iw = (float)e->texture.d.width, ih = (float)e->texture.d.height;
    uint32_t con[16] = {0};
    if (mode == UP_NIS) {
        upscale_nis_upload();
        uint8_t *mapped; VkDescriptorBufferInfo ub = uniform_allocate_overwrite(128, &mapped);
        float c[32] = {0}; upscale_nis_config(c, (uint32_t *)c, sw, sh, (int)iw, (int)ih, dw, dh); memcpy(mapped, c, 128);
        VkDescriptorSet set = descriptor_allocate(upscale.nis_set_layout);
        VkDescriptorImageInfo smp = {.sampler = upscale.sampler}, in = {.imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL}, out = {.imageView = o->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
            c0 = {.imageView = upscale.nis_coef[0].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL}, c1 = {.imageView = upscale.nis_coef[1].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[6] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ub},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &smp},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &in},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &out},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &c0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &c1}};
        vkUpdateDescriptorSets(device, 6, w, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, upscale.nis);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, upscale.nis_layout, 0, 1, &set, 0, NULL);
        vkCmdDispatch(command, ((uint32_t)dw + 31) / 32, ((uint32_t)dh + 23) / 24, 1);
    } else if (mode == UP_CAS) {
        /* CasSetup (ffx_cas.h), sharpness 0.5 */
        float sharp = -1.0f / (8.0f + (5.0f - 8.0f) * 0.5f); _Float16 hs = (_Float16)sharp; uint16_t hb; memcpy(&hb, &hs, 2);
        con[0] = upscale_float_bits((float)sw / dw); con[1] = upscale_float_bits((float)sh / dh);
        con[2] = upscale_float_bits(0.5f * sw / dw - 0.5f); con[3] = upscale_float_bits(0.5f * sh / dh - 0.5f);
        con[4] = upscale_float_bits(sharp); con[5] = hb; con[6] = upscale_float_bits(8.0f * sw / dw); con[7] = 0;
        upscale_pass(upscale.pipe[UP_CAS], src, o->view, con, (uint32_t)dw, (uint32_t)dh);
    } else if (mmpx) {
        int32_t size[4] = {sw, sh, 0, 0}; memcpy(con, size, sizeof size);
        upscale_pass(upscale.pipe[UP_MMPX], src, o->view, con, (uint32_t)sw, (uint32_t)sh);
    } else if (mode == UP_SGSR) {
        float v[8] = {1.0f / iw, 1.0f / ih, iw, ih, (float)sw / iw, (float)sh / ih, (float)dw, (float)dh}; memcpy(con, v, sizeof v);
        upscale_pass(upscale.pipe[UP_SGSR], src, o->view, con, (uint32_t)dw, (uint32_t)dh);
    } else {
        float v[4] = {(float)sw, (float)sh, (float)dw, (float)dh}; memcpy(con, v, sizeof v);
        upscale_pass(upscale.pipe[mode], src, o->view, con, (uint32_t)dw, (uint32_t)dh);
    }
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    int32_t ow = mmpx ? sw * 2 : dw, oh = mmpx ? sh * 2 : dh;
    VkImageBlit copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {ow, oh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x, y, 0}, {x + dw, y + dh, 1}}};
    vkCmdBlitImage(command, o->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, mmpx ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    upscale.frames++;
    return 1;
}

/* Replaces the present blit of the picture (the sw x sh region at the origin of e) into dst's (x, y, dw, dh) rect when it is
   an enlargement. Returns 0 to let the caller blit. */
static int upscale_present(image_entry *e, int sx, int sy, int sw, int sh, VkImage dst, int x, int y, int dw, int dh) {
    /* the live word upscaler=<value> (<files>/exp.live) switches as the menu does, for side-by-side checks */
    { const char *w = live_exp[0] ? exp_word(live_exp, "upscaler=", 0) : NULL; char v[16] = ""; int n = 0;
      while (w && n < 15 && exp_word_char(w[n])) { v[n] = w[n]; n++; }
      if (n && upscale_ready() >= 0) { int m = upscale_mode_of(v); if (m != upscale.mode) { upscale.mode = m; LOG("[upscale] present upscaler: %s (live)", upscale_names[m]); } } }
    if (!upscale_ready() || sx || sy || sw <= 0 || sh <= 0 || (sw >= dw && sh >= dh) || e->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { upscale.fallbacks++; return 0; }
    graphics_pass_end();
    upscale_image_make(&upscale.mid, (uint32_t)dw, (uint32_t)dh); upscale_image_make(&upscale.out, (uint32_t)dw, (uint32_t)dh);
    image_access(e, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (upscale.mode != UP_FSR1) return upscale_present_other(e, sw, sh, dst, x, y, dw, dh);
    VkImageMemoryBarrier start[2];
    for (int i = 0; i < 2; i++) start[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = i ? upscale.out.image : upscale.mid.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, start);

    /* EASU constants (FsrEasuCon): viewport sw x sh inside the e texture, output dw x dh */
    float iw = (float)e->texture.d.width, ih = (float)e->texture.d.height, vw = (float)sw, vh = (float)sh, ow = (float)dw, oh = (float)dh;
    uint32_t con[16] = {
        upscale_float_bits(vw / ow), upscale_float_bits(vh / oh), upscale_float_bits(0.5f * vw / ow - 0.5f), upscale_float_bits(0.5f * vh / oh - 0.5f),
        upscale_float_bits(1.0f / iw), upscale_float_bits(1.0f / ih), upscale_float_bits(1.0f / iw), upscale_float_bits(-1.0f / ih),
        upscale_float_bits(-1.0f / iw), upscale_float_bits(2.0f / ih), upscale_float_bits(1.0f / iw), upscale_float_bits(2.0f / ih),
        upscale_float_bits(0.0f), upscale_float_bits(4.0f / ih), 0, 0};
    /* without sharpening EASU writes the output itself */
    int sharpen = upscale.sharpness >= 0;
    upscale_pass(upscale.easu, attachment_view(e, NULL, e->format), sharpen ? upscale.mid.view : upscale.out.view, con, (uint32_t)dw, (uint32_t)dh);
    VkImageMemoryBarrier mid = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = upscale.mid.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (sharpen) {
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &mid);
        /* RCAS constants (FsrRcasCon): sharpness in stops -> linear */
        uint32_t rcon[16] = {0}; rcon[0] = upscale_float_bits(exp2f(-upscale.sharpness));
        { _Float16 h = (_Float16)exp2f(-upscale.sharpness); uint16_t hb; memcpy(&hb, &h, 2); rcon[1] = (uint32_t)hb | ((uint32_t)hb << 16); }
        upscale_pass(upscale.rcas, upscale.mid.view, upscale.out.view, rcon, (uint32_t)dw, (uint32_t)dh);
    }
    VkImageMemoryBarrier done = mid; done.image = upscale.out.image; done.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &done);

    VkImageBlit copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {dw, dh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x, y, 0}, {x + dw, y + dh, 1}}};
    vkCmdBlitImage(command, upscale.out.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, VK_FILTER_NEAREST);
    upscale.frames++;
    return 1;
}
