/* Present-time upscaling (TOTK_UPSCALER=fsr1 | bilinear; default fsr1). In handheld mode the game's finished picture is
   1280x720 (UI included) and the present stretched it to the 1920x1080 panel with a linear blit. With FSR 1 the stretch
   is EASU (edge-adaptive upsampling) into a display-size image, then RCAS sharpening (TOTK_FSR_SHARPNESS, in stops: 0 is
   the strongest, default 0.25), then a same-size copy into the swapchain image. Shaders: shaders/fsr1_*.comp (AMD
   FidelityFX FSR 1, MIT, third_party/fsr1; tools/gen_shaders.sh). The picture is gamma-encoded RGBA8, as EASU expects. */
#include <math.h>
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t w, h; } upscale_image;
static struct {
    int mode;   /* -1 unread, 0 bilinear blit, 1 FSR 1 */
    float sharpness;
    VkDescriptorSetLayout set_layout; VkPipelineLayout layout; VkPipeline easu, rcas; VkSampler sampler;
    upscale_image mid, out;
    uint64_t frames, fallbacks;
} upscale = {.mode = -1};

static void upscale_image_make(upscale_image *i, uint32_t w, uint32_t h) {
    if (i->image && i->w == w && i->h == h) return;
    if (i->image) { vkDeviceWaitIdle(device); vkDestroyImageView(device, i->view, NULL); vkDestroyImage(device, i->image, NULL); vkFreeMemory(device, i->memory, NULL); }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(device, &ci, NULL, &i->image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, i->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < memory_properties.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) && (memory_properties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    if (type == UINT32_MAX) die("No Vulkan memory for the upscaler");
    VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VK_CHECK(vkAllocateMemory(device, &mi, NULL, &i->memory)); VK_CHECK(vkBindImageMemory(device, i->image, i->memory, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(device, &vi, NULL, &i->view));
    i->w = w; i->h = h;
}

static VkPipeline upscale_pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = upscale.layout};
    VkPipeline pipeline; VK_CHECK(pipeline_create_compute(&pi, &pipeline)); vkDestroyShaderModule(device, module, NULL);
    return pipeline;
}

static int upscale_ready(void) {
    if (upscale.mode >= 0) return upscale.mode;
    const char *e = getenv("TOTK_UPSCALER"), *s = getenv("TOTK_FSR_SHARPNESS");
    upscale.mode = !(e && !strcmp(e, "bilinear"));
    upscale.sharpness = s ? (float)atof(s) : 0.25f;
    if (upscale.mode) {
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
        upscale.easu = upscale_pipeline(easu, sizeof easu); upscale.rcas = upscale_pipeline(rcas, sizeof rcas);
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(device, &si, NULL, &upscale.sampler));
    }
    LOG("[upscale] present upscaler: %s (sharpness %.2f stops)", upscale.mode ? "FSR 1 (EASU + RCAS)" : "bilinear blit", upscale.sharpness);
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

/* Replaces the present blit of the picture (the sw x sh region at the origin of e) into dst's (x, y, dw, dh) rect when it is
   an enlargement. Returns 0 to let the caller blit as before. */
static int upscale_present(image_entry *e, int sx, int sy, int sw, int sh, VkImage dst, int x, int y, int dw, int dh) {
    if (!upscale_ready() || sx || sy || sw <= 0 || sh <= 0 || (sw >= dw && sh >= dh) || e->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { upscale.fallbacks++; return 0; }
    graphics_pass_end();
    upscale_image_make(&upscale.mid, (uint32_t)dw, (uint32_t)dh); upscale_image_make(&upscale.out, (uint32_t)dw, (uint32_t)dh);
    image_access(e, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
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
    upscale_pass(upscale.easu, attachment_view(e, NULL, e->format), upscale.mid.view, con, (uint32_t)dw, (uint32_t)dh);
    VkImageMemoryBarrier mid = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = upscale.mid.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &mid);

    /* RCAS constants (FsrRcasCon): sharpness in stops -> linear */
    uint32_t rcon[16] = {0}; rcon[0] = upscale_float_bits(exp2f(-upscale.sharpness));
    { _Float16 h = (_Float16)exp2f(-upscale.sharpness); uint16_t hb; memcpy(&hb, &h, 2); rcon[1] = (uint32_t)hb | ((uint32_t)hb << 16); }
    upscale_pass(upscale.rcas, upscale.mid.view, upscale.out.view, rcon, (uint32_t)dw, (uint32_t)dh);
    VkImageMemoryBarrier done = mid; done.image = upscale.out.image; done.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &done);

    VkImageBlit copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {dw, dh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x, y, 0}, {x + dw, y + dh, 1}}};
    vkCmdBlitImage(command, upscale.out.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, VK_FILTER_NEAREST);
    upscale.frames++;
    return 1;
}
