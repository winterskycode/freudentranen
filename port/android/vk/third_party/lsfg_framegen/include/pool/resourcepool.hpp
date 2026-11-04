#pragma once

#include "core/device.hpp"
#include "core/buffer.hpp"
#include "core/sampler.hpp"

#include "vulkan/vulkan_core.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace LSFG::Pool {

    ///
    /// Resource pool for each Vulkan device.
    ///
    class ResourcePool {
    public:
        ResourcePool() noexcept = default;

        ///
        /// Create the resource pool.
        ///
        /// @param isHdr HDR support stored in buffers.
        /// @param flowScale Scale factor stored in buffers.
        ///
        /// @throws std::runtime_error if the resource pool cannot be created.
        ///
        ResourcePool(bool isHdr, float flowScale, uint64_t generationCount = 0)
            : isHdr(isHdr), flowScale(flowScale), generationCount(generationCount) {}

        /// TOTK: per-frame timestamps. The constant buffers of a generated picture (one per flag combination, shared by
        /// every shader of that picture) are remembered by pass; recordTimestampCopies records, at the start of a slot's
        /// command buffer, a copy into each of them from a staging buffer of that slot and pass, and writeTimestamp
        /// fills the staging buffers before the slot is submitted. The constant buffers then carry the frame's
        /// timestamp when the pass's shaders run, in queue order, while the recorded command buffers stay as they are.
        void recordTimestampCopies(const Core::Device& device, VkCommandBuffer cmd, uint32_t slot, uint32_t pass);
        void writeTimestamp(const Core::Device& device, uint32_t slot, uint32_t pass, float timestamp);

        ///
        /// Retrieve a buffer with given parameters or create it.
        ///
        /// @param timestamp Timestamp stored in buffer
        /// @param firstIter First iteration stored in buffer
        /// @param firstIterS First special iteration stored in buffer
        /// @return Created or cached buffer
        ///
        /// @throws LSFG::vulkan_error if the buffer cannot be created.
        ///
        Core::Buffer getBuffer(
            const Core::Device& device,
            float timestamp = 0.0F, bool firstIter = false, bool firstIterS = false);

        ///
        /// Retrieve a sampler by type or create it.
        ///
        /// @param type Type of the sampler
        /// @param compare Compare operation for the sampler
        /// @param isWhite Whether the sampler is white
        /// @return Created or cached sampler
        ///
        /// @throws LSFG::vulkan_error if the sampler cannot be created.
        ///
        Core::Sampler getSampler(
            const Core::Device& device,
            VkSamplerAddressMode type = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VkCompareOp compare = VK_COMPARE_OP_NEVER,
            bool isWhite = false);

    private:
        std::unordered_map<uint64_t, Core::Buffer> buffers;
        std::unordered_map<uint64_t, Core::Sampler> samplers;
        bool isHdr{};
        float flowScale{};
        uint64_t generationCount{};
        struct PassBuffer { bool firstIter, firstIterS; Core::Buffer buffer; };
        std::unordered_map<uint32_t, std::vector<PassBuffer>> passBuffers;   // by pass
        std::unordered_map<uint64_t, Core::Buffer> staging;                   // by (slot, pass, index)
    };

}
