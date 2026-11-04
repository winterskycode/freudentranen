/* Bug-report recording (host/recorder.c, TOTK_RECORD): each presented picture, at most RECORDER_FPS a second, scaled
   to RECORDER_W x RECORDER_H and converted to NV12 by shaders/record_nv12.comp into one of REC_SLOTS host-visible
   buffers, in the present's own command buffer, before the menu or HUD is drawn on the screen image. A feeding thread
   waits for that submission and hands the buffer to the encoder; with every buffer still waiting, the picture is
   dropped (the game never waits for the recorder). */
#include "recorder.h"
#define REC_SLOTS 4
static struct {
    VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSetLayout set_layout; VkSampler linear;
    vk_buffer buf[REC_SLOTS]; uint64_t ticket[REC_SLOTS], pts[REC_SLOTS]; int state[REC_SLOTS];   /* 0 free, 1 recorded, 2 submitted */
    int pending; uint64_t last_ns; int thread;
    pthread_mutex_t lock; pthread_cond_t cond;
} rec = {.pending = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void *rec_feed(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&rec.lock);
        int slot = -1;
        for (;;) {
            for (int i = 0; i < REC_SLOTS; i++) if (rec.state[i] == 2 && (slot < 0 || rec.pts[i] < rec.pts[slot])) slot = i;
            if (slot >= 0) break;
            pthread_cond_wait(&rec.cond, &rec.lock);
        }
        uint64_t ticket = rec.ticket[slot], pts = rec.pts[slot];
        pthread_mutex_unlock(&rec.lock);
        mtl_wait(ticket, UINT64_MAX);
        recorder_frame(rec.buf[slot].mapped, pts);
        pthread_mutex_lock(&rec.lock); rec.state[slot] = 0; pthread_mutex_unlock(&rec.lock);
    }
    return NULL;
}

/* The picture (the sx, sy, sw, sh rectangle of e) into a free buffer, recorded into the current command buffer;
   rec_submitted gives it the submission's ticket. */
static void rec_picture(image_entry *e, int sx, int sy, int sw, int sh) {
    if (!recorder_enabled() || sw <= 0 || sh <= 0) return;
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC);
    if (rec.last_ns && now - rec.last_ns < 1000000000ull / RECORDER_FPS - 2000000ull) return;
    if (e->format != VK_FORMAT_R8G8B8A8_UNORM && e->format != VK_FORMAT_R8G8B8A8_SRGB && e->format != VK_FORMAT_B8G8R8A8_UNORM && e->format != VK_FORMAT_B8G8R8A8_SRGB) return;
    pthread_mutex_lock(&rec.lock);
    int slot = -1; for (int i = 0; i < REC_SLOTS && slot < 0; i++) if (!rec.state[i]) slot = i;
    if (slot >= 0) rec.state[slot] = 1;
    pthread_mutex_unlock(&rec.lock);
    if (slot < 0) return;
    if (!rec.pipeline) {
        VkDescriptorSetLayoutBinding b[2] = {
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, NULL, &rec.set_layout));
        VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 28};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &rec.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
        VK_CHECK(vkCreatePipelineLayout(device, &pl, NULL, &rec.layout));
        static const uint32_t code[] = {
#include "record_nv12_spv.inc"
        };
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof code, .pCode = code};
        VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
        VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = rec.layout};
        VK_CHECK(pipeline_create_compute(&pi, &rec.pipeline)); vkDestroyShaderModule(device, module, NULL);
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(device, &si, NULL, &rec.linear));
        for (int i = 0; i < REC_SLOTS; i++) rec.buf[i] = buffer_create((uint64_t)RECORDER_W * RECORDER_H * 3 / 2);
    }
    if (!rec.thread) { pthread_t t; if (!pthread_create(&t, NULL, rec_feed, NULL)) { pthread_detach(t); rec.thread = 1; } }
    image_access(e, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkDescriptorImageInfo img = {rec.linear, attachment_view(e, NULL, e->format), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo out = {rec.buf[slot].buffer, 0, (VkDeviceSize)RECORDER_W * RECORDER_H * 3 / 2};
    VkDescriptorSet set = descriptor_allocate(rec.set_layout);
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &img},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &out}};
    vkUpdateDescriptorSets(device, 2, wr, 0, NULL);
    struct { float ox, oy, ew, eh; int32_t w, h, srgb; } push = {(float)sx, (float)sy, (float)sw, (float)sh, RECORDER_W, RECORDER_H,
        e->format == VK_FORMAT_R8G8B8A8_SRGB || e->format == VK_FORMAT_B8G8R8A8_SRGB};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, rec.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, rec.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(command, rec.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(command, (RECORDER_W / 4 + 7) / 8, (RECORDER_H / 2 + 7) / 8, 1);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    rec.pts[slot] = now; rec.pending = slot; rec.last_ns = now;
}
/* After the present's submission: its ticket for the buffer recorded into it. */
static void rec_submitted(uint64_t ticket) {
    if (rec.pending < 0) return;
    pthread_mutex_lock(&rec.lock);
    rec.ticket[rec.pending] = ticket; rec.state[rec.pending] = 2; rec.pending = -1;
    pthread_cond_signal(&rec.cond); pthread_mutex_unlock(&rec.lock);
}
