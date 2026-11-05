// lsfg_test: runs the vendored LSFG frame-generation library (vk/third_party/lsfg_framegen, through vk/lsfg_bridge.cpp)
// on two pictures outside the game, the way the game does (on the caller's device and queue), to tell driver problems
// from integration problems. Builds for the PC and for the device (adb push, run under /data/local/tmp).
//
// usage: lsfg_test <shader dir> <previous.ppm> <newest.ppm> <generated.ppm> [performance 0|1] [flow scale]
//   previous / newest: P6 PPMs of one size (e.g. <files>/fgdump/*-lsfg-in-prev.ppm / -in-new.ppm from the app);
//   generated: the picture LSFG puts halfway between them.
// Environment:
//   LSFG_TEST_DRIVER=<.../libvulkan_freedreno.so> (device): that driver through its Android HAL entry (Turnip) instead
//     of the system Vulkan loader
//   LSFG_TEST_FRAMES=n (default 2): frames run; even ones read the previous picture as the newest, odd ones the newest
//   LSFG_TEST_REWRITE: what happens between LSFG's frames. 1: a transfer writes the input (the inverted previous picture,
//     then the real pair for the last two frames); 2: the same by a compute shader; 3: a transfer into an unrelated image;
//     5: an empty submission; 6: a queue idle; 7: a transfer reads the input; 8: only an image barrier on it; 9: copy-in
//     mode as the game uses it (a transfer writes one picture, LSFG copies it in). On Turnip (every build tried, Mesa
//     26.2 to 27.0-devel, Adreno 830) 1, 2, 7 and 9 make LSFG's first stage compute a constant in a share of runs, which
//     persists; Qualcomm's driver and RADV never do
//   LSFG_TEST_DUMP=<dir>: every internal LSFG image after the last frame, to compare two runs stage by stage
#include <volk.h>
#include "lsfg_bridge.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int lsfg_debug_dump_images(const char *dir);

static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static VkCommandPool pool;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", #x, r_); std::exit(1); } } while (0)

static bool read_ppm(const char *path, uint32_t &w, uint32_t &h, std::vector<uint8_t> &rgba) {
    FILE *f = std::fopen(path, "rb"); if (!f) return false;
    int max = 0; if (std::fscanf(f, "P6 %u %u %d", &w, &h, &max) != 3 || max != 255) { std::fclose(f); return false; }
    std::fgetc(f);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    bool ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size(); std::fclose(f);
    rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) { rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1]; rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255; }
    return ok;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) return t;
    std::fprintf(stderr, "no memory type\n"); std::exit(1);
}

// matches the app's lsfg_image (nvn_vulkan_lsfg.inc)
static VkImage make_image(uint32_t w, uint32_t h) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image; CHECK(vkCreateImage(dev, &ci, nullptr, &image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(dev, &mi, nullptr, &memory));
    CHECK(vkBindImageMemory(dev, image, memory, 0));
    return image;
}

struct Buffer { VkBuffer buffer; void *mapped; };
static Buffer make_buffer(VkDeviceSize size) {
    Buffer b{};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateBuffer(dev, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(dev, &mi, nullptr, &memory));
    CHECK(vkBindBufferMemory(dev, b.buffer, memory, 0));
    CHECK(vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

// Copies between an image (GENERAL afterwards, like the app keeps it) and a buffer, with full barriers on both sides as
// the app's copy has (LSFG's work before and after it on the queue is ordered by them), and waits.
static void transfer(VkImage image, const Buffer &b, uint32_t w, uint32_t h, bool upload, bool first) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.image = image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {w, h, 1};
    if (upload) vkCmdCopyBufferToImage(cb, b.buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    else vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void empty_submit(VkImage image) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &bi));
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.image = image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, image ? 0 : 1, &mb, 0, nullptr, image ? 1 : 0, &ib);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

