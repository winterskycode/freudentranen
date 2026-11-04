#pragma once

#include "core/image.hpp"
#include "core/semaphore.hpp"
#include "core/fence.hpp"
#include "core/commandbuffer.hpp"
#include "shaders/alpha.hpp"
#include "shaders/beta.hpp"
#include "shaders/delta.hpp"
#include "shaders/gamma.hpp"
#include "shaders/generate.hpp"
#include "shaders/mipmaps.hpp"
#include "common/utils.hpp"

#include <vulkan/vulkan_core.h>

#include <optional>
#include <vector>
#include <cstdint>
#include <array>

namespace LSFG_3_1 {

    using namespace LSFG;

    class Context {
    public:
        ///
        /// Create a context
        ///
        /// @param vk The Vulkan instance to use.
        /// @param in0 File descriptor for the first input image.
        /// @param in1 File descriptor for the second input image.
        /// @param outN File descriptors for the output images.
        /// @param extent The size of the images.
        /// @param format The format of the images.
        ///
        /// @throws LSFG::vulkan_error if the context fails to initialize.
        ///
        Context(Vulkan& vk,
            int in0, int in1, const std::vector<int>& outN,
            VkExtent2D extent, VkFormat format);

        ///
        /// Present on the context.
        ///
        /// @param inSem Semaphore to wait on before starting the generation.
        /// @param outSem Semaphores to signal after each generation is done.
        ///
        /// @throws LSFG::vulkan_error if the context fails to present.
        ///
        void present(Vulkan& vk,
            int inSem, const std::vector<int>& outSem);
        /// TOTK: this frame's pictures: timestamps.size() of them (at most generationCount), each at its timestamp
        void present(Vulkan& vk,
            int inSem, const std::vector<int>& outSem, const std::vector<float>& timestamps);

        // Trivially copyable, moveable and destructible
        Context(const Context&) = default;
        Context& operator=(const Context&) = default;
        Context(Context&&) = default;
        Context& operator=(Context&&) = default;
        ~Context() = default;
    private:
        Core::Image inImg_0, inImg_1; // inImg_0 is next when fc % 2 == 0
        std::optional<Core::Image> source; // TOTK: copy-in mode (in0 == in1): the caller's picture, copied in each frame
        uint64_t frameIdx{0};

        struct RenderData {
            Core::Semaphore inSemaphore; // signaled when input is ready
            std::vector<Core::Semaphore> internalSemaphores; // signaled when first step is done
            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done
            std::vector<Core::Fence> completionFences; // fence for completion of each pass

            Core::CommandBuffer cmdBuffer1;
            std::vector<Core::CommandBuffer> cmdBuffers2; // command buffers for second step

            bool shouldWait{false};
            bool recorded{false}; // TOTK: the command buffers, fences and semaphores above are kept and resubmitted
            size_t submitted{0};  // TOTK: passes submitted the last time (their fences are the ones to wait for)
        };
        // TOTK: 24 slots. The command stream repeats every 6 frames (the inputs alternate by 2, gamma's ring by 3), and a
        // slot's buffers are resubmitted only 24 frames later, long after its fences were waited for (8 frames later, as
        // before). Recording everything afresh each frame cost the caller ~4 ms of CPU per frame on the Odin (2026-11-01).
        std::array<RenderData, kSlots> data;

        Shaders::Mipmaps mipmaps;
        std::array<Shaders::Alpha, 7> alpha;
        Shaders::Beta beta;
        std::array<Shaders::Gamma, 7> gamma;
        std::array<Shaders::Delta, 3> delta;
        Shaders::Generate generate;
    };

}
