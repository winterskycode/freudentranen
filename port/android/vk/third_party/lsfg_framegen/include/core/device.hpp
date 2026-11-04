#pragma once

#include "core/instance.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>

namespace LSFG::Core {

    ///
    /// C++ wrapper class for a Vulkan device.
    ///
    /// This class manages the lifetime of a Vulkan device.
    ///
    class Device {
    public:
        ///
        /// Create the device.
        ///
        /// @param instance Vulkan instance
        /// @param deviceUUID The UUID of the Vulkan device to use.
        ///
        /// @throws LSFG::vulkan_error if object creation fails.
        ///
        Device(const Instance& instance, uint64_t deviceUUID);

        ///
        /// TOTK: the app's own device and queue (same-device mode); not destroyed here. The device must have
        /// synchronization2, timelineSemaphore, vulkanMemoryModel and robustness2's nullDescriptor enabled (Vulkan 1.3).
        /// The caller serializes queue use: every LSFG call that submits runs under the app's queue lock.
        ///
        Device(VkPhysicalDevice physical, VkDevice existing, VkQueue queue, uint32_t family);

        /// Get the Vulkan handle.
        [[nodiscard]] auto handle() const { return *this->device; }
        /// Get the physical device associated with this logical device.
        [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return this->physicalDevice; }
        /// Get the compute queue family index.
        [[nodiscard]] uint32_t getComputeFamilyIdx() const { return this->computeFamilyIdx; }
        /// Get the compute queue.
        [[nodiscard]] VkQueue getComputeQueue() const { return this->computeQueue; }

        // Trivially copyable, moveable and destructible
        Device(const Core::Device&) noexcept = default;
        Device& operator=(const Core::Device&) noexcept = default;
        Device(Device&&) noexcept = default;
        Device& operator=(Device&&) noexcept = default;
        ~Device() = default;
    private:
        std::shared_ptr<VkDevice> device;
        VkPhysicalDevice physicalDevice{};

        uint32_t computeFamilyIdx{0};

        VkQueue computeQueue{};
    };

}