// The same upload as a compute shader (upload.comp) instead of a transfer.
static const uint32_t upload_spv[] = {
#include "upload_spv.inc"
};
static void compute_upload(VkImage image, const Buffer &b, uint32_t w, uint32_t h) {
    static VkPipeline pipeline; static VkPipelineLayout layout; static VkDescriptorSetLayout set_layout; static VkDescriptorPool dpool;
    if (!pipeline) {
        VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sli.bindingCount = 2; sli.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(dev, &sli, nullptr, &set_layout));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
        CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = sizeof upload_spv; smi.pCode = upload_spv;
        VkShaderModule module; CHECK(vkCreateShaderModule(dev, &smi, nullptr, &module));
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"}; cpi.layout = layout;
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 64; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dpool));
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; CHECK(vkCreateImageView(dev, &vi, nullptr, &view));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, dpool, 1, &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorBufferInfo bi{b.buffer, 0, VK_WHOLE_SIZE}; VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi},
                                      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii}};
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &cbi));
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb); vkDestroyImageView(dev, view, nullptr);
}

#ifdef __ANDROID__
// The Android Vulkan HAL (hardware/hwvulkan.h), enough to reach a driver's vkGetInstanceProcAddr.
struct hw_module_t;
struct hw_device_t { uint32_t tag, version; hw_module_t *module; uint64_t reserved[12]; int (*close)(hw_device_t *); };
struct hw_module_methods_t { int (*open)(const hw_module_t *, const char *id, hw_device_t **device); };
struct hw_module_t { uint32_t tag; uint16_t module_api_version, hal_api_version; const char *id, *name, *author; hw_module_methods_t *methods; void *dso; uint64_t reserved[32 - 7]; };
struct hwvulkan_device_t { hw_device_t common; PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties; PFN_vkCreateInstance CreateInstance; PFN_vkGetInstanceProcAddr GetInstanceProcAddr; };
static PFN_vkGetInstanceProcAddr hal_driver(const char *path) {
    void *so = dlopen(path, RTLD_NOW | RTLD_LOCAL); if (!so) { std::fprintf(stderr, "%s\n", dlerror()); std::exit(1); }
    auto *module = (hw_module_t *)dlsym(so, "HMI"); hw_device_t *device = nullptr;
    if (!module || module->methods->open(module, "vk0", &device) || !device) { std::fprintf(stderr, "%s: no Vulkan HAL\n", path); std::exit(1); }
    return ((hwvulkan_device_t *)device)->GetInstanceProcAddr;
}
#endif

static VkImage make_image(uint32_t w, uint32_t h);
static VkImage source_dummy(uint32_t w, uint32_t h) { static VkImage dummy; if (!dummy) dummy = make_image(w, h); return dummy; }

static int env_int(const char *name, int fallback) { const char *e = std::getenv(name); return e ? std::atoi(e) : fallback; }

