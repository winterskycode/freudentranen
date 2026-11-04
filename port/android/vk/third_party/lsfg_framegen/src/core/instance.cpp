#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using namespace LSFG::Core;

const std::vector<const char*> requiredExtensions = {

};

// TOTK: the app's Vulkan loader (the custom driver it loaded, e.g. Turnip through adrenotools); the system libvulkan
// would bring up a different driver. Set by the app before LSFG_3_1*::initialize.
extern "C" PFN_vkGetInstanceProcAddr lsfg_vk_get_instance_proc_addr;
PFN_vkGetInstanceProcAddr lsfg_vk_get_instance_proc_addr = nullptr;

Instance::Instance(VkInstance existing) {
    if (lsfg_vk_get_instance_proc_addr) volkInitializeCustom(lsfg_vk_get_instance_proc_addr);
    else volkInitialize();
    volkLoadInstanceOnly(existing);
    this->instance = std::shared_ptr<VkInstance>(new VkInstance(existing));
}

Instance::Instance() {
    if (lsfg_vk_get_instance_proc_addr) volkInitializeCustom(lsfg_vk_get_instance_proc_addr);
    else volkInitialize();

    // create Vulkan instance
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsfg-vk-base",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "lsfg-vk-base",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_3
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size()),
        .ppEnabledExtensionNames = requiredExtensions.data()
    };
    VkInstance instanceHandle{};
    auto res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan instance");

    volkLoadInstance(instanceHandle);

    // store in shared ptr
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) {
            vkDestroyInstance(*instance, nullptr);
        }
    );
}
