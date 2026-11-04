#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1p/context.hpp"
#include "common/utils.hpp"
#include "common/exception.hpp"

#include <vector>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <cstdint>

using namespace LSFG;
using namespace LSFG_3_1P;

Context::Context(Vulkan& vk,
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    // import input images
    if (in0 == in1) {
        // TOTK: copy-in mode. The caller hands in one picture, which each frame copies into an input image of the
        // library's own at the start of its command buffer. On Turnip (Adreno 830), an input image the caller's own
        // commands touch (write or even read) between LSFG's frames is sampled wrongly by the next frame's first stage
        // (a constant), so the images LSFG samples are touched by nothing else.
        this->source.emplace(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } else {
    this->inImg_0 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in0);
    this->inImg_1 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in1);
    }

    // prepare render data
    for (size_t i = 0; i < this->data.size(); i++) {
        auto& data = this->data.at(i);
        data.internalSemaphores.resize(vk.generationCount);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        data.cmdBuffers2.resize(vk.generationCount);
    }

    // create shader chains
    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
        this->gamma.at(i) = Shaders::Gamma(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()));
        if (i < 4) continue;

        this->delta.at(i - 4) = Shaders::Delta(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(6 - i),
            (i == 4) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage1()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage2()));
    }
    this->generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        this->gamma.at(6).getOutImage(),
        this->delta.at(2).getOutImage1(),
        this->delta.at(2).getOutImage2(),
        outN, format);
}

void Context::present(Vulkan& vk,
        int inSem, const std::vector<int>& outSem) {
    present(vk, inSem, outSem, {});
}

void Context::present(Vulkan& vk,
        int inSem, const std::vector<int>& outSem, const std::vector<float>& timestamps) {
    const auto slot = static_cast<uint32_t>(this->frameIdx % this->data.size());
    auto& data = this->data.at(slot);
    const size_t count = timestamps.empty() ? vk.generationCount : std::min<size_t>(timestamps.size(), vk.generationCount);

    // 3. wait for completion of previous frame in this slot
    if (data.shouldWait)
        for (size_t pass = 0; pass < data.submitted; pass++)
            if (!data.completionFences.at(pass).wait(vk.device, UINT64_MAX))
                throw LSFG::vulkan_error(VK_TIMEOUT, "Fence wait timed out");
    data.shouldWait = true;

    // TOTK: without external semaphores the slot's command buffers are recorded once (from the second round of slots
    // on, when every image is in the general layout the barriers were recorded with) and resubmitted afterwards; its
    // fences are reset and its semaphores reused. See the data array's note in the header.
    const bool reuse = data.recorded;

    // 1. create mipmaps and process input image
    if (!reuse) {
        if (inSem >= 0) data.inSemaphore = Core::Semaphore(vk.device, inSem);
        for (size_t i = 0; i < vk.generationCount; i++)
            data.internalSemaphores.at(i) = Core::Semaphore(vk.device);

        data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);
        data.cmdBuffer1.begin();
        if (this->source) // TOTK: copy-in mode
            Utils::copyIn(data.cmdBuffer1, *this->source, this->frameIdx % 2 == 0 ? this->inImg_0 : this->inImg_1);

        this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);
        for (size_t i = 0; i < 7; i++)
            this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);
        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);

        data.cmdBuffer1.end();
    }
    std::vector<Core::Semaphore> waits = { data.inSemaphore };
    if (inSem < 0) waits.clear();
    const std::vector<Core::Semaphore> internals(data.internalSemaphores.begin(), data.internalSemaphores.begin() + static_cast<long>(count));   // TOTK: one per submitted pass
    data.cmdBuffer1.submit(vk.device.getComputeQueue(), std::nullopt,
        waits, std::nullopt,
        internals, std::nullopt);

    // 2. generate intermediary frames (TOTK: every pass is recorded, the first `count` are submitted, each with this
    //    frame's timestamp copied into its constant buffers at the start of its command buffer)
    for (size_t pass = 0; pass < vk.generationCount; pass++) {
        auto& internalSemaphore = data.internalSemaphores.at(pass);
        auto& outSemaphore = data.outSemaphores.at(pass);
        if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));
        auto& completionFence = data.completionFences.at(pass);
        auto& buf2 = data.cmdBuffers2.at(pass);
        if (reuse) { if (pass < count) completionFence.reset(vk.device); }
        else {
            completionFence = Core::Fence(vk.device);
            buf2 = Core::CommandBuffer(vk.device, vk.commandPool);
            buf2.begin();
            vk.resources.recordTimestampCopies(vk.device, buf2.handle(), slot, static_cast<uint32_t>(pass));

            for (size_t i = 0; i < 7; i++) {
                this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass);
                if (i >= 4)
                    this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass);
            }
            this->generate.Dispatch(buf2, this->frameIdx, pass);

            buf2.end();
        }
        if (pass >= count) continue;
        vk.resources.writeTimestamp(vk.device, slot, static_cast<uint32_t>(pass),
            timestamps.empty() ? static_cast<float>(pass + 1) / static_cast<float>(vk.generationCount + 1) : timestamps.at(pass));
        std::vector<Core::Semaphore> signals = { outSemaphore };
        if (inSem < 0) signals.clear();
        buf2.submit(vk.device.getComputeQueue(), completionFence,
            { internalSemaphore }, std::nullopt,
            signals, std::nullopt);
    }
    data.submitted = count;

    if (!reuse && inSem < 0 && this->frameIdx >= this->data.size()) data.recorded = true;
    this->frameIdx++;
}