int main(int argc, char **argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: %s <shader dir> <previous.ppm> <newest.ppm> <generated.ppm> [performance 0|1] [flow scale]\n", argv[0]); return 2; }
    int performance = argc > 5 ? std::atoi(argv[5]) : 1; float flow = argc > 6 ? (float)std::atof(argv[6]) : 0.75f;
    uint32_t w, h, w2, h2; std::vector<uint8_t> prev, next;
    if (!read_ppm(argv[2], w, h, prev) || !read_ppm(argv[3], w2, h2, next) || w != w2 || h != h2) { std::fprintf(stderr, "cannot read the two pictures (P6, same size)\n"); return 1; }

    PFN_vkGetInstanceProcAddr gipa = nullptr;
#ifdef __ANDROID__
    if (const char *driver = std::getenv("LSFG_TEST_DRIVER")) gipa = hal_driver(driver);
#endif
    if (gipa) volkInitializeCustom(gipa); else { CHECK(volkInitialize()); gipa = vkGetInstanceProcAddr; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; CHECK(vkCreateInstance(&ici, nullptr, &instance));
    volkLoadInstanceOnly(instance);
    uint32_t n = 1; vkEnumeratePhysicalDevices(instance, &n, &phys);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver}; vkGetPhysicalDeviceProperties2(phys, &props2);
    std::printf("device: %s (%s %s), %ux%u, %s mode, flow scale %.2f\n", props2.properties.deviceName, driver.driverName, driver.driverInfo,
        w, h, performance ? "performance" : "quality", flow);

    // the features LSFG needs, as the app enables them (individual structs, as next to its other ones)
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT}; robustness2.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, &robustness2}; sync2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, &sync2}; timeline.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkanMemoryModelFeatures memory_model{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, &timeline}; memory_model.vulkanMemoryModel = VK_TRUE;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = 0; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    const char *exts[] = {VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &memory_model}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = 0;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));

    VkImage in0 = make_image(w, h), in1 = make_image(w, h), out = make_image(w, h);
    Buffer staging = make_buffer((VkDeviceSize)w * h * 4);
    std::memcpy(staging.mapped, prev.data(), prev.size()); transfer(in0, staging, w, h, true, true);
    std::memcpy(staging.mapped, next.data(), next.size()); transfer(in1, staging, w, h, true, true);
    transfer(out, staging, w, h, false, true);

    if (lsfg_bridge_init(gipa, instance, phys, dev, queue, 0, performance, flow, 1, argv[1]) < 0) return 1;
    int out_handle = lsfg_bridge_adopt(out);
    // LSFG_TEST_REWRITE=9: copy-in mode, as the game uses it: we write one picture, LSFG copies it into inputs of its own
    const bool copy_in = env_int("LSFG_TEST_REWRITE", 0) == 9;
    VkImage source = copy_in ? make_image(w, h) : VK_NULL_HANDLE;
    if (copy_in) transfer(source, staging, w, h, false, true);
    int ctx = copy_in ? lsfg_bridge_create(lsfg_bridge_adopt(source), lsfg_bridge_adopt(source), &out_handle, 1, w, h, VK_FORMAT_R8G8B8A8_UNORM)
                      : lsfg_bridge_create(lsfg_bridge_adopt(in0), lsfg_bridge_adopt(in1), &out_handle, 1, w, h, VK_FORMAT_R8G8B8A8_UNORM);
    if (ctx < 0) return 1;

    int frames = env_int("LSFG_TEST_FRAMES", 2) & ~1; if (frames < 2) frames = 2;
    bool rewrite = env_int("LSFG_TEST_REWRITE", 0) != 0, rewrite_compute = env_int("LSFG_TEST_REWRITE", 0) == 2;
    std::vector<uint8_t> inverted(prev); for (size_t i = 0; i < inverted.size(); i++) if (i % 4 != 3) inverted[i] = 255 - inverted[i];
    for (int i = 0; i < frames; i++) {
        const int mode = env_int("LSFG_TEST_REWRITE", 0);
        if (mode == 5) empty_submit(VK_NULL_HANDLE);
        else if (mode == 6) vkQueueWaitIdle(queue);
        else if (mode == 8) empty_submit(i % 2 ? in1 : in0);
        else if (mode == 3) transfer(source_dummy(w, h), staging, w, h, true, false);
        else if (mode == 7) transfer(i % 2 ? in1 : in0, staging, w, h, false, false);
        else if (rewrite) {
            const std::vector<uint8_t> &picture = i == frames - 1 ? next : i == frames - 2 ? prev : inverted;
            std::memcpy(staging.mapped, picture.data(), picture.size());
            if (copy_in) transfer(source, staging, w, h, true, false);
            else if (rewrite_compute) compute_upload(i % 2 ? in1 : in0, staging, w, h); else transfer(i % 2 ? in1 : in0, staging, w, h, true, false);
        }
        if (lsfg_bridge_present(ctx) < 0) return 1;
        // dump after this many frames
        if (i + 1 == env_int("LSFG_TEST_STOP", 0)) break;
    }
    CHECK(vkQueueWaitIdle(queue));

    if (const char *dump = std::getenv("LSFG_TEST_DUMP")) std::printf("%d internal images dumped to %s\n", lsfg_debug_dump_images(dump), dump);
    transfer(out, staging, w, h, false, false);
    FILE *f = std::fopen(argv[4], "wb"); if (!f) { std::perror(argv[4]); return 1; }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const uint8_t *px = (const uint8_t *)staging.mapped;
    for (size_t i = 0; i < (size_t)w * h; i++) std::fwrite(px + i * 4, 1, 3, f);
    std::fclose(f);
    std::printf("wrote %s\n", argv[4]);
    return 0;
}
