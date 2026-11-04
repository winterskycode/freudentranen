#include "pool/resourcepool.hpp"
#include <volk.h>
#include <cmath>
#include "core/buffer.hpp"
#include "core/device.hpp"
#include "core/sampler.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>

using namespace LSFG;
using namespace LSFG::Pool;

struct ConstantBuffer {
    std::array<uint32_t, 2> inputOffset;
    uint32_t firstIter;
    uint32_t firstIterS;
    uint32_t advancedColorKind;
    uint32_t hdrSupport;
    float resolutionInvScale;
    float timestamp;
    float uiThreshold;
    std::array<uint32_t, 3> pad;
};

Core::Buffer ResourcePool::getBuffer(
            const Core::Device& device,
            float timestamp, bool firstIter, bool firstIterS) {
    uint64_t hash = 0;
    const union { float f; uint32_t i; } u{
        .f = timestamp };
    hash |= u.i;
    hash |= static_cast<uint64_t>(firstIter) << 32;
    hash |= static_cast<uint64_t>(firstIterS) << 33;

    auto it = buffers.find(hash);
    if (it != buffers.end())
        return it->second;

    // create the buffer
    const ConstantBuffer data{
        .inputOffset = { 0, 0 },
        .advancedColorKind = this->isHdr ? 2U : 0U,
        .hdrSupport = this->isHdr,
        .resolutionInvScale = this->flowScale,
        .timestamp = timestamp,
        .uiThreshold = 0.5F,
    };
    Core::Buffer buffer(device, data, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    buffers[hash] = buffer;
    // TOTK: a generated picture's buffer: remembered by pass for per-frame timestamps
    if (this->generationCount > 0 && timestamp > 0.0F && timestamp < 1.0F) {
        const float passF = timestamp * static_cast<float>(this->generationCount + 1) - 1.0F;
        const auto pass = static_cast<uint32_t>(passF + 0.5F);
        if (pass < this->generationCount && std::abs(passF - static_cast<float>(pass)) < 0.01F)
            this->passBuffers[pass].push_back({ firstIter, firstIterS, buffer });
    }
    return buffer;
}

// TOTK
void ResourcePool::recordTimestampCopies(const Core::Device& device, VkCommandBuffer cmd, uint32_t slot, uint32_t pass) {
    auto it = this->passBuffers.find(pass);
    if (it == this->passBuffers.end()) return;
    for (size_t i = 0; i < it->second.size(); i++) {
        auto& pb = it->second.at(i);
        const uint64_t key = (static_cast<uint64_t>(slot) << 32) | (static_cast<uint64_t>(pass) << 16) | i;
        auto st = this->staging.find(key);
        if (st == this->staging.end()) {
            const ConstantBuffer data{
                .inputOffset = { 0, 0 }, .firstIter = pb.firstIter, .firstIterS = pb.firstIterS,
                .advancedColorKind = this->isHdr ? 2U : 0U, .hdrSupport = this->isHdr,
                .resolutionInvScale = this->flowScale,
                .timestamp = static_cast<float>(pass + 1) / static_cast<float>(this->generationCount + 1),
                .uiThreshold = 0.5F,
            };
            st = this->staging.emplace(key, Core::Buffer(device, data, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)).first;
        }
        const VkBufferMemoryBarrier2 before{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .buffer = pb.buffer.handle(), .size = VK_WHOLE_SIZE };
        const VkDependencyInfo depBefore{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &before };
        vkCmdPipelineBarrier2(cmd, &depBefore);
        const VkBufferCopy region{ .size = sizeof(ConstantBuffer) };
        vkCmdCopyBuffer(cmd, st->second.handle(), pb.buffer.handle(), 1, &region);
        const VkBufferMemoryBarrier2 after{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT,
            .buffer = pb.buffer.handle(), .size = VK_WHOLE_SIZE };
        const VkDependencyInfo depAfter{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &after };
        vkCmdPipelineBarrier2(cmd, &depAfter);
    }
}

void ResourcePool::writeTimestamp(const Core::Device& device, uint32_t slot, uint32_t pass, float timestamp) {
    auto it = this->passBuffers.find(pass);
    if (it == this->passBuffers.end()) return;
    for (size_t i = 0; i < it->second.size(); i++) {
        auto& pb = it->second.at(i);
        const uint64_t key = (static_cast<uint64_t>(slot) << 32) | (static_cast<uint64_t>(pass) << 16) | i;
        auto st = this->staging.find(key);
        if (st == this->staging.end()) continue;   // not recorded yet: the baked timestamp stays
        const ConstantBuffer data{
            .inputOffset = { 0, 0 }, .firstIter = pb.firstIter, .firstIterS = pb.firstIterS,
            .advancedColorKind = this->isHdr ? 2U : 0U, .hdrSupport = this->isHdr,
            .resolutionInvScale = this->flowScale, .timestamp = timestamp, .uiThreshold = 0.5F,
        };
        st->second.write(device, &data, sizeof data);
    }
}

Core::Sampler ResourcePool::getSampler(
            const Core::Device& device,
            VkSamplerAddressMode type,
            VkCompareOp compare,
            bool isWhite) {
    uint64_t hash = 0;
    hash |= static_cast<uint64_t>(type) << 0;
    hash |= static_cast<uint64_t>(compare) << 8;
    hash |= static_cast<uint64_t>(isWhite) << 16;

    auto it = samplers.find(hash);
    if (it != samplers.end())
        return it->second;

    // create the sampler
    Core::Sampler sampler(device, type, compare, isWhite);
    samplers[hash] = sampler;
    return sampler;
}
