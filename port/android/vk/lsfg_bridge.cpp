// C interface to the LSFG frame-generation library (vk/third_party/lsfg_framegen, lsfg-vk v1.0.0, MIT) for the Vulkan
// backend (nvn_vulkan_lsfg.inc). The library runs on the app's own device and queue (its "same-device mode", a local
// change): a second device sharing images through file descriptors breaks on Turnip, where GPU work of another device
// between LSFG's frames leaves its first stage computing from a constant (see tools/lsfg_test). Its shaders are Lossless
// Scaling's, made on the startup screen from the player's own Lossless.dll (Lsfg.java) and read here from
// <dir>/<name>.spv. Exceptions stop at this boundary.
#include "lsfg_bridge.h"
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cstdio>

extern "C" PFN_vkGetInstanceProcAddr lsfg_vk_get_instance_proc_addr;   // third_party/lsfg_framegen/src/core/instance.cpp

namespace {
bool g_performance;
std::string g_dir;
void note(const char *what, const std::exception &e) { std::fprintf(stderr, "[lsfg] %s failed: %s\n", what, e.what()); }
std::vector<uint8_t> load(const std::string &name) {
    std::ifstream f(g_dir + "/" + name + ".spv", std::ios::binary);
    if (!f) { std::fprintf(stderr, "[lsfg] shader %s missing in %s (Tools > Lossless Scaling on the startup screen makes them)\n", name.c_str(), g_dir.c_str()); return {}; }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
}

extern "C" {

int lsfg_bridge_init(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
        VkQueue queue, uint32_t family, int performance, float flow_scale, int generated, const char *shader_dir) {
    lsfg_vk_get_instance_proc_addr = gipa; g_performance = performance != 0; g_dir = shader_dir;
    try {
        if (g_performance) LSFG_3_1P::initializeShared(instance, physical, device, queue, family, false, 1.0f / flow_scale, (uint64_t)generated, load);
        else LSFG_3_1::initializeShared(instance, physical, device, queue, family, false, 1.0f / flow_scale, (uint64_t)generated, load);
        return 0;
    } catch (const std::exception &e) { note("initialize", e); return -1; }
}

int lsfg_bridge_adopt(VkImage image) {
    return g_performance ? LSFG_3_1P::adoptImage(image) : LSFG_3_1::adoptImage(image);
}

int lsfg_bridge_create(int in0, int in1, const int *out, int nout, uint32_t width, uint32_t height, VkFormat format) {
    try {
        std::vector<int> outs(out, out + nout);
        return g_performance ? LSFG_3_1P::createContext(in0, in1, outs, {width, height}, format)
                             : LSFG_3_1::createContext(in0, in1, outs, {width, height}, format);
    } catch (const std::exception &e) { note("createContext", e); return -1; }
}

int lsfg_bridge_present(int context) { return lsfg_bridge_present_at(context, nullptr, 0); }

// this frame's pictures: n of them (0..the context's count) at the given timestamps (0..1 between the previous
// picture and this one); n < 0: every picture at its fixed timestamp
int lsfg_bridge_present_at(int context, const float *timestamps, int n) {
    try {
        const std::vector<int> none;   // inSem -1, no outSem: no semaphores, ordered by the shared queue
        const std::vector<float> at(timestamps, timestamps + (n > 0 ? n : 0));
        if (n < 0) { if (g_performance) LSFG_3_1P::presentContext(context, -1, none); else LSFG_3_1::presentContext(context, -1, none); }
        else if (g_performance) LSFG_3_1P::presentContext(context, -1, none, at);
        else LSFG_3_1::presentContext(context, -1, none, at);
        return 0;
    } catch (const std::exception &e) { note("presentContext", e); return -1; }
}


void lsfg_bridge_finalize(void) {
    try { if (g_performance) LSFG_3_1P::finalize(); else LSFG_3_1::finalize(); }
    catch (const std::exception &e) { note("finalize", e); }
}

}  // extern "C"
