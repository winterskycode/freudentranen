/* C interface to the LSFG frame-generation library (lsfg_bridge.cpp). All functions return < 0 on failure (logged). */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* LSFG on the app's own device and queue. gipa: the app's vkGetInstanceProcAddr (the loaded driver); the device needs
   Vulkan 1.3 with synchronization2, timelineSemaphore, vulkanMemoryModel and robustness2's nullDescriptor. flow_scale
   0.25..1 (lower: cheaper optical flow); generated: pictures per real frame (1 = x2); shader_dir: <name>.spv files.
   Every call except lsfg_bridge_adopt submits to the queue: the caller holds its queue lock. */
int lsfg_bridge_init(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, uint32_t family, int performance, float flow_scale, int generated, const char *shader_dir);
/* A handle for one of the app's images (SAMPLED | STORAGE usage, kept in VK_IMAGE_LAYOUT_GENERAL) */
int lsfg_bridge_adopt(VkImage image);
/* in0 / in1: the two input images (alternating: frame n reads in[n % 2] as the newest), out: one per generated picture;
   returns a context id */
int lsfg_bridge_create(int in0, int in1, const int *out, int nout, uint32_t width, uint32_t height, VkFormat format);
/* Records and submits the generation of one frame on the queue. Ordering is by submission order: the newest input must
   be written (and made visible with a barrier) by work submitted earlier; the outputs are read by work submitted later. */
int lsfg_bridge_present(int context);
/* n pictures at these timestamps (n = 0: none this frame, the flow is still computed) */
int lsfg_bridge_present_at(int context, const float *timestamps, int n);
/* Ends LSFG (its contexts and device state; the app's instance, device and adopted images stay): lsfg_bridge_init may
   then start it again with other settings. Waits for the device to be idle. */
void lsfg_bridge_finalize(void);
#ifdef __cplusplus
}
#endif
