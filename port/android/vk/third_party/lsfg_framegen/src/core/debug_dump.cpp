// TOTK: debugging aid, not part of lsfg-vk. Every image the library creates is registered; lsfg_debug_dump_images()
// writes the contents of all live ones to <dir>/<index>_<width>x<height>_<format>.raw (index = creation order) so two
// runs can be compared stage by stage (tools/lsfg_test, LSFG_TEST_DUMP).
#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/image.hpp"
#include "core/device.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace {
struct Entry {
    std::weak_ptr<VkImage> image;
    std::shared_ptr<VkImageLayout> layout;
    VkExtent2D extent;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    uint32_t family;
};
std::mutex g_lock;
std::vector<Entry> g_entries;

uint32_t texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}
}

namespace LSFG::Core {
void debugRegisterImage(const Device& device, const std::shared_ptr<VkImage>& image,
        const std::shared_ptr<VkImageLayout>& layout, VkExtent2D extent, VkFormat format, VkImageAspectFlags aspect) {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_entries.push_back({image, layout, extent, format, aspect, device.handle(), device.getPhysicalDevice(),
        device.getComputeQueue(), device.getComputeFamilyIdx()});
}
}

extern "C" int lsfg_debug_dump_images(const char *dir) {
    const std::lock_guard<std::mutex> guard(g_lock);
    int written = 0;
    for (size_t index = 0; index < g_entries.size(); index++) {
        Entry &e = g_entries[index];
        auto image = e.image.lock();
        const uint32_t bpp = texel_bytes(e.format);
        if (!image || !bpp) continue;
        const VkDeviceSize size = (VkDeviceSize)e.extent.width * e.extent.height * bpp;
        vkQueueWaitIdle(e.queue);

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer; if (vkCreateBuffer(e.device, &bi, nullptr, &buffer)) continue;
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(e.device, buffer, &req);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(e.physical, &mp);
        uint32_t type = UINT32_MAX; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) type = t;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
        VkDeviceMemory memory; if (type == UINT32_MAX || vkAllocateMemory(e.device, &ai, nullptr, &memory)) { vkDestroyBuffer(e.device, buffer, nullptr); continue; }
        vkBindBufferMemory(e.device, buffer, memory, 0);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = e.family;
        VkCommandPool pool; vkCreateCommandPool(e.device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb; vkAllocateCommandBuffers(e.device, &cai, &cb);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb, &cbi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = *e.layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = *image; b.subresourceRange = {e.aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        *e.layout = VK_IMAGE_LAYOUT_GENERAL;
        VkBufferImageCopy c{}; c.imageSubresource = {e.aspect, 0, 0, 1}; c.imageExtent = {e.extent.width, e.extent.height, 1};
        vkCmdCopyImageToBuffer(cb, *image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &c);
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        vkQueueSubmit(e.queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(e.queue);

        void *mapped = nullptr;
        if (!vkMapMemory(e.device, memory, 0, size, 0, &mapped)) {
            char path[1024]; std::snprintf(path, sizeof path, "%s/%03zu_%ux%u_%d.raw", dir, index, e.extent.width, e.extent.height, (int)e.format);
            if (FILE *f = std::fopen(path, "wb")) { std::fwrite(mapped, 1, size, f); std::fclose(f); written++; }
            vkUnmapMemory(e.device, memory);
        }
        vkDestroyCommandPool(e.device, pool, nullptr);
        vkFreeMemory(e.device, memory, nullptr); vkDestroyBuffer(e.device, buffer, nullptr);
    }
    return written;
}
