#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/image.hpp"
#include "common/utils.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <vector>
#include <optional>

using namespace LSFG::Core;

namespace LSFG::Core {
void debugRegisterImage(const Device& device, const std::shared_ptr<VkImage>& image,
    const std::shared_ptr<VkImageLayout>& layout, VkExtent2D extent, VkFormat format, VkImageAspectFlags aspect);
}

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create image
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,   // TOTK: cleared below
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        } // NOLINTEND
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // allocate and bind memory
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // create image view
    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .levelCount = 1,
            .layerCount = 1
        }
    };

    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    // store objects in shared ptr
    this->layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            vkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            vkFreeMemory(dev, *mem, nullptr);
        }
    );
    this->view = std::shared_ptr<VkImageView>(
        new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            vkDestroyImageView(dev, *imgView, nullptr);
        }
    );
    debugRegisterImage(device, this->image, this->layout, extent, format, aspectFlags);   // TOTK: debug_dump.cpp

    // TOTK: zeroed before first use. LSFG reads some images (previous-frame state) before it ever writes them; fresh
    // memory happens to be zero on desktop drivers, but on Turnip (KGSL, UBWC) it is not, and the garbage persists in
    // the temporal state for good (a scaled double image in every generated frame).
    Utils::clearImage(device, *this);
}

// TOTK: same-device mode. The handles count down from INT_MAX, far above any file descriptor.
namespace {
    std::vector<VkImage> adopted;
    constexpr int adoptBase = 0x7fffffff;
    bool adoptedImage(int handle) { return handle > adoptBase - static_cast<int>(adopted.size()) && handle <= adoptBase; }
}

int Image::adoptImage(VkImage image) {
    adopted.push_back(image);
    return adoptBase - static_cast<int>(adopted.size() - 1);
}

void Image::adopt(const Core::Device& device, VkExtent2D extent, VkFormat format, VkImageAspectFlags aspectFlags, int handle) {
    const VkImage imageHandle = adopted.at(static_cast<size_t>(adoptBase - handle));
    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { .aspectMask = aspectFlags, .levelCount = 1, .layerCount = 1 }
    };
    VkImageView viewHandle{};
    auto res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");
    this->layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_GENERAL);   // the app keeps its images in GENERAL
    this->image = std::shared_ptr<VkImage>(new VkImage(imageHandle));
    this->memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(VK_NULL_HANDLE));
    this->view = std::shared_ptr<VkImageView>(
        new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            vkDestroyImageView(dev, *imgView, nullptr);
        }
    );
    debugRegisterImage(device, this->image, this->layout, extent, format, aspectFlags);
}

// shared memory constructor

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int fd)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    if (adoptedImage(fd)) { adopt(device, extent, format, aspectFlags, fd); return; }   // TOTK: same-device mode

    // create image
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,   // TOTK: the exporter's usage exactly (shared images need identical creation parameters)
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        } // NOLINTEND
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // ~~allocate~~ and bind memory
    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo2{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicatedInfo2,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
        .fd = fd // closes the fd
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = fd == -1 ? nullptr : &importInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // create image view
    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .levelCount = 1,
            .layerCount = 1
        }
    };

    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    // store objects in shared ptr
    this->layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            vkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            vkFreeMemory(dev, *mem, nullptr);
        }
    );
    this->view = std::shared_ptr<VkImageView>(
        new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            vkDestroyImageView(dev, *imgView, nullptr);
        }
    );
    debugRegisterImage(device, this->image, this->layout, extent, format, aspectFlags);   // TOTK: debug_dump.cpp
}
