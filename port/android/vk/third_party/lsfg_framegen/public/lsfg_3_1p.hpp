#pragma once

#include <vulkan/vulkan_core.h>

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace LSFG_3_1P {

    ///
    /// Initialize the LSFG library.
    ///
    /// @param deviceUUID The UUID of the Vulkan device to use.
    /// @param isHdr Whether the images are in HDR format.
    /// @param flowScale Internal flow scale factor.
    /// @param generationCount Number of frames to generate.
    /// @param loader Function to load shader source code by name.
    ///
    /// @throws LSFG::vulkan_error if Vulkan objects fail to initialize.
    ///
    __attribute__((visibility("default")))
    void initialize(uint64_t deviceUUID,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader);

    ///
    /// TOTK: same-device mode. LSFG runs on the app's own instance, device and queue instead of a device of its own:
    /// images are the app's (adoptImage handles in place of file descriptors) and the work is ordered by submission
    /// order on the shared queue (presentContext with inSem -1 and no outSem: no semaphores). The device needs Vulkan
    /// 1.3 with synchronization2, timelineSemaphore, vulkanMemoryModel and robustness2's nullDescriptor enabled; every
    /// call that submits must hold the app's lock for that queue.
    ///
    __attribute__((visibility("default")))
    void initializeShared(VkInstance instance, VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader);

    /// TOTK: a handle for one of the app's images (same-device mode), passed to createContext in place of an fd. The
    /// image needs SAMPLED and STORAGE usage and stays in VK_IMAGE_LAYOUT_GENERAL.
    __attribute__((visibility("default")))
    int adoptImage(VkImage image);

    ///
    /// Create a new LSFG context on a swapchain.
    ///
    /// @param in0 File descriptor for the first input image.
    /// @param in1 File descriptor for the second input image.
    /// @param outN File descriptor for each output image. This defines the LSFG level.
    /// @param extent The size of the images
    /// @param format The format of the images.
    /// @return A unique identifier for the created context.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be created.
    ///
    __attribute__((visibility("default")))
    int32_t createContext(
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format);

    ///
    /// Present a context.
    ///
    /// @param id Unique identifier of the context to present.
    /// @param inSem Semaphore to wait on before starting the generation.
    /// @param outSem Semaphores to signal once each output image is ready.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be presented.
    ///
    __attribute__((visibility("default")))
    void presentContext(int32_t id, int inSem, const std::vector<int>& outSem);
    /// TOTK: present with this frame's picture count and timestamps (timestamps.size() pictures, at most the context's generationCount; empty: all, at their baked timestamps)
    __attribute__((visibility("default")))
    void presentContext(int32_t id, int inSem, const std::vector<int>& outSem, const std::vector<float>& timestamps);

    ///
    /// Delete an LSFG context.
    ///
    /// @param id Unique identifier of the context to delete.
    ///
    __attribute__((visibility("default")))
    void deleteContext(int32_t id);

    ///
    /// Deinitialize the LSFG library.
    ///
    __attribute__((visibility("default")))
    void finalize();

}
