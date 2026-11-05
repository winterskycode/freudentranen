// Each Vulkan context owns its loader and complete dispatch; select a driver only at initialization.
#pragma once
#ifdef __ANDROID__
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#endif
#include <vulkan/vulkan.h>
#include "vulkan_dispatch.h"

typedef struct dm_vk_driver {
    void *library, *adrenotools;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define DM_VK_FIELD(name) PFN_##name name;
    DM_VK_GLOBAL_FUNCTIONS(DM_VK_FIELD)
    DM_VK_INSTANCE_FUNCTIONS(DM_VK_FIELD)
    DM_VK_ANDROID_INSTANCE_FUNCTIONS(DM_VK_FIELD)
    DM_VK_DEVICE_FUNCTIONS(DM_VK_FIELD)
#undef DM_VK_FIELD
    char hook_directory[1024], driver_directory[1024], driver_name[128], error[512];
    int requested_custom, using_custom, require_custom, needs_surface;
    uint32_t actual_driver_id, subgroup_size;
    // Preserve actual physical defaults; record explicitly selected shader size separately and choose only at initialization.
    uint32_t requested_subgroup_size, effective_shader_subgroup_size;
    VkShaderStageFlags required_subgroup_stages;
    int subgroup_policy_required, subgroup_policy_supported, subgroup_policy_prepared, subgroup_policy_enabled;
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup_features;
    const char **policy_device_extensions;
} dm_vk_driver;

int dm_vk_driver_open(dm_vk_driver *driver);
int dm_vk_driver_load_instance(dm_vk_driver *driver, VkInstance instance, int needs_surface);
int dm_vk_driver_load_device(dm_vk_driver *driver, VkDevice device);
// A NULL shader directory is for independent diagnostics/GLSL tests without an offline Maxwell cache.
int dm_vk_driver_validate(dm_vk_driver *driver, VkPhysicalDevice physical, const char *shader_directory);
// Preserve caller device extensions/feature chains; fail on unsupported explicit policy and preserve defaults.
int dm_vk_driver_prepare_device(dm_vk_driver *driver, VkDeviceCreateInfo *create);
// Keep caller-owned required storage alive until pipeline creation; never enable FULL, allowing guest groups of 32.
int dm_vk_driver_prepare_shader_stage(dm_vk_driver *driver, VkPipelineShaderStageCreateInfo *stage, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *required);
// Destroy old surface/instance/device with their old dispatch before calling; live-object switching is unsupported.
int dm_vk_driver_fallback(dm_vk_driver *driver, const char *reason);
const char *dm_vk_driver_error(const dm_vk_driver *driver);
