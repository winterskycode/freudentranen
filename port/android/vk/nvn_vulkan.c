// Vulkan NVN command executor. Retain upstream C interface names to reuse the recorder.
#include "d2r/dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "d2r/nvn_dirty.h"
#include "nvn_deswizzle.h"
#include <sys/resource.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include "host_posix.h"
#include "legacy_digest.h"
#include "performance_hint.h"
#include "pending_ranges.h"
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#define DM_VK_DISPATCH_CONTEXT vk_api
#include "gpu_driver.h"
static dm_vk_driver vk_api;
#endif
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <errno.h>
#include <dlfcn.h>
#include "totk_affinity.h"

/* Live VkDeviceMemory objects, against the driver's maxMemoryAllocationCount (logged every 600 frames): images and
   buffers each have their own allocation */
static _Atomic int64_t vk_memory_objects;
#ifdef vkAllocateMemory
static VkResult vk_allocate_memory_counted(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *m) {
    VkResult r = (DM_VK_DISPATCH_CONTEXT.vkAllocateMemory)(d, info, a, m); if (r == VK_SUCCESS) atomic_fetch_add(&vk_memory_objects, 1); return r;
}
static void vk_free_memory_counted(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    if (m != VK_NULL_HANDLE) atomic_fetch_sub(&vk_memory_objects, 1); (DM_VK_DISPATCH_CONTEXT.vkFreeMemory)(d, m, a);
}
#undef vkAllocateMemory
#undef vkFreeMemory
#define vkAllocateMemory vk_allocate_memory_counted
#define vkFreeMemory vk_free_memory_counted
#endif
#define VK_CHECK(expr) do { VkResult result_ = (expr); if (result_ != VK_SUCCESS) die("Vulkan %s failed: %d", #expr, result_); } while (0)
typedef struct { _Atomic unsigned references; uint64_t activation; } buffer_owner;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; uint64_t size, capacity; buffer_owner *owner; uint64_t slab_offset; int slab; } vk_buffer;
typedef struct cached_buffer { vk_buffer buffer; struct cached_buffer *next; } cached_buffer;
static cached_buffer *buffer_cache;
static pthread_mutex_t buffer_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t buffer_cache_bytes, buffer_allocations, buffer_reuses, snapshot_bytes, active_buffer_bytes, buffer_activation;
// Large buffers (> 8 MiB) are accounted separately: buffer_large_cache_bytes is the part of buffer_cache_bytes held by large buffers (see buffer_destroy).
#define DM_VK_SMALL_BUFFER_MAX (8ull << 20)
#define DM_VK_LARGE_BUFFER_MAX (64ull << 20)
#define DM_VK_LARGE_CACHE_BUDGET (128ull << 20)
static uint64_t buffer_large_cache_bytes, buffer_large_allocations, buffer_large_allocated_bytes, buffer_large_reuses, buffer_large_freed;
// Read-only probe: wall clock and thread CPU accumulated inside creating a buffer (vkCreateBuffer + allocate + bind +
// map), a hundred-plus times a frame in a heavy scene. Two extra clock reads on that path; nothing else changes.
static uint64_t buffer_create_wall_ns, buffer_create_cpu_ns;
typedef struct read_snapshot { uintptr_t address; uint64_t stamp, used; vk_buffer buffer; struct read_snapshot *next,*prev,*hash_next; } read_snapshot;
static read_snapshot *read_snapshots,*read_snapshot_tail,*read_index[8192];
static uint64_t read_cache_bytes, read_cache_clock, read_cache_hits, read_cache_misses, immutable_buffer_bytes;
static void read_cache_remove(uintptr_t address, uint64_t size);
static void snapshot_diag_poll(void);
static void pso_warm_start(void);
static unsigned batch_draws;
static void descriptor_reset(void);
static void batch_resources_select(unsigned slot);
static void forward_reset(void);
static uint64_t timestamp_delta(uint64_t begin,uint64_t end,unsigned bits);
typedef struct image_entry image_entry;
static void graphics_pass_end(void);
static void pclear_flush_all(void);
static int graphics_pass_open(void);
static int pass_queries_on(void);static void samples_report(void);static void samples_reset(void);
static void ucap_wait_current(void);
/* draws this batch wrote storage the host reads after the fence: barrier at the commit */
static int graphics_host_writes;
static int graphics_image_is_attachment(const image_entry *image);
static void graphics_cache_forget_image(image_entry *image);
static void dependencies_prepare_range(uintptr_t address,uint64_t size);
static void dependencies_prepare_texture(const tex_t *texture);
static void dependencies_prepare_copy_texture(const p_copy_b2t *copy);
static void graphics_probe_draw(const p_draw *draw,const p_draw_idx *indexed);
static void graphics_command_reset(void);
/* command buffers begun: chunk boundaries and batch starts (image_entry.touched_chunk, the upload census) */
static uint64_t chunk_epoch;
static void graphics_probe_dispatch(const p_dispatch *dispatch);
static void graphics_probe_query_batch(uint64_t ticket,const uint64_t *counts,uint32_t count);
static void graphics_probe_poll(void);
static void graphics_probe_live_done(void);
/* zc_*: zero-copy backing (TOTK_VK_ZEROCOPY=1): the pool's pages are an exported Vulkan allocation (compat_guest_overlay) */
typedef struct pool_range { const uint8_t *base; uint64_t size; struct pool_range *next; VkDeviceMemory zc_memory; VkBuffer zc_buffer; int zc_fd; } pool_range;
/* write-back origins: shader-written storage buffer / texel buffer, buffer copy from a GPU result / from CPU data,
   texture-to-buffer copy */
enum { POST0_SITE_SSBO = 1, POST0_SITE_TEXEL, POST0_SITE_COPY_GPU, POST0_SITE_COPY_CPU, POST0_SITE_TEX_TO_BUF, POST0_SITES };
typedef struct post_op {
    int kind; volatile uint32_t *event; uint32_t mode, value;
    uint64_t order;int range_registered,counter_gpu_encoded,counter_timestamp_encoded;
    void *destination; uint64_t size; vk_buffer source, output;
    /* the CPU-known initial bytes of a shader-written buffer; only blocks that changed are published */
    uint8_t *seed;
    /* write-backs (kind 0): where it was made, for [vulkan-post0-sites] (POST0_SITE_*) */
    uint8_t site;
    uint64_t batch_serial;
    /* census: the copied piece (ordinal in its submission) and the submission it was registered in */
    int census_piece; uint64_t census_frame;
    /* its range in the pending page tables */
    uintptr_t pp_all_addr,pp_unknown_addr; uint64_t pp_all_size,pp_unknown_size;
    VkRenderPass pass; VkFramebuffer frame; VkPipeline pipeline;
    VkPipelineLayout pipeline_layout; VkDescriptorPool descriptors;
    VkImageView image_view;
    VkBufferView buffer_view;
    /* kind 6: a GPU-written image going back to guest memory (nvn_vulkan_tex.inc) */
    struct image_writeback *writeback;
    VkPipelineStageFlags gpu_stage;VkAccessFlags gpu_access,gpu_synchronized[32];
    /* GPU reads of this version since its last write: by earlier draws / by draw read_draw (ssbo_write_in_place) */
    VkPipelineStageFlags read_stages,read_stages_draw;uint64_t read_draw;
    /* the render pass (render_passes while it is open) of its latest in-place write by a draw (0: outside one), and the
           game barriers before it (ssbo_barrier_epoch) */
    uint64_t inplace_pass,inplace_epoch;
    struct post_op *next,*copy_next;
} post_op;
static int counter_gpu_record(post_op *post);
static void counter_gpu_flush(void);
struct async_batch;static void counter_gpu_batch_begin(struct async_batch *batch);
static void graphics_query_split(void);
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkCommandPool commands;
static VkCommandBuffer command;
static VkFence gpu_fence;
static VkPipelineCache pipeline_cache;
static VkQueryPool draw_queries;
static VkQueryPool batch_times;
static uint32_t gpu_timestamp_bits;
static double gpu_timestamp_period,gpu_total_ns,fence_wait_total_ns;
// mtl_wait's blocked time accumulates atomically on its own: a caller (the game thread's SyncWait) must not contend for queue_lock just to
// record a reading, since in translation-thread mode the translation thread holds it for a whole submit.
static _Atomic uint64_t fence_wait_unlocked_ns;
// queue_lock protects totals; worker publication overlaps foreground work, not extra frame time.
static uint64_t stability_ring_wait_ns,stability_worker_publish_ns;
static uint32_t used_queries;
static uint64_t query_counts[1024];
static VkPhysicalDeviceMemoryProperties memory_properties;
static VkPhysicalDeviceFeatures gpu_features;
static void memprobe_run(void);
static void overlay_setup(uint32_t family);
void overlay_forget_image(VkImage image);
/* VK_EXT_provoking_vertex: NVN (like GL) takes flat varyings from the last vertex (nvnCommandBufferSetProvokingVertexMode 0),
   Vulkan from the first; flat varyings follow the guest convention when the device has the extension */
static int provoking_last;
/* The device is made ready for LSFG (Vulkan 1.3, robustness2's nullDescriptor, synchronization2, ...) whenever the driver
   can: the in-game menu switches LSFG on and off live. TOTK_LSFG=0: not. */
static int lsfg_device_wanted(void) { const char *l = getenv("TOTK_LSFG"); return !(l && !strcmp(l, "0")); }
/* TOTK_FRAMEGEN=lsfg<rate>[x<multiplier>] (the menu's LSFG choices), or TOTK_LSFG=1 */
static int lsfg_requested(void) { const char *l = getenv("TOTK_LSFG"), *f = getenv("TOTK_FRAMEGEN"); return (l && atoi(l) > 0) || (f && !strncmp(f, "lsfg", 4)); }
/* VK_EXT_extended_dynamic_state (+ _2): depth / stencil state, cull mode, front face (and depth-bias enable) are set per
   draw instead of baked into the pipeline. TOTK changes them per draw, so with them in the pipeline key every new
   combination with the same shaders compiles a new pipeline (~3 a frame in the Room of Awakening, most hitches).
   TOTK_VK_EDS=0 keeps them in the pipeline, which the TOTK_VK_EXP depth/cull experiments need. */
static int ext_eds, ext_eds2, eds_on, eds2_on;
static PFN_vkCmdSetDepthTestEnableEXT eds_depth_test; static PFN_vkCmdSetDepthWriteEnableEXT eds_depth_write;
static PFN_vkCmdSetDepthCompareOpEXT eds_depth_compare; static PFN_vkCmdSetStencilTestEnableEXT eds_stencil_test;
static PFN_vkCmdSetStencilOpEXT eds_stencil_op; static PFN_vkCmdSetCullModeEXT eds_cull; static PFN_vkCmdSetFrontFaceEXT eds_front;
static PFN_vkCmdSetDepthBiasEnableEXT eds_bias_enable;
/* Experiment words (TOTK_VK_EXP, <files>/exp.live): whole words, separated by anything but letters, digits and '_', so
   "barrier" does not also switch on "nofeedbackbarrier". A word may be followed by "@A-B" (a draw range), a word ending
   in '=' by its value, and with number=1 by digits ("restart20"). Returns what follows the word, or NULL. */
static int exp_word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static const char *exp_word(const char *list, const char *word, int number) {
    size_t n = strlen(word); int valued = n && word[n - 1] == '=';
    for (const char *p = list; p && (p = strstr(p, word)); p++) {
        if (p > list && exp_word_char(p[-1])) continue;
        const char *after = p + n;
        if (number) while (*after >= '0' && *after <= '9') after++;
        if (!valued && exp_word_char(*after)) continue;
        return p + n;
    }
    return NULL;
}
/* An experiment word in TOTK_VK_EXP (read once per call site: the callers cache it). */
static int exp_env(const char *word) { return exp_word(getenv("TOTK_VK_EXP"), word, 0) != NULL; }
/* TOTK_VK_INPLACE (on unless 0; 2 adds the capture check): uniform, storage, vertex and index buffers in the zero-copy
   pools are bound in place, without the forward and pending-write lookups and without copies, and storage writes go
   into the game's memory directly; the game's own synchronization (its barriers between passes, a barrier after each
   dispatch, its fences) orders them, as on the console. The uniform blocks the game reuses before the GPU has read them
   (TOTK_VK_INPLACE_KEEP=<hex address>,...: the uniform probe's list) keep the capture path. The known holes are the
   early-reuse blocks not on the list and GPU writes read through the paths that still snapshot (pools under 64 KiB). */
static int inplace_state = -1; static uint64_t inplace_keep[32]; static unsigned inplace_nkeep;
/* Uniform blocks stay on the submit capture (TOTK_VK_INPLACE_UBO=1 binds them in place): the capture is right by construction
   (the block as it was at the submit call, as the console's GPU read it), and it measures faster (still scene: translation
   42.3 M instructions a frame against 45.9 in place; the capture's descriptors repeat, in-place ones do not), while the
   completion-time probe (TOTK_VK_INPLACE=2) finds blocks the game rewrites before the GPU is done with them (256- and
   672-byte per-object blocks, up to 13 a frame in bursts). Vertex, index and storage buffers have no capture: in place. */
static int inplace_ubo_on = 0;
static int inplace_init(void) {
    const char *e = getenv("TOTK_VK_INPLACE"); inplace_state = e && atoi(e) == 2 ? 2 : !(e && atoi(e) == 0);
    /* the default list: the uniform probe's early-reuse blocks */
    const char *k = getenv("TOTK_VK_INPLACE_KEEP"); if (!k) k = "3a4a76000,3a4a76400,3a876f000";
    { const char *u = getenv("TOTK_VK_INPLACE_UBO"); inplace_ubo_on = u && atoi(u) > 0; }
    while (k && *k && inplace_nkeep < 32) { inplace_keep[inplace_nkeep++] = strtoull(k, (char **)&k, 16); if (*k == ',') k++; else break; }
    if (inplace_state) LOG("[vulkan-inplace] on: buffers bound in place from the zero-copy pools (%u blocks kept on the capture path)", inplace_nkeep);
    return inplace_state;
}
static inline int inplace_on(void) { return inplace_state >= 0 ? inplace_state : inplace_init(); }
static int inplace_kept(uintptr_t address) { for (unsigned i = 0; i < inplace_nkeep; i++) if ((address & ~0xfffull) == (inplace_keep[i] & ~0xfffull)) return 1; return 0; }
static uint64_t inplace_ubo, inplace_ssbo, inplace_ssbo_w, inplace_vbuf, inplace_index, inplace_fallback;
static void inplace_classify(uint64_t addr, uint64_t len);
enum { IF_UBO_POOL, IF_UBO_DIRTY, IF_UBO_KEPT, IF_UBO_ALIGN, IF_SSBO_POOL, IF_SSBO_ALIGN, IF_VBUF_POOL, IF_INDEX_POOL, IF_INDEX_KIND, IF_COUNT_ }; static uint64_t inplace_why[IF_COUNT_];
/* inline uniform updates (OP_UPDATE_UB) a frame, and how many arrive while a pass is open */
static uint64_t inplace_updates, inplace_updates_inpass;
/* a draw of the open pass wrote storage in place: a full barrier when the pass ends (its readers come later, in place too) */
static int inplace_pass_writes; static uint64_t inplace_pass_barriers;
static VkBuffer zc_buffer_at(uint64_t addr, uint64_t len, uint64_t *off);
#include "nvn_vulkan_defer.inc"
/* VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf listed (zero-copy guest memory) */
static int ext_memory_fd, ext_dma_buf;
/* VK_EXT_robustness2 listed (LSFG frame generation needs its nullDescriptor) */
static int ext_robustness2;
/* robustBufferAccess2 on: reads past a uniform descriptor's range return zeros (exact-range capture binds) */
static int ubo_robust_ok;
/* VK_EXT_attachment_feedback_loop_layout enabled: a draw that samples its own colour target stays in the render pass,
   behind an in-pass barrier (nvn_vulkan_feedback.inc). */
static int ext_fbloop, fbloop_ok;
/* VK_EXT/ARM_rasterization_order_attachment_access, asked for by feedback-loop pipelines: overlapping fragments of one
   decal draw then read each other's target writes in order. The Switch gives TOTK that; without it the pebble decals
   show cache-line "ants" (Turnip on a7xx+ flushes per overlap only for this, not for feedback loops). Costs ~0.9 ms
   GPU a frame (the overlapping meshes' flushes), so it is off unless TOTK_VK_RORDER=1. */
static int ext_rorder, rorder_ok;
/* VK_EXT_custom_border_color without a format: a sampler clamps to the game's own border colour. TotK's grass map
   clamps to 0.5 grey; as transparent black the grass lay flat away from Link and cutting it left no cut. Without the
   extension (or past maxCustomBorderColorSamplers) the nearest of Vulkan's three fixed colours is used.
   TOTK_VK_CUSTOM_BORDER=0 keeps the fixed colours. */
static int ext_custom_border, custom_border_ok;
static uint32_t custom_border_max, custom_border_used;
static int ext_gpl, ext_pipeline_library, gpl_state;
static int ext_descriptor_indexing;
static int bindless_on(void);static void bindless_create(void);
static int lsfg_device_ok;
static int fsr_device_ok;
static VkPhysicalDeviceLimits gpu_limits;
static uint32_t gpu_subgroup_size;
/* ballot in vertex shaders (Turnip yes, Qualcomm's driver no): the translator keeps the game's vertex counter as written */
static int gpu_vs_subgroups;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkExtent2D screen_extent;
static VkFormat screen_format;
/* Aspect-fit a sw x sh picture into dst, centred: out = {x, y, w, h} in dst pixels. */
static void dm_android_present_fit(int sw, int sh, int dst_w, int dst_h, int out[4]) {
    double scale = (double)dst_w / sw; if (scale * sh > dst_h) scale = (double)dst_h / sh;
    int dw = sw * scale, dh = sh * scale;
    out[0] = (dst_w - dw) / 2; out[1] = (dst_h - dh) / 2; out[2] = dw; out[3] = dh;
}
/* the swapchain images can be rendered to (the overlay's render pass; Qualcomm's driver fails the frame otherwise) */
static int screen_color_attachment;
static VkImage *screen_images;
static uint8_t *screen_initialized;
/* screen_acquired: the ring entry the present in progress acquires with */
static VkSemaphore screen_acquired, screen_rendered;
/* Acquire semaphores: a ring. A binary semaphore may be reused only once the submission that waited on it completed, so
   with one semaphore every present would first wait on the CPU for the previous present's blit. With frame generation
   two to four presents follow each other within a frame, and the second's wait would cover LSFG's GPU work as well (5.9 ms
   a frame on the translation thread). Each ring entry remembers the ticket of the submission that consumed it; by the
   time it comes round again that work is long done. */
#define SCREEN_ACQUIRE_RING 8
static VkSemaphore screen_acquire_ring[SCREEN_ACQUIRE_RING]; static uint64_t screen_acquire_ring_ticket[SCREEN_ACQUIRE_RING]; static unsigned screen_acquire_slot;
static VkSemaphore *screen_present_wait;
static uint64_t screen_acquire_ticket;
/* the translation thread's time blocked in the presents since the last reading (lsfg_adaptive_count) */
static uint64_t present_block_ns;
static int screen_submission;
static uint32_t screen_count;
/* TOTK_VK_PRESENT_MODE=fifo (default) | mailbox | immediate: a probe. The blit into a swapchain image waits for the
   image's acquire semaphore, and everything submitted after it (the next frame) queues behind that wait on the
   in-order queue. */
static VkPresentModeKHR screen_present_mode(void) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("TOTK_VK_PRESENT_MODE"); mode = e && !strcmp(e, "mailbox") ? 1 : e && !strcmp(e, "immediate") ? 2 : 0; if (mode) LOG("[vulkan-wsi] present mode %s (TOTK_VK_PRESENT_MODE)", e); }
    return mode == 1 ? VK_PRESENT_MODE_MAILBOX_KHR : mode == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
}
// WSI recovery state (see the note before mtl_present). Only two kinds of code ever change the swapchain, and they
// exclude each other: (1) the game thread inside a present (within gfx_enter) — rebuild at the top of a frame on
// screen_dirty, rebuild after a failed acquire, release on surface loss; (2) a callback thread after
// dm_gfx_suspend(1) (attach for a new window / detach when the window is destroyed), when the game thread is parked
// outside NVN. screen_lock only separates (2) from backend initialisation (on the game thread, not inside
// gfx_enter): a window callback that arrives while initialisation builds the surface and swapchain waits for it.
// "Game thread" in this file's notes on presenting / top-of-frame rebuilds / acquire means the presenting thread: the
// game thread in sync mode, the translation thread in translation-thread mode (TOTK_ASYNC_REPLAY=1; mtl_present runs
// there). Backend initialisation is on the game thread in both modes (front_run calls mtl_init before any game code
// runs; the translation thread is created later, by x_start's pthread_once on one of the game's NVN calls).
// "Within gfx_enter" in (1) and "waits out the one in flight" in the mtl_screen_attach note also describe sync mode.
// In translation-thread mode (1) and (2) still exclude each other:
// dm_gfx_suspend(1) blocks the submit / present enqueues wrapped by gfx_enter, waits out in-flight calls, then x_drain()s
// until every queued job has run (g_xq_done advances only after a job finishes). The translation thread may still run
// XJ_TEX_RELEASE / XJ_FRAME jobs while suspended; they do not touch WSI state.
static pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;
// Under screen_lock: initialisation finished the surface/swapchain
static int screen_live;
// Rebuild the swapchain at the top of the next frame
static _Atomic int screen_dirty;
// Reason bits of screen_dirty (for the log).
enum { SCREEN_DIRTY_RESIZED = 1, SCREEN_DIRTY_OUT_OF_DATE = 2, SCREEN_DIRTY_EXTENT = 4, SCREEN_DIRTY_RETRY = 8 };
// Acquire wait limit. A normal FIFO acquire waits at most two vsyncs (≈ 33 ms); waiting a full second means nobody is
// consuming the window's buffer queue (being torn down, on the way to the background), so this frame is not
// presented and a rebuild is marked instead of blocking forever, which would starve the "ask for a save first" step of
// the window-destroyed callback of game ticks.
#define SCREEN_ACQUIRE_TIMEOUT_NS 1000000000ull
static VkSurfaceTransformFlagBitsKHR screen_transform;
// Present number of the last extent check after SUBOPTIMAL
static uint64_t screen_caps_checked;
static uint64_t screen_lost_retry_ns;
// Raw window size at the last build (0 = the window gave none, currentExtent was used)
static int screen_built_w, screen_built_h;
// Retire list: swapchains taken out of service and their present-wait semaphores are destroyed only once there is
// evidence that the presentation engine is done with them (see screen_retire_collect). On the same surface the
// evidence follows the Khronos swapchain_recreation sample: an image of the current (newest) swapchain that has already
// been presented is acquired again and the blit submission that consumed that acquire has completed => its previous
// present has completed => the presents of older swapchains queued before it have completed too (relying, like the
// sample, on presents completing in submission order). It is not limited to the image of the first present: the
// acquire order is up to the implementation, so one given image may never come back, but a swapchain has count images
// and every successful acquire is presented, so within count+1 successful acquires one image is acquired for the second
// time (pigeonhole) - as long as frames are presented, the evidence arrives. The list never destroys anything without
// evidence: once SCREEN_RETIRE_DEFER entries are pending, a rebuild that is only a size change waits (the current
// swapchain still presents and the compositor scales it to the window) until the evidence has reclaimed the list; with
// a current swapchain, a rebuild that cannot wait (out of date) and SCREEN_RETIRE_MAX entries pending goes through the
// surface-lost path instead (screen_release, see the note there). It is changed only where the swapchain is changed
// (the game thread inside a present, or a callback thread after dm_gfx_suspend(1)), so it needs no lock of its own.
#define SCREEN_RETIRE_MAX 16
#define SCREEN_RETIRE_DEFER 4
typedef struct { VkSwapchainKHR handle; VkSemaphore *present_wait; uint32_t count; } screen_retired;
static screen_retired screen_retire[SCREEN_RETIRE_MAX];
static unsigned screen_retire_count;
// Image index of the evidence (for the log)
static uint32_t screen_retire_image = UINT32_MAX;
// Completion ticket of the blit that consumed the re-acquire of an already presented image
static uint64_t screen_retire_ticket;
// The window size was clamped into the surface min/max at the last build
static int screen_built_clamped;
// Present number of the last capability re-read while clamped
static uint64_t screen_clamp_checked;
// WSI result counters (written by the game thread only; read by logging).
enum { WSI_OK, WSI_SUBOPTIMAL, WSI_OUT_OF_DATE, WSI_TIMEOUT, WSI_SURFACE_LOST, WSI_RESULTS };
static uint64_t wsi_acquire[WSI_RESULTS], wsi_present[WSI_RESULTS], wsi_recreates, wsi_recreate_failures, wsi_surface_rebuilds, wsi_skipped;
static uint64_t wsi_retired, wsi_retire_freed, wsi_retire_overflow, wsi_retire_deferred, wsi_same_size_skips;
// Cost of the window size queries per frame (and in the rebuild decision and the SUBOPTIMAL check)
// (ANativeWindow_getWidth/Height): they go through window->query and may reach the producer (a binder call to
// SurfaceFlinger on API 29-30), so it is measured and printed with the 600-frame line instead of assumed free. The one
// query while building a swapchain is not counted.
static uint64_t wsi_wq_ns_sum, wsi_wq_ns_max, wsi_wq_n;
#ifdef __ANDROID__
// The window the current surface belongs to; the backend holds its own reference (screen_bind_window)
static struct ANativeWindow *screen_window;
#include "nvn_vulkan_pacing.inc"
static void screen_request_rate(struct ANativeWindow *window) {
    /* SurfaceFlinger's content detection otherwise matches the panel to our ~60 posts per second (60 Hz), which in turn caps
       us at 60 through vsync. TOTK_DISPLAY_HZ: the rate we ask for (default 120; 0 leaves it to the system). */
    if (window) { const char *e = getenv("TOTK_DISPLAY_HZ"); float hz = e ? (float)atof(e) : 120.0f;
        /* libnativewindow, API 30 */
        static int (*set_rate)(ANativeWindow *, float, int8_t);
        if (!set_rate) { void *lib = dlopen("libnativewindow.so", RTLD_NOW); set_rate = lib ? (int (*)(ANativeWindow *, float, int8_t))dlsym(lib, "ANativeWindow_setFrameRate") : NULL; }
        if (hz > 0 && set_rate) { int r = set_rate(window, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT); LOG("[vulkan-wsi] requested %.0f Hz from the display (result %d)", hz, r); } }
}
// Rebind the window: take the new window's reference before dropping the old one. The backend holds its own
// reference, so the pointer is still valid when retrying on the same window after a surface loss.
static void screen_bind_window(struct ANativeWindow *window) {
    if (window) ANativeWindow_acquire(window);
    if (screen_window) ANativeWindow_release(screen_window);
    screen_window = window;
    screen_request_rate(window);
}
#endif
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pools_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completion_condition = PTHREAD_COND_INITIALIZER;
static pool_range *pools;
/* TOTK has many memory pools and mtl_addr runs several times per draw, so a linear walk of the pool list would be the
   biggest single cost of the translation thread (~15%). A sorted array of the pools (rebuilt when one is added or
   removed) is searched by start address instead. */
static pool_range **pool_sorted; static size_t pool_sorted_n, pool_sorted_cap;
/* advances with every rebuild (under pools_lock): mtl_addr's memo is valid while it does not */
static _Atomic uint64_t pool_gen;
static int pool_cmp(const void *x, const void *y) { uintptr_t a = (uintptr_t)(*(pool_range *const *)x)->base, b = (uintptr_t)(*(pool_range *const *)y)->base; return a < b ? -1 : a > b; }
static void pool_sorted_rebuild_locked(void) {
    size_t n = 0; for (pool_range *p = pools; p; p = p->next) n++;
    if (n > pool_sorted_cap) { pool_sorted_cap = n + n / 2 + 16; pool_sorted = realloc(pool_sorted, pool_sorted_cap * sizeof *pool_sorted); if (!pool_sorted) die("pool index"); }
    n = 0; for (pool_range *p = pools; p; p = p->next) pool_sorted[n++] = p;
    qsort(pool_sorted, n, sizeof *pool_sorted, pool_cmp); pool_sorted_n = n;
    atomic_fetch_add_explicit(&pool_gen, 1, memory_order_release);
}
static post_op *post_first, **post_tail;
static post_op *post_copy_first,**post_copy_tail;
static int pending_host_sync;
static uint64_t post_order;
/* advances with every post that writes guest memory (post_register_range: copies, GPU outputs, events, counters), never
   reset: a uniform read served from the submission's capture stays valid until one of these is recorded, while posts
   that only hold resources (kind 3, several a draw) cannot change its bytes (uniform_read_cached_read) */
static uint64_t post_write_order;
static post_op *const *forward_scope_marker;
static _Atomic uint64_t submitted, completed, pool_adds;
static uint64_t copy_bytes,forward_copy_bytes;
static uint64_t samples_passed, presents;
/* the game's synchronization points so far (its barriers and its submissions): nvn_vulkan_tex.inc's TOTK_VK_SYNCCHECK */
static uint64_t game_sync_epoch;
/* TOTK_VK_STATELEAK census state (see stateleak_draw below the replay) */
enum { RM_RT = 1u << 0, RM_PROGRAM = 1u << 1, RM_BLEND = 1u << 2, RM_COLOR = 1u << 3, RM_MASK = 1u << 4, RM_MS = 1u << 5, RM_POLY = 1u << 6, RM_DEPTH = 1u << 7,
       RM_VIEWPORT = 1u << 8, RM_SCISSOR = 1u << 9, RM_DEPTHRANGE = 1u << 10, RM_TEXPOOL = 1u << 11, RM_SAMPPOOL = 1u << 12, RM_BLENDCOLOR = 1u << 13,
       RM_POLYOFFSET = 1u << 14, RM_STENCIL = 1u << 15, RM_RENDERENABLE = 1u << 16, RM_DEPTHCLAMP = 1u << 17, RM_RASTERDISCARD = 1u << 18, RM_RENDERCOND = 1u << 19, RM_COUNT_ = 20 };
typedef struct { uint32_t set; uint16_t ubo[6], ssbo[6], vattr, vstream, vbuf; uint64_t tex[6][2]; } rec_masks;
static rec_masks rec_mask;
static int stateleak_state = -1;
static inline int stateleak_on(void) { if (stateleak_state < 0) { const char *e = getenv("TOTK_VK_STATELEAK"); stateleak_state = e && atoi(e) > 0; } return stateleak_state; }
static struct { uint64_t draws, leaking, called, called_leaking, recordings, cat[RM_COUNT_], ubo, ssbo, tex, vattr, vstream, vbuf; int piece_depth; uint64_t main_ops[OP_COUNT_], main_draws, piece_draws;
    /* per piece ordinal in the frame: draws, draws inheriting a binding slot (not pools / multisample) */
    int piece; uint64_t pdraws[32], pleak[32], pcount[32];
    /* dependencies: forwards and image reads whose writer is in the same piece, another piece of the same submission, or
           an older submission */
    uint64_t submission, fwd_same, fwd_cross, fwd_older, img_same, img_cross, img_older;
    /* detail: for the frames after each summary, log every inherited slot (which, bound to what) and the root's own ops */
    uint64_t detail_until; } sl;
#define SL_DETAIL() (stateleak_state > 0 && presents < sl.detail_until)
static int recording;
typedef struct nvn_state {
    p_set_rt rt; const prog_t *program;
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t textures[6][128], samplers[6][128], images[6][32];
    va_st attributes[16]; vs_st streams[16]; NVNbufferRange vertices[16];
    color_st color; chmask_st mask; blend_st blend[8]; ms_st multisample; poly_st polygon; ds_st depth;
    p_scissor scissor; p_viewports viewports; p_depth_range depth_range;
    VkStencilOpState stencil_faces[2]; p_poly_offset polygon_offset; p_blend_color blend_color;
    const dpool_t *texture_pool, *sampler_pool; int64_t copy_stride;
    int depth_clamp, render_disabled, raster_discard, render_cond_off;
} nvn_state;
static nvn_state state;
/* The pure state ops: the packet's data moved into a tracked state, nothing else. The replay applies them to `state`
   (with its own side effects around the call: masks, driver-block validity, statistics); a predecode (nvn_vulkan_plan.inc)
   applies them to a private state. Returns 0 for an op that is not one of them. */
static int nvn_state_apply(nvn_state *st, unsigned op, const uint8_t *a) {
    switch (op) {
        case OP_SET_TEX_POOL: st->texture_pool = ((const p_set_pool *)a)->pool; return 1;
        case OP_SET_SAMP_POOL: st->sampler_pool = ((const p_set_pool *)a)->pool; return 1;
        case OP_BIND_PROGRAM: st->program = ((const p_bind_program *)a)->prog; return 1;
        case OP_SET_RT: st->rt = *(const p_set_rt *)a; return 1;
        case OP_DEPTH_CLAMP: st->depth_clamp = ((const p_flag *)a)->value != 0; return 1;
        case OP_RENDER_ENABLE: st->render_disabled = ((const p_flag *)a)->value == 0; st->render_cond_off = 0; return 1;
        case OP_RASTER_DISCARD: st->raster_discard = ((const p_flag *)a)->value != 0; return 1;
        case OP_SCISSOR: st->scissor = *(const p_scissor *)a; return 1;
        case OP_VIEWPORTS: { const p_viewports *v = (const void *)a; if (v->first < 0 || v->n < 0 || v->first + v->n > 16) die("NVN viewport bounds"); for (int i = 0; i < v->n; ++i) memcpy(st->viewports.v[v->first + i], v->v[i], sizeof v->v[i]); st->viewports.n = v->first + v->n; return 1; }
        case OP_DEPTH_RANGE: st->depth_range = *(const p_depth_range *)a; return 1;
        case OP_BIND_STATE: { const p_bind_state *b = (const void *)a;
            switch (b->kind) {
                case 0: { blend_st blend; memcpy(&blend, b->bytes, sizeof blend); st->blend[blend.target] = blend; break; }
                case 1: memcpy(&st->color, b->bytes, sizeof st->color); break;
                case 2: memcpy(&st->mask, b->bytes, sizeof st->mask); break;
                case 3: memcpy(&st->multisample, b->bytes, sizeof st->multisample); break;
                case 4: memcpy(&st->polygon, b->bytes, sizeof st->polygon); break;
                case 5: memcpy(&st->depth, b->bytes, sizeof st->depth); break;
                default: die("NVN state kind %d", b->kind);
            } return 1; }
        case OP_BIND_UBO: case OP_BIND_SSBO: { const p_bind_buf *b = (const void *)a; if (b->stage < 0 || b->stage >= 6 || b->index < 0 || b->index >= 16) die("NVN buffer binding bounds");
            *(op == OP_BIND_UBO ? &st->ubo[b->stage][b->index] : &st->ssbo[b->stage][b->index]) = *b; return 1; }
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { const p_bind_handles *b = (const void *)a; int max = op == OP_BIND_IMG ? 32 : 128; if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->first + b->n > max) die("NVN handle binding bounds");
            uint64_t *dst = op == OP_BIND_IMG ? st->images[b->stage] : op == OP_BIND_TEX ? st->textures[b->stage] : st->samplers[b->stage]; memcpy(dst + b->first, b->h, b->n * sizeof *dst); return 1; }
        case OP_BIND_VATTR: { const p_bind_vattr *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex attribute bounds"); memcpy(st->attributes + b->first, b->st, b->n * sizeof *st->attributes); return 1; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex stream bounds"); memcpy(st->streams + b->first, b->st, b->n * sizeof *st->streams); return 1; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex buffer bounds"); memcpy(st->vertices + b->first, b->r, b->n * sizeof *st->vertices); return 1; }
        case OP_COPY_STRIDE: st->copy_stride = ((const p_copy_stride *)a)->stride; return 1;
        case OP_BLEND_COLOR: st->blend_color = *(const p_blend_color *)a; return 1;
        case OP_POLY_OFFSET: st->polygon_offset = *(const p_poly_offset *)a; return 1;
        case OP_STENCIL: { const p_stencil *s = (const void *)a; for (int i = 0; i < 2; ++i) if (s->face & (1 << i)) { if (s->which == 0) st->stencil_faces[i].compareMask = s->value_mask; else if (s->which == 1) st->stencil_faces[i].writeMask = s->mask; else st->stencil_faces[i].reference = s->ref; } return 1; }
        default: return 0;
    }
}
/* Validation messages (TOTK_VK_DEBUG=1): logged once per message id, at most 200 in all. */
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                                       const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user; static int32_t seen[256]; static unsigned nseen, total;
    for (unsigned i = 0; i < nseen; i++) if (seen[i] == data->messageIdNumber) return VK_FALSE;
    if (nseen < 256) seen[nseen++] = data->messageIdNumber;
    if (total++ < 200) LOG("[vulkan-debug] %s %s", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning", data->pMessage);
    return VK_FALSE;
}

#include "nvn_vulkan_shader_variant.inc"
#include "nvn_vulkan_pipeline_cache.inc"
#include "nvn_vulkan_cpu_phases.inc"

#ifdef __ANDROID__
// Release an unsubmitted context only on initialization failure, then fully fall back; never switch a live driver.
static int initialize_driver_fallback(const char *reason) {
    if(!vk_api.using_custom)return 0;
    if(device){if(!vk_api.vkDestroyDevice)return 0;vkDestroyDevice(device,NULL);device=VK_NULL_HANDLE;}
    if(surface){if(!vk_api.vkDestroySurfaceKHR)return 0;vkDestroySurfaceKHR(instance,surface,NULL);surface=VK_NULL_HANDLE;}
    if(instance){if(!vk_api.vkDestroyInstance)return 0;vkDestroyInstance(instance,NULL);instance=VK_NULL_HANDLE;}
    physical=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;
    return dm_vk_driver_fallback(&vk_api,reason);
}
#endif

// Swapchain plus its semaphores and images, as its own function so returning from the background can rebuild it
// from a new ANativeWindow: Android always destroys the window when the activity stops, and the window texture is
// created only once at startup (dm_uiscreen.c). The caller must own the swapchain exclusively: either the game thread
// inside a present (within gfx_enter), or with graphics suspended (dm_gfx_suspend(1)), or initialisation. What is
// built goes into out and the caller installs it with screen_install_chain (the caller retires the old swapchain, see
// screen_retire_chain). A non-null old is chained through oldSwapchain: per the spec, once vkCreateSwapchainKHR has been
// called, old is retired whether or not creation succeeds, so *old_consumed is set to 1 and the caller must put it on
// the retire list and never present to it again; a failure returned before vkCreateSwapchainKHR is called (surface
// capabilities unavailable, zero-sized window) leaves old alone. Returns VK_SUCCESS; a recoverable failure (surface
// lost, window in use, out of date, zero-sized window) is returned as is with nothing new left behind, for the caller
// to retry later; any other failure dies (fail closed, no guessing).
typedef struct { VkSwapchainKHR handle; VkExtent2D extent; uint32_t count; VkImage *images; uint8_t *initialized; VkSemaphore *present_wait; VkSurfaceTransformFlagBitsKHR transform; int window_w, window_h, clamped; } screen_chain;
static int wsi_recoverable(VkResult r) {
    return r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
}
#define WSI_TRY(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { if (wsi_recoverable(r_)) return r_; die("Vulkan %s failed: %d", #expr, r_); } } while (0)
static VkResult screen_build_chain(struct ANativeWindow *window, VkSwapchainKHR old, int *old_consumed, screen_chain *out) {
    if (old_consumed) *old_consumed = 0;
    memset(out, 0, sizeof *out);
    if (!surface || !device) return VK_ERROR_SURFACE_LOST_KHR;
    VkSurfaceCapabilitiesKHR cap; WSI_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &cap));
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) die("Vulkan surface has no transfer destination support");
    uint32_t nf; WSI_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, NULL)); VkSurfaceFormatKHR *formats = calloc(nf ? nf : 1, sizeof *formats); if (!formats) die("Vulkan surface format allocation");
    VkResult listed = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, formats);
    if (listed != VK_SUCCESS || !nf) { free(formats); if (wsi_recoverable(listed)) return listed; die("Vulkan surface formats failed: %d count=%u", listed, nf); }
    VkSurfaceFormatKHR format = formats[0]; for (unsigned i = 0; i < nf; ++i) if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { format = formats[i]; break; } free(formats);
    screen_format = format.format;
    // The size follows the window at this moment; only when the window cannot give one (0 / error) does the surface's
    // currentExtent apply, then it is clamped into min/max (the spec only requires imageExtent within min..max; Android
    // scales the buffer to the window). Inside onNativeWindowResized the window already reports the new size while
// currentExtent lags 3-9 frames, so building from it would rebuild the same size for nothing and then stretch the old
// size for a few frames.
    VkExtent2D extent = cap.currentExtent; int window_w = 0, window_h = 0;
#ifdef __ANDROID__
    if (window) { window_w = ANativeWindow_getWidth(window); window_h = ANativeWindow_getHeight(window); }
    if (window_w > 0 && window_h > 0) extent = (VkExtent2D){(uint32_t)window_w, (uint32_t)window_h};
#else
    (void)window;
#endif
    if (extent.width == UINT32_MAX) die("Vulkan surface reports no current extent and no window size");
    // A window reporting zero size mid-transition cannot be used yet; the caller retries later.
    if (!extent.width || !extent.height) return VK_ERROR_OUT_OF_DATE_KHR;
    if (extent.width < cap.minImageExtent.width) extent.width = cap.minImageExtent.width;
    if (extent.height < cap.minImageExtent.height) extent.height = cap.minImageExtent.height;
    if (cap.maxImageExtent.width && extent.width > cap.maxImageExtent.width) extent.width = cap.maxImageExtent.width;
    if (cap.maxImageExtent.height && extent.height > cap.maxImageExtent.height) extent.height = cap.maxImageExtent.height;
    /* One image more than the minimum (4 on the Odin: 5). With frame generation the pictures of a frame are presented in
       a burst; more images do not help (measured with 6, 7, 8 and 11 on LSFG x2 and x4): the compositor paces the burst
       in vkQueuePresentKHR either way, ~4 ms a picture, while the game is at its cap and the translation thread has
       nothing else to do. */
    uint32_t images = cap.minImageCount + 1; if (cap.maxImageCount && images > cap.maxImageCount) images = cap.maxImageCount;
    LOG("[vulkan-swapchain] images: %u requested (minimum %u, maximum %u)", images, cap.minImageCount, cap.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; if (!(cap.supportedCompositeAlpha & alpha)) alpha = cap.supportedCompositeAlpha & -cap.supportedCompositeAlpha;
    // The final blit produces an upright window-space image, not a pre-rotated image; the compositor handles display
    // orientation. Android therefore keeps reporting SUBOPTIMAL while currentTransform is 90/270: that is not a reason
    // to rebuild (see screen_note_suboptimal).
    VkSurfaceTransformFlagBitsKHR transform = (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    LOG("[vulkan-surface-transform] supported=%#x current=%#x selected=%#x extent=%ux%u (window %dx%d, currentExtent %ux%u) upright_blit=1 old_swapchain=%d",cap.supportedTransforms,cap.currentTransform,transform,extent.width,extent.height,window_w,window_h,cap.currentExtent.width,cap.currentExtent.height,old != VK_NULL_HANDLE);
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = images, .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (cap.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = transform, .compositeAlpha = alpha, .presentMode = screen_present_mode(), .clipped = VK_TRUE, .oldSwapchain = old};
    // old is retired from this call on
    if (old_consumed) *old_consumed = old != VK_NULL_HANDLE;
    VkSwapchainKHR created = VK_NULL_HANDLE; VkResult made = vkCreateSwapchainKHR(device, &ci, NULL, &created);
    if (made != VK_SUCCESS) { if (wsi_recoverable(made)) return made; die("Vulkan vkCreateSwapchainKHR failed: %d", made); }
    screen_color_attachment = (ci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    uint32_t count = 0; VK_CHECK(vkGetSwapchainImagesKHR(device, created, &count, NULL)); LOG("[vulkan-swapchain] images: %u obtained", count);
    VkImage *list = calloc(count ? count : 1, sizeof *list); uint8_t *initialized = calloc(count ? count : 1, 1); if (!list || !initialized) die("Vulkan swapchain image allocation");
    VK_CHECK(vkGetSwapchainImagesKHR(device, created, &count, list));
    // Reacquiring/waiting on the same image proves its present wait consumed; a submission fence does not.
    VkSemaphore *present_wait=calloc(count ? count : 1,sizeof *present_wait);if(!present_wait)die("Vulkan present semaphore allocation");
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    // The acquire semaphores belong to no swapchain: created once and kept. Each present first waits for the blit that
    // last consumed its ring entry (present_one) and a failed acquire signals nothing, so whenever a swapchain is torn
    // down or rebuilt none has a pending signal or wait and none need be recreated.
    if (!screen_acquire_ring[0]) { for (unsigned i = 0; i < SCREEN_ACQUIRE_RING; i++) VK_CHECK(vkCreateSemaphore(device, &sem, NULL, &screen_acquire_ring[i])); screen_acquired = screen_acquire_ring[0]; }
    for(unsigned i=0;i<count;i++)VK_CHECK(vkCreateSemaphore(device,&sem,NULL,&present_wait[i]));
    int known_window = window_w > 0 && window_h > 0;
    *out = (screen_chain){created, extent, count, list, initialized, present_wait, transform, known_window ? window_w : 0, known_window ? window_h : 0,
                          known_window && (extent.width != (uint32_t)window_w || extent.height != (uint32_t)window_h)};
    LOG("[vulkan] Android game surface %ux%u images=%u", extent.width, extent.height, count);
    return VK_SUCCESS;
}
// Install a freshly built swapchain; its retirement evidence starts afresh (evidence image, re-acquire ticket; see screen_retire).
static void screen_install_chain(const screen_chain *c) {
    swapchain = c->handle; screen_extent = c->extent; screen_count = c->count; screen_images = c->images; screen_initialized = c->initialized;
    screen_present_wait = c->present_wait; screen_transform = c->transform; screen_rendered = VK_NULL_HANDLE;
    screen_built_w = c->window_w; screen_built_h = c->window_h;
    screen_built_clamped = c->clamped; screen_clamp_checked = 0;
    screen_retire_image = UINT32_MAX; screen_retire_ticket = 0;
}
// Take the current swapchain off the global state (nothing destroyed): the image table stays in the returned value;
// the caller retires or destroys the handle and the semaphores.
static screen_chain screen_take_chain(void) {
    screen_chain c = {swapchain, screen_extent, screen_count, screen_images, screen_initialized, screen_present_wait, screen_transform, screen_built_w, screen_built_h, screen_built_clamped};
    swapchain = VK_NULL_HANDLE; screen_extent = (VkExtent2D){0, 0}; screen_count = 0; screen_images = NULL; screen_initialized = NULL;
    screen_present_wait = NULL; screen_rendered = VK_NULL_HANDLE; screen_retire_image = UINT32_MAX; screen_retire_ticket = 0; screen_built_w = screen_built_h = 0; screen_built_clamped = 0;
    return c;
}

// Idle the queue: finish every in-flight submission (including the blit that references a swapchain image) before
// tearing the swapchain down. It covers only fenced submissions, not the presentation engine's use of the semaphores
// vkQueuePresentKHR waits on (the spec; Khronos swapchain_semaphore_reuse), so a rebuild on the same surface does not
// rely on it but on the retire list.
static void screen_queue_idle(void) {
    if (!device || !queue) return;
    pthread_mutex_lock(&queue_lock); VK_CHECK(vkQueueWaitIdle(queue)); pthread_mutex_unlock(&queue_lock);
}
static void screen_retired_destroy(screen_retired *r) {
    /* the overlay's views of its images go first: a later swapchain's image may get the same handle */
    if (r->handle) {
        VkImage images[16]; uint32_t n = 16;
        if (vkGetSwapchainImagesKHR(device, r->handle, &n, images) >= 0) for (uint32_t i = 0; i < n; i++) overlay_forget_image(images[i]);
        vkDestroySwapchainKHR(device, r->handle, NULL);
    }
    for (uint32_t i = 0; i < r->count; i++) if (r->present_wait && r->present_wait[i]) vkDestroySemaphore(device, r->present_wait[i], NULL);
    free(r->present_wait); *r = (screen_retired){0};
}
// Retire: the handle and the present-wait semaphores go on the list; the image table is freed here (the images belong
// to the swapchain). The list is never full on entry: screen_recreate stops chaining new swapchains once
// SCREEN_RETIRE_MAX entries are pending (it takes the surface-lost path instead), so a full list here is a program
// error and stops the run (fail closed: nothing is ever destroyed without evidence).
static void screen_retire_chain(screen_chain *c) {
    free(c->images); free(c->initialized); c->images = NULL; c->initialized = NULL;
    if (!c->handle && !c->present_wait) return;
    if (screen_retire_count == SCREEN_RETIRE_MAX) die("[vulkan-wsi] retire list full (%u entries) on retire: screen_recreate must not chain a new swapchain here", screen_retire_count);
    screen_retire[screen_retire_count++] = (screen_retired){c->handle, c->present_wait, c->count}; wsi_retired++;
    c->handle = VK_NULL_HANDLE; c->present_wait = NULL; c->count = 0;
}
// Top of a frame, game thread: once the current swapchain's retirement evidence has arrived (see the note on
// screen_retire), destroy every old swapchain and semaphore on the list.
static void screen_retire_collect(void) {
    if (!screen_retire_count || !screen_retire_ticket || !mtl_wait(screen_retire_ticket, 0)) return;
    unsigned chains = 0, semaphores = 0;
    for (unsigned i = 0; i < screen_retire_count; i++) { chains += screen_retire[i].handle != VK_NULL_HANDLE; semaphores += screen_retire[i].count; screen_retired_destroy(&screen_retire[i]); }
    wsi_retire_freed += screen_retire_count;
    if (wsi_retire_freed <= 32 || !(wsi_retire_freed % 64))
        LOG("[vulkan-wsi] retired %u entries (%u swapchains, %u present semaphores) after image %u of the current swapchain was re-acquired and its blit completed (ticket %llu)", screen_retire_count, chains, semaphores, screen_retire_image, (unsigned long long)screen_retire_ticket);
    screen_retire_count = 0; screen_retire_ticket = 0;
}
// The surface goes away with its window (window destroyed: detach / surface lost / new window: attach / the retire list
// is full and a rebuild cannot wait): every swapchain of it must be destroyed before vkDestroySurfaceKHR
// (VUID-vkDestroySurfaceKHR-surface-01266), and no newer swapchain will present on this surface, so the evidence above
// cannot be had. The evidence here is the Android platform itself (this file is built for Android only):
//  - on Android VK_KHR_swapchain is implemented by the platform loader (frameworks/native/vulkan/libvulkan/swapchain.cpp);
//    vkQueuePresentKHR consumes the present wait semaphores inside the call through the driver's
//    vkQueueSignalReleaseImageANDROID (loader comment: "QueueSignalReleaseImageANDROID consumes the wait semaphores"),
//    and Mesa's (Turnip's) common implementation turns that into a submission on the same queue (commit "vulkan/android:
//    improve vkQueueSignalReleaseImageANDROID"). So when vkQueueWaitIdle returns, those semaphore waits have executed.
//  - the swapchain images are gralloc buffers that BufferQueue / the compositor hold their own references to;
//    vkDestroySwapchainKHR only cancels the buffers still held by the application.
//  - the present fence of VK_EXT_swapchain_maintenance1 is, on Android, a temporary import of that same release fence
//    (same swapchain.cpp), so it gives no more evidence than the queue idle; and for a present to a retired swapchain
//    the loader does not import it at all (it sync_waits itself and returns OUT_OF_DATE), so waiting for it would hang.
//    It is therefore not enabled (initialisation only logs whether it is listed).
// Hence: after idling the queue, destroy every swapchain handle of this surface and their present semaphores (the
// current one and the ones on the list), empty the list, and destroy the surface last.
static void screen_release(void) {
    if (!device) return;
    screen_queue_idle();
    unsigned chains = 0, semaphores = 0;
    if (swapchain) { screen_chain c = screen_take_chain(); free(c.images); free(c.initialized);
        screen_retired current = {c.handle, c.present_wait, c.count}; chains++; semaphores += current.count; screen_retired_destroy(&current); }
    for (unsigned i = 0; i < screen_retire_count; i++) { chains += screen_retire[i].handle != VK_NULL_HANDLE; semaphores += screen_retire[i].count; screen_retired_destroy(&screen_retire[i]); }
    if (screen_retire_count) wsi_retire_freed += screen_retire_count;
    screen_retire_count = 0; screen_retire_ticket = 0; screen_retire_image = UINT32_MAX;
    if (surface) { vkDestroySurfaceKHR(instance, surface, NULL); surface = VK_NULL_HANDLE; }
    LOG("[vulkan-wsi] surface released after queue idle: %u swapchains, %u present semaphores destroyed", chains, semaphores);
}

#ifdef __ANDROID__
#define SCREEN_WINDOW screen_window
#else
#define SCREEN_WINDOW NULL
#endif
// Front end: just back / surface just rebuilt; slow frames in the next seconds are not GPU pressure (hle_oe.c). The
// GPU fixtures do not link the host, so it may be absent.
__attribute__((weak)) void dm_3d_scale_resumed(void);
// Last failed rebuild: retries at most 4 per second, never idling the queue every frame
static uint64_t screen_retry_ns;
// Last successful rebuild: with a driver that keeps reporting out of date, rebuilds stay at least 250 ms apart
static uint64_t screen_rebuilt_ns;
static void screen_surface_lost(const char *where);
// The window's raw size at this moment (ANativeWindow_getWidth/Height go through window->query and may reach the
// producer; the time is counted in wsi_wq_*). Returns 0 when the window cannot give one. It is compared with the raw
// window size recorded at the last build (screen_built_w/h), not with the clamped swapchain size: the clamp uses the
// surface capabilities of the last build, and once the window outgrows that old maximum (the Android loader reports
// max(4096, the currentExtent of that moment)) a clamped comparison would always look "equal" and never rebuild;
// comparing raw sizes rebuilds once and re-reads the capabilities while building.
static int screen_window_size(int *w, int *h) {
#ifdef __ANDROID__
    if (!screen_window) return 0;
    uint64_t t = dm_android_clock_ns(CLOCK_MONOTONIC);
    *w = ANativeWindow_getWidth(screen_window); *h = ANativeWindow_getHeight(screen_window);
    t = dm_android_clock_ns(CLOCK_MONOTONIC) - t; wsi_wq_ns_sum += t; wsi_wq_n++; if (t > wsi_wq_ns_max) wsi_wq_ns_max = t;
    return *w > 0 && *h > 0;
#else
    (void)w; (void)h; return 0;
#endif
}
// The raw window size differs from the last build (or the window gave none then).
static int screen_window_changed(int *known, int *w, int *h) {
    *known = screen_window_size(w, h);
    return *known && (*w != screen_built_w || *h != screen_built_h);
}
// The window size was clamped by the surface capabilities at the last build: an unchanged raw size does not mean the
// extent has converged, the capabilities may catch up later. Re-read them; if the same rule then gives an extent
// different from the swapchain -> 1 (rebuild). Capabilities unavailable (surface lost etc.) -> 0, other paths handle that.
static int screen_clamp_moved(int w, int h) {
    if (!screen_built_clamped || !surface || w <= 0 || h <= 0) return 0;
    VkSurfaceCapabilitiesKHR cap; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &cap) != VK_SUCCESS) return 0;
    VkExtent2D e = {(uint32_t)w, (uint32_t)h};
    if (e.width < cap.minImageExtent.width) e.width = cap.minImageExtent.width;
    if (e.height < cap.minImageExtent.height) e.height = cap.minImageExtent.height;
    if (cap.maxImageExtent.width && e.width > cap.maxImageExtent.width) e.width = cap.maxImageExtent.width;
    if (cap.maxImageExtent.height && e.height > cap.maxImageExtent.height) e.height = cap.maxImageExtent.height;
    return e.width != screen_extent.width || e.height != screen_extent.height;
}
// Rebuild the swapchain on the current surface (same window resized / out of date / extent changed / the last
// attempt failed). The caller owns the swapchain exclusively (as for screen_build_chain) and holds no acquired image.
// The old swapchain is chained through oldSwapchain and goes on the retire list; it is neither destroyed here nor is
// the queue idled: the only submission referencing swapchain images is the present blit, which the top of the frame
// has already waited for (mtl_present), and the presentation engine's use of the old images and semaphores is covered
// by the retirement evidence. dirty holds the reason bits. A window the same size as the swapchain, when the reason is
// neither out of date nor a retry after a failure, is not rebuilt (on return to the foreground the system often sends
// same-size resize callbacks). A real size change is exempt from the 250 ms success-rebuild throttle (which only guards
// against a driver that keeps reporting out of date); when a throttle defers the rebuild the reason bits are kept as they
// are. Returns 1 on success; a lost surface goes to screen_surface_lost; any other recoverable failure leaves
// screen_dirty set for a later retry.
static int screen_recreate(const char *why, int dirty) {
    if (!surface || !device) return 0;
    uint64_t start = dm_android_clock_ns(CLOCK_MONOTONIC);
    int known = 0, ww = 0, wh = 0, moved = screen_window_changed(&known, &ww, &wh);
    int change = !swapchain || (known ? moved || screen_clamp_moved(ww, wh) : (dirty & (SCREEN_DIRTY_RESIZED | SCREEN_DIRTY_EXTENT)) != 0);
    if (!change && !(dirty & (SCREEN_DIRTY_OUT_OF_DATE | SCREEN_DIRTY_RETRY))) {
        if (++wsi_same_size_skips <= 8 || !(wsi_same_size_skips % 64)) LOG("[vulkan-wsi] rebuild skipped reason=%s: window %dx%d is the size the swapchain was built for (skip #%llu)", why, ww, wh, (unsigned long long)wsi_same_size_skips);
        return 0;
    }
    // Retire list backlog: while the current swapchain can still present and the reason is not out of date, do not build
    // yet: keep the reason bits and look again next frame; the current chain keeps presenting (the compositor scales it
    // to the window), its retirement evidence arrives within count+1 successful acquires and the rebuild happens once the
    // list is reclaimed. With a current swapchain, out of date (cannot wait) and a full list: do not chain (that would
    // overrun the list) but take the surface-lost path: screen_release frees every swapchain of this surface on the
    // platform evidence and the next frame rebuilds on the same window. Without a current swapchain nothing is chained
    // (oldSwapchain = NULL), nothing joins the list, and the rebuild goes ahead.
    if (swapchain && screen_retire_count >= SCREEN_RETIRE_DEFER && !(dirty & SCREEN_DIRTY_OUT_OF_DATE)) {
        if (++wsi_retire_deferred <= 8 || !(wsi_retire_deferred % 64)) LOG("[vulkan-wsi] rebuild deferred reason=%s: %u retired swapchains still wait for evidence, presenting to the current %ux%u swapchain meanwhile (deferral #%llu)", why, screen_retire_count, screen_extent.width, screen_extent.height, (unsigned long long)wsi_retire_deferred);
        atomic_fetch_or(&screen_dirty, dirty ? dirty : SCREEN_DIRTY_EXTENT); return 0;
    }
    if (swapchain && screen_retire_count >= SCREEN_RETIRE_MAX) {
        wsi_retire_overflow++;
        LOG("[vulkan-wsi] 【红】retire list full (%u entries) and the rebuild cannot wait (reason=%s, overflow #%llu): releasing the surface instead of chaining", screen_retire_count, why, (unsigned long long)wsi_retire_overflow);
        screen_surface_lost("retire list full"); return 0;
    }
    // Retries after a failure at most 4 per second; with a driver that keeps reporting out of date, two successful
    // rebuilds also stay 250 ms apart, which a real size change (change) is exempt from.
    if ((screen_retry_ns && start - screen_retry_ns < 250000000ull) || (!change && screen_rebuilt_ns && start - screen_rebuilt_ns < 250000000ull)) { if (dirty) atomic_fetch_or(&screen_dirty, dirty); return 0; }
    VkExtent2D before = screen_extent;
    screen_chain fresh; int consumed = 0;
    VkResult result = screen_build_chain(SCREEN_WINDOW, swapchain, &consumed, &fresh);
    // The old swapchain passed in is retired by the spec (success or not): onto the retire list, never presented again.
    if (consumed) { screen_chain old = screen_take_chain(); screen_retire_chain(&old); }
    uint64_t took = dm_android_clock_ns(CLOCK_MONOTONIC) - start;
    if (result == VK_SUCCESS) {
        screen_install_chain(&fresh);
        wsi_recreates++; screen_retry_ns = 0; screen_caps_checked = 0; screen_rebuilt_ns = dm_android_clock_ns(CLOCK_MONOTONIC);
        LOG("[vulkan-wsi] swapchain rebuilt reason=%s %ux%u -> %ux%u in %.3f ms (frame %llu, rebuild #%llu, old swapchain %s, retire list %u)", why, before.width, before.height, screen_extent.width, screen_extent.height, took / 1e6, (unsigned long long)presents, (unsigned long long)wsi_recreates, consumed ? "chained+retired" : "none", screen_retire_count);
        if (dm_3d_scale_resumed) dm_3d_scale_resumed();
        return 1;
    }
    wsi_recreate_failures++; screen_retry_ns = start;
    if (result == VK_ERROR_SURFACE_LOST_KHR) { screen_surface_lost(why); return 0; }
    atomic_fetch_or(&screen_dirty, dirty | SCREEN_DIRTY_RETRY);
    if (wsi_recreate_failures <= 8 || !(wsi_recreate_failures % 64))
        LOG("[vulkan-wsi] 【红】swapchain rebuild failed reason=%s result=%d (failure #%llu, old swapchain %s); %s, retrying", why, result, (unsigned long long)wsi_recreate_failures,
            consumed ? "retired" : "kept", swapchain ? "still presenting to the old swapchain" : "no presenting until it succeeds");
    return 0;
}
// Surface lost (the window was abandoned by the system): release the swapchain and the surface together and stop
// presenting; afterwards either the front end hands over a new window (attach) or it is rebuilt from the same window
// (screen_surface_retry).
static void screen_surface_lost(const char *where) {
    LOG("[vulkan-wsi] 【红】surface lost at %s (frame %llu): surface and swapchain released, no presenting until a valid surface exists", where, (unsigned long long)presents);
    screen_release();
    screen_lost_retry_ns = 0;
}
#ifdef __ANDROID__
// Game thread, top of a frame, no surface while the front end still holds a window: at most once a second try to
// rebuild the surface and swapchain on that same window. A new window goes through attach.
static void screen_surface_retry(void) {
    if (surface || !screen_window || !instance || !device) return;
    uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC);
    if (screen_lost_retry_ns && now - screen_lost_retry_ns < 1000000000ull) return;
    screen_lost_retry_ns = now;
    // The new surface and swapchain are built at the window size of this moment: marks and throttles gathered before /
    // during the loss are void (a resize arriving after this marks again), so the fresh swapchain is neither torn down
    // right away nor deferred.
    atomic_store(&screen_dirty, 0); screen_retry_ns = 0; screen_rebuilt_ns = 0; screen_caps_checked = 0;
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = screen_window};
    VkResult result = vkCreateAndroidSurfaceKHR(instance, &si, NULL, &surface);
    if (result != VK_SUCCESS) { surface = VK_NULL_HANDLE; LOG("[vulkan-wsi] 【红】surface rebuild on the same window failed result=%d; waiting for a new window", result); return; }
    screen_chain fresh; result = screen_build_chain(screen_window, VK_NULL_HANDLE, NULL, &fresh);
    if (result != VK_SUCCESS) { vkDestroySurfaceKHR(instance, surface, NULL); surface = VK_NULL_HANDLE; LOG("[vulkan-wsi] 【红】swapchain on the rebuilt surface failed result=%d; waiting for a new window", result); return; }
    screen_install_chain(&fresh);
    wsi_surface_rebuilds++;
    LOG("[vulkan-wsi] surface rebuilt on the same window %ux%u (rebuild #%llu)", screen_extent.width, screen_extent.height, (unsigned long long)wsi_surface_rebuilds);
    if (dm_3d_scale_resumed) dm_3d_scale_resumed();
}
#endif
// SUBOPTIMAL: the image is presented as usual (an acquired image must be submitted and presented), then check whether
// the surface extent changed. Android reports SUBOPTIMAL on every frame while currentTransform is 90/270 and the
// swapchain uses IDENTITY (we deliberately do not pre-rotate), so an unchanged extent keeps presenting without a
// rebuild; the extent is queried at most once every 60 frames.
static void screen_note_suboptimal(const char *where) {
    if (screen_caps_checked && presents - screen_caps_checked < 60) return;
    screen_caps_checked = presents;
    VkSurfaceCapabilitiesKHR cap; VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &cap);
    if (result != VK_SUCCESS) { if (!wsi_recoverable(result)) die("Vulkan surface capabilities after SUBOPTIMAL: %d", result); atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_RETRY); return; }
    // The build rule: the raw window size first (against the last build), currentExtent only when the window cannot give one.
    VkExtent2D extent = cap.currentExtent; int known = 0, ww = 0, wh = 0, moved = screen_window_changed(&known, &ww, &wh);
    if (known) extent = (VkExtent2D){(uint32_t)ww, (uint32_t)wh};
    else if (extent.width == UINT32_MAX) return;
    if (known ? moved || screen_clamp_moved(ww, wh) : extent.width != screen_extent.width || extent.height != screen_extent.height) {
        LOG("[vulkan-wsi] SUBOPTIMAL at %s with extent %ux%u -> %ux%u: rebuilding at the next frame", where, screen_extent.width, screen_extent.height, extent.width, extent.height);
        atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_EXTENT);
        return;
    }
    static int reported;
    if (!reported++) LOG("[vulkan-wsi] SUBOPTIMAL at %s with unchanged extent %ux%u (currentTransform=%#x preTransform=%#x, compositor rotates): presenting as is, no rebuild", where, extent.width, extent.height, cap.currentTransform, screen_transform);
}
static void wsi_count(uint64_t *counts, VkResult result) {
    counts[result == VK_SUCCESS ? WSI_OK : result == VK_SUBOPTIMAL_KHR ? WSI_SUBOPTIMAL : result == VK_ERROR_OUT_OF_DATE_KHR ? WSI_OUT_OF_DATE :
           result == VK_TIMEOUT || result == VK_NOT_READY ? WSI_TIMEOUT : WSI_SURFACE_LOST]++;
}

#ifdef __ANDROID__
// The standalone GPU fixtures (build_gpu_fixtures.py) link only this file and not the host graphics layer, so
// dm_gfx_suspend is allowed to be absent: without it we simply do not suspend, and the fixtures have no concurrent
// presenting to race against anyway.
__attribute__((weak)) void dm_gfx_suspend(int on);
/* Presents come from the translation thread, and from the overlay thread while the game is paused under the menu. The
   overlay thread is outside dm_gfx_suspend's gate, so the window callbacks also take this lock to change the swapchain. */
static pthread_mutex_t present_serial = PTHREAD_MUTEX_INITIALIZER;
// Whether the backend has finished building its surface and swapchain (initialisation done). A call during
// initialisation waits for it to finish (screen_lock).
static int screen_backend_live(void) {
    pthread_mutex_lock(&screen_lock); int live = screen_live && instance && device; pthread_mutex_unlock(&screen_lock);
    return live;
}
// Called by the front end when the window goes away or appears. A destroyed window does not end the process: Android
// always destroys it when the activity stops, and keeping the process is what lets play resume on return. A real exit
// goes through the destroyed() path.
void mtl_screen_detach(void) {
    // mtl_init is deliberately not called here: it would initialise the whole Vulkan backend on the callback
    // thread (the UI thread). Only the game thread initialises the backend; when it has not started, return — it
    // waits for a window itself (dm_android_window_wait). When it is in progress, wait for it to finish, then tear down.
    if (!screen_backend_live()) return;
    // Suspend here as well: the order of window_destroyed and paused is not guaranteed, suspending is idempotent,
    // and doing it first is what keeps this from racing the presenting thread.
    if (dm_gfx_suspend) dm_gfx_suspend(1);
    pthread_mutex_lock(&present_serial); pthread_mutex_lock(&screen_lock);
    screen_release(); screen_bind_window(NULL); atomic_store(&screen_dirty, 0);
    pthread_mutex_unlock(&screen_lock); pthread_mutex_unlock(&present_serial);
    LOG("[android] 窗口销毁：表面与交换链已释放，进程保留");
}
void mtl_screen_attach(struct ANativeWindow *window) {
    // Same as above: never initialise the backend on the callback thread; when it is not live yet, initialisation
    // builds the surface and swapchain itself from the front end's current window.
    if (!window || !screen_backend_live()) return;
    // Suspend before touching the swapchain, for the same reason as in detach: Android hands the window over
    // *after* onResume (measured on device: the graphics resume, then the window rebuild), so by the time this runs the
    // game thread is already free and may be inside a present reading screen_images / swapchain. Suspending blocks new entries
    // and waits out the one in flight (gfx_enter/gfx_leave), which is what makes this point safe. Releasing it is
    // left to the front end, which alone knows whether the app is in the foreground: this callback can arrive
    // either side of onResume.
    if (dm_gfx_suspend) dm_gfx_suspend(1);
    pthread_mutex_lock(&present_serial); pthread_mutex_lock(&screen_lock);
    screen_release();
    // New window: throttles and the extent check start afresh
    screen_bind_window(window); atomic_store(&screen_dirty, 0); screen_retry_ns = 0; screen_rebuilt_ns = 0; screen_caps_checked = 0;
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window};
    VkResult result = vkCreateAndroidSurfaceKHR(instance, &si, NULL, &surface);
    if (result != VK_SUCCESS) {
        surface = VK_NULL_HANDLE;
        if (result != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR && !wsi_recoverable(result)) die("Vulkan vkCreateAndroidSurfaceKHR for the new window failed: %d", result);
        LOG("[vulkan-wsi] 【红】new window: surface creation failed result=%d; the game thread retries on this window", result);
    } else {
        screen_chain fresh;
        if ((result = screen_build_chain(window, VK_NULL_HANDLE, NULL, &fresh)) == VK_SUCCESS) screen_install_chain(&fresh);
        else {
            // Surface present, swapchain not yet buildable (zero-sized window etc.): the game thread retries at the top of
            // a frame.
            atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_RETRY);
            LOG("[vulkan-wsi] 【红】new window: swapchain creation deferred result=%d; the game thread retries", result);
        }
    }
    int ready = surface && swapchain;
    pthread_mutex_unlock(&screen_lock); pthread_mutex_unlock(&present_serial);
    // Say "ready" only when both the surface and the swapchain exist; otherwise log "pending" as it is, real recovery is
    // proven by a later successful present.
    if (ready) LOG("[android] 窗口重建：新的表面与交换链已就绪");
    else LOG("[android] 窗口重建：新窗口已绑定，表面 %s、交换链 %s，由游戏线程在帧开头重试", surface ? "已建" : "未建", swapchain ? "已建" : "未建");
}
#endif

static void initialize(void) {
    /* TOTK_VK_BINDLESS: its shader cache, before anything reads DM_VK_SHADERS */
    bindless_on();
    // Match the original NVN defaults; explicit commands override them without rewriting legal depth ranges.
    state.mask.mask=UINT32_MAX;state.depth_range=(p_depth_range){0,1};
    state.color=(color_st){0,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS};
    state.polygon=(poly_st){NVN_FACE_NONE,NVN_FRONT_FACE_CCW,NVN_POLYGON_MODE_FILL,0};
    state.depth=(ds_st){0,1,NVN_DEPTH_FUNC_LESS,0,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_FUNC_ALWAYS,1,1,1,1,1,1};
    for(unsigned i=0;i<8;i++)state.blend[i]=(blend_st){i,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};
    for(unsigned i=0;i<2;i++)state.stencil_faces[i].compareMask=state.stencil_faces[i].writeMask=255;
    LOG("[vulkan-cpu-phases] record_phases=%s (DM_VK_CPU_PHASES=1 / cpu_phases=1 turns per-draw phase timing on; present-path phases are always timed)",dm_cpu_record_phases_enabled()?"on":"off");
    // Building the surface / swapchain excludes the window callbacks (screen_lock, see its note): an attach / detach
    // arriving during initialisation waits until this is done. When the window is already gone before initialisation
    // (backgrounded during start-up), wait without the lock for the front end's next window; otherwise a windowless
    // device without the swapchain extension would be created and nothing could be presented on return.
    pthread_mutex_lock(&screen_lock);
#ifdef __ANDROID__
    // The window is taken together with a reference (dm_android_window_acquire); after initialisation it is handed to
    // screen_window (no second reference), and a fallback to the system driver reuses the same window.
    ANativeWindow *window = NULL;
    {   extern void *dm_android_window_wait(void) __attribute__((weak)); extern void *dm_android_window_acquire(void) __attribute__((weak));
        void *front_metal_layer(void);
        for (;;) {
            if (dm_android_window_acquire) window = dm_android_window_acquire();
            else if ((window = front_metal_layer())) ANativeWindow_acquire(window);
            if (window || !dm_android_window_wait) break;
            pthread_mutex_unlock(&screen_lock); LOG("[vulkan-wsi] no window at backend initialisation: waiting for the front end's next window");
            void *ready = dm_android_window_wait(); pthread_mutex_lock(&screen_lock); if (!ready) break; }
    }
    if(!dm_vk_driver_open(&vk_api))die("Vulkan loader: %s",dm_vk_driver_error(&vk_api));
driver_retry:;
#endif
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "D2R NVN Vulkan", .apiVersion = VK_API_VERSION_1_1};
    /* LSFG frame generation: synchronization2, SPIR-V 1.6 */
    if (lsfg_device_wanted()) app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
#ifdef __ANDROID__
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    /* TOTK_VK_DEBUG=1 (with the validation layer, tools/gpu_debug.sh): validation messages go to the log */
    int debug_utils = getenv("TOTK_VK_DEBUG") && atoi(getenv("TOTK_VK_DEBUG"));
    if (window) { info.enabledExtensionCount = 2 + (debug_utils ? 1 : 0); info.ppEnabledExtensionNames = extensions; }
    else if (debug_utils) { info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = extensions + 2; }
    /* TOTK_RENDERDOC=1 (APK built with TOTK_RENDERDOC=1): enable the capture layer explicitly; the debug-layer setting alone
       does not reach the instance on Qualcomm's driver */
    static const char *rdoc_layer[] = {"VK_LAYER_RENDERDOC_Capture"};
    if (getenv("TOTK_RENDERDOC") && atoi(getenv("TOTK_RENDERDOC"))) { info.enabledLayerCount = 1; info.ppEnabledLayerNames = rdoc_layer; }
#endif
    /* TOTK_VK_SYNCVAL=1 (with the validation layer): synchronization validation as well, asked for at instance creation
       through the layer's own extensions (its settings file and properties do not reach it on Android) */
    static const VkValidationFeatureEnableEXT syncval[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT validation = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = syncval};
    static const VkBool32 yes = VK_TRUE;
    VkLayerSettingEXT setting = {"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &yes};
    VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, .pNext = &validation, .settingCount = 1, .pSettings = &setting};
    const char *with_validation[8]; uint32_t nwith = 0;
    if (getenv("TOTK_VK_SYNCVAL") && atoi(getenv("TOTK_VK_SYNCVAL"))) {
        for (uint32_t i = 0; i < info.enabledExtensionCount; i++) with_validation[nwith++] = info.ppEnabledExtensionNames[i];
        VkExtensionProperties layer_ext[32]; uint32_t n = 32;
        PFN_vkEnumerateInstanceExtensionProperties enumerate = (PFN_vkEnumerateInstanceExtensionProperties)vk_api.vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
        if (enumerate && enumerate("VK_LAYER_KHRONOS_validation", &n, layer_ext) >= 0)
            for (uint32_t i = 0; i < n && nwith < 8; i++) {
                LOG("[vulkan] validation layer instance extension %s", layer_ext[i].extensionName);
                if (!strcmp(layer_ext[i].extensionName, "VK_EXT_layer_settings") || !strcmp(layer_ext[i].extensionName, "VK_EXT_validation_features")) with_validation[nwith++] = layer_ext[i].extensionName;
            }
        info.enabledExtensionCount = nwith; info.ppEnabledExtensionNames = with_validation; info.pNext = &settings;
        LOG("[vulkan] synchronization validation requested (%u instance extensions)", nwith);
    }
    VkResult create_result=vkCreateInstance(&info,NULL,&instance);
#ifdef __ANDROID__
    if(create_result!=VK_SUCCESS&&initialize_driver_fallback("vkCreateInstance failed"))goto driver_retry;
#endif
    VK_CHECK(create_result);
#ifdef __ANDROID__
    if(!dm_vk_driver_load_instance(&vk_api,instance,window!=NULL)){if(initialize_driver_fallback(dm_vk_driver_error(&vk_api)))goto driver_retry;die("Vulkan instance dispatch: %s",dm_vk_driver_error(&vk_api));}
    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vk_api.vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = vk_debug_message};
        static VkDebugUtilsMessengerEXT messenger;
        if (!create || create(instance, &mi, NULL, &messenger) != VK_SUCCESS) LOG("[vulkan-debug] no debug messenger (is the validation layer on?)");
        else LOG("[vulkan-debug] validation messages are logged");
    }
    if (window) { VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window};
        create_result=vkCreateAndroidSurfaceKHR(instance,&si,NULL,&surface);
        if(create_result!=VK_SUCCESS&&initialize_driver_fallback("vkCreateAndroidSurfaceKHR failed"))goto driver_retry;VK_CHECK(create_result);
    }
#endif
    uint32_t n = 0; VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, NULL));
    if (!n) {
#ifdef __ANDROID__
        if(initialize_driver_fallback("custom Vulkan has no physical device"))goto driver_retry;
#endif
        die("Vulkan has no physical device");
    }
    VkPhysicalDevice *devices = calloc(n, sizeof *devices); if (!devices) die("Vulkan device list allocation");
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devices));
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < n && family == UINT32_MAX; ++i) {
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q); if (!q) die("Vulkan queue list allocation");
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &nq, q);
        for (uint32_t j = 0; j < nq; ++j) {
            LOG("[vulkan] queue family %u: flags %#x, %u queues", j, q[j].queueFlags, q[j].queueCount);
            if ((q[j].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { VkBool32 supported = VK_TRUE; if (surface) VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, surface, &supported)); if (supported) { physical = devices[i]; family = j; gpu_timestamp_bits=q[j].timestampValidBits; break; } }
        }
        free(q);
    }
    free(devices); if (!physical) {
#ifdef __ANDROID__
        if(initialize_driver_fallback("custom Vulkan has no graphics/compute/present queue"))goto driver_retry;
#endif
        die("Vulkan requires a graphics and compute queue");
    }
#ifdef __ANDROID__
    const char *shader_profile_directory=getenv("DM_VK_SHADERS");
    if(vk_api.using_custom&&!shader_profile_directory)die("Custom game driver requires DM_VK_SHADERS and its subgroup profile");
    if(!dm_vk_driver_validate(&vk_api,physical,shader_profile_directory)){
        if(initialize_driver_fallback(dm_vk_driver_error(&vk_api)))goto driver_retry;
        die("Vulkan driver validation: %s",dm_vk_driver_error(&vk_api));
    }
#endif
    VkPhysicalDeviceProperties properties; VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&subgroup};
    vkGetPhysicalDeviceProperties2(physical,&properties2);properties=properties2.properties;gpu_limits=properties.limits;gpu_subgroup_size=subgroup.subgroupSize;
    gpu_vs_subgroups=(subgroup.supportedStages&VK_SHADER_STAGE_VERTEX_BIT)&&(subgroup.supportedOperations&VK_SUBGROUP_FEATURE_BALLOT_BIT);
#ifdef __ANDROID__
    // Check caches against the explicitly selected and supported shader size; preserve actual physical defaults separately.
    gpu_subgroup_size=vk_api.effective_shader_subgroup_size;
#endif
    vkGetPhysicalDeviceFeatures(physical, &features); gpu_features = features;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    for(uint32_t i=0;i<memory_properties.memoryTypeCount;i++)LOG("[vulkan] memory type %u flags=%#x heap=%u",i,memory_properties.memoryTypes[i].propertyFlags,memory_properties.memoryTypes[i].heapIndex);
    LOG("Android NVN Vulkan: %s API %u.%u BC=%u ASTC=%u", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), features.textureCompressionBC, features.textureCompressionASTC_LDR);
    LOG("[vulkan] subgroup size=%u stages=%#x operations=%#x quad_all=%u",subgroup.subgroupSize,subgroup.supportedStages,subgroup.supportedOperations,subgroup.quadOperationsInAllStages);
    // VK_EXT_swapchain_maintenance1 is logged only, not enabled: the Android loader's present fence is a temporary import
    // of the same release fence and is not imported at all for a present to a retired swapchain (see the platform
    // evidence at screen_release), so it gives no more evidence than the queue idle.
    if (surface) { uint32_t ne = 0; int maintenance1 = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, NULL, &ne, NULL) == VK_SUCCESS && ne) { VkExtensionProperties *ext = calloc(ne, sizeof *ext);
            if (ext && vkEnumerateDeviceExtensionProperties(physical, NULL, &ne, ext) == VK_SUCCESS) for (uint32_t i = 0; i < ne; i++) { maintenance1 |= !strcmp(ext[i].extensionName, "VK_EXT_swapchain_maintenance1");
                if (!strcmp(ext[i].extensionName, "VK_EXT_external_memory_host")) LOG("[vulkan] VK_EXT_external_memory_host available (zero-copy guest memory possible)");
                pace_ext |= !strcmp(ext[i].extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
                provoking_last |= !strcmp(ext[i].extensionName, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
                ext_eds |= !strcmp(ext[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME); ext_eds2 |= !strcmp(ext[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
                ext_memory_fd |= !strcmp(ext[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME); ext_robustness2 |= !strcmp(ext[i].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME); ext_fbloop |= !strcmp(ext[i].extensionName, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME); ext_rorder |= !strcmp(ext[i].extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME); ext_custom_border |= !strcmp(ext[i].extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME); ext_dma_buf |= !strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME); ext_descriptor_indexing |= !strcmp(ext[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
                ext_gpl |= !strcmp(ext[i].extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME); ext_pipeline_library |= !strcmp(ext[i].extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME); }
            free(ext); }
        LOG("[vulkan-wsi] device extensions=%u VK_EXT_swapchain_maintenance1=%s (not enabled; same-surface rebuilds retire on re-acquire evidence, surface teardown on queue idle)", ne, maintenance1 ? "listed" : "absent"); }
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    /* largePoints: point sizes above 1 (the grass-removal stamps) */
    VkPhysicalDeviceFeatures enable = {.textureCompressionBC = features.textureCompressionBC, .textureCompressionASTC_LDR = features.textureCompressionASTC_LDR, .independentBlend = features.independentBlend, .shaderImageGatherExtended = features.shaderImageGatherExtended, .fragmentStoresAndAtomics = features.fragmentStoresAndAtomics, .vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics, .shaderStorageImageReadWithoutFormat = features.shaderStorageImageReadWithoutFormat, .shaderStorageImageWriteWithoutFormat = features.shaderStorageImageWriteWithoutFormat, .occlusionQueryPrecise = features.occlusionQueryPrecise, .largePoints = features.largePoints,
        /* Ryujinx-translated shaders and NVN state use these where the device has them */
        .depthClamp = features.depthClamp, .depthBiasClamp = features.depthBiasClamp, .samplerAnisotropy = features.samplerAnisotropy,
        .shaderClipDistance = features.shaderClipDistance, .shaderCullDistance = features.shaderCullDistance, .imageCubeArray = features.imageCubeArray,
        .shaderInt16 = features.shaderInt16, .shaderInt64 = features.shaderInt64, .sampleRateShading = features.sampleRateShading,
        .fillModeNonSolid = features.fillModeNonSolid, .multiViewport = features.multiViewport, .drawIndirectFirstInstance = features.drawIndirectFirstInstance,
        .dualSrcBlend = features.dualSrcBlend, .logicOp = features.logicOp, .depthBounds = features.depthBounds,
        .shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats, .geometryShader = features.geometryShader};
    /* FSR frame interpolation: formatless and extended storage images, and subgroup arithmetic and quads in compute */
    {
        const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
        fsr_device_ok = features.shaderStorageImageExtendedFormats && features.shaderStorageImageReadWithoutFormat && features.shaderStorageImageWriteWithoutFormat &&
            (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (subgroup.supportedOperations & need) == need && subgroup.subgroupSize >= 32;
        static const struct { VkFormat f; VkFormatFeatureFlags need; } formats[] = {{VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT}, {VK_FORMAT_R8G8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
            {VK_FORMAT_R16G16_SINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT}, {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT}, {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
            {VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT}, {VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT}};
        for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(physical, formats[i].f, &fp);
            if ((fp.optimalTilingFeatures & formats[i].need) != formats[i].need) fsr_device_ok = 0; }
        if (getenv("TOTK_FSRFG_NONE")) fsr_device_ok = 0;
        LOG("[fsrfg] %s on this device (subgroup size %u)", fsr_device_ok ? "available" : "not available", subgroup.subgroupSize); }
    if (getenv("TOTK_VK_DRAWSTATS")) enable.pipelineStatisticsQuery = features.pipelineStatisticsQuery;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .pEnabledFeatures = &enable};
    /* Ryujinx's SPIR-V declares DrawParameters (base vertex / instance) */
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .shaderDrawParameters = VK_TRUE};
    di.pNext = &draw_parameters;
    const char *device_extensions[20]; unsigned nde = 0;
    if (surface) device_extensions[nde++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* frame-generation pacing (nvn_vulkan_pacing.inc) */
    if (surface && pace_ext) device_extensions[nde++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    /* experiment: Vulkan's default */
    if (exp_env("firstvertex")) provoking_last = 0;
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, .provokingVertexLast = VK_TRUE};
    if (provoking_last) { device_extensions[nde++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME; provoking.pNext = (void *)di.pNext; di.pNext = &provoking; }
    LOG("[vulkan] provoking vertex: %s", provoking_last ? "last (VK_EXT_provoking_vertex)" : "first (extension absent: flat varyings may come from the wrong vertex)");
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    {   const char *e = getenv("TOTK_VK_EDS"); if (e && !atoi(e)) ext_eds = ext_eds2 = 0;
        if (ext_eds) { eds.pNext = ext_eds2 ? &eds2 : NULL; VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &eds};
                       vkGetPhysicalDeviceFeatures2(physical, &f2); ext_eds = eds.extendedDynamicState; ext_eds2 = ext_eds && ext_eds2 && eds2.extendedDynamicState2; }
        eds.pNext = NULL; eds2.pNext = NULL; eds.extendedDynamicState = VK_TRUE; eds2.extendedDynamicState2 = VK_TRUE;
        eds2.extendedDynamicState2LogicOp = eds2.extendedDynamicState2PatchControlPoints = VK_FALSE;
        if (ext_eds) { device_extensions[nde++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME; eds.pNext = (void *)di.pNext; di.pNext = &eds; }
        if (ext_eds2) { device_extensions[nde++] = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME; eds2.pNext = (void *)di.pNext; di.pNext = &eds2; } }
    /* dma-buf export: zero-copy pools and their probe */
    {   const char *e = getenv("TOTK_VK_MEMPROBE"), *z = getenv("TOTK_VK_ZEROCOPY");
        if (!((e && atoi(e)) || (z && atoi(z))) || !ext_memory_fd || !ext_dma_buf) ext_memory_fd = ext_dma_buf = 0;
        else { device_extensions[nde++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME; device_extensions[nde++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME; } }
    /* LSFG frame generation runs on this device (nvn_vulkan_lsfg.inc) and needs these (individual structs: the subgroup
       policy adds VkPhysicalDeviceSubgroupSizeControlFeatures, which excludes VkPhysicalDeviceVulkan13Features) */
    VkPhysicalDeviceRobustness2FeaturesEXT lsfg_robustness2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceSynchronization2Features lsfg_sync2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .pNext = &lsfg_robustness2};
    VkPhysicalDeviceTimelineSemaphoreFeatures lsfg_timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &lsfg_sync2};
    VkPhysicalDeviceVulkanMemoryModelFeatures lsfg_memory_model = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, .pNext = &lsfg_timeline};
    lsfg_device_ok = 0;
    {   int l = lsfg_device_wanted();
        if (l && ext_robustness2 && properties.apiVersion >= VK_API_VERSION_1_3) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &lsfg_memory_model};
            vkGetPhysicalDeviceFeatures2(physical, &query);
            if (lsfg_robustness2.nullDescriptor && lsfg_sync2.synchronization2 && lsfg_timeline.timelineSemaphore && lsfg_memory_model.vulkanMemoryModel) {
                lsfg_robustness2 = (VkPhysicalDeviceRobustness2FeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = (void *)di.pNext, .nullDescriptor = VK_TRUE};
                lsfg_sync2.synchronization2 = lsfg_timeline.timelineSemaphore = VK_TRUE;
                lsfg_memory_model = (VkPhysicalDeviceVulkanMemoryModelFeatures){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, .pNext = &lsfg_timeline, .vulkanMemoryModel = VK_TRUE};
                di.pNext = &lsfg_memory_model; device_extensions[nde++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME; lsfg_device_ok = 1;
            } else LOG("[lsfg] the device lacks a feature LSFG needs (nullDescriptor %u synchronization2 %u timelineSemaphore %u vulkanMemoryModel %u)",
                lsfg_robustness2.nullDescriptor, lsfg_sync2.synchronization2, lsfg_timeline.timelineSemaphore, lsfg_memory_model.vulkanMemoryModel);
        } else if (l) LOG("[lsfg] the device lacks VK_EXT_robustness2 or Vulkan 1.3"); }
    /* robustBufferAccess2 (TOTK_VK_ROBUST2=0 turns it off): uniform blocks whose shader reach is unknown bind the submit
       capture at their exact range instead of the arena path; Turnip's UBO descriptor carries the range for the hardware */
    VkPhysicalDeviceRobustness2FeaturesEXT ubo_robust = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    {   const char *e = getenv("TOTK_VK_ROBUST2");
        if (ext_robustness2 && !(e && !atoi(e))) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ubo_robust}; vkGetPhysicalDeviceFeatures2(physical, &query);
            if (ubo_robust.robustBufferAccess2) {
                /* the robustness2 struct is already in the chain */
                if (lsfg_device_ok) lsfg_robustness2.robustBufferAccess2 = VK_TRUE;
                else { ubo_robust = (VkPhysicalDeviceRobustness2FeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = (void *)di.pNext, .robustBufferAccess2 = VK_TRUE};
                       di.pNext = &ubo_robust; device_extensions[nde++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME; }
                ubo_robust_ok = 1; } }
        LOG("[vulkan] robustBufferAccess2: %s", ubo_robust_ok ? "on (exact-range uniform binds)" : "off"); }
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbloop = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    {   if (ext_fbloop) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fbloop}; vkGetPhysicalDeviceFeatures2(physical, &query);
            if (fbloop.attachmentFeedbackLoopLayout) { fbloop.pNext = (void *)di.pNext; di.pNext = &fbloop; device_extensions[nde++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME; fbloop_ok = 1; } }
        LOG("[vulkan] attachment feedback loops: %s", fbloop_ok ? "in-pass (VK_EXT_attachment_feedback_loop_layout)" : ext_fbloop ? "pass breaks (feature missing)" : "pass breaks (extension absent)"); }
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rorder = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    {   const char *e = getenv("TOTK_VK_RORDER");
        if (ext_rorder && fbloop_ok && e && atoi(e) > 0) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rorder}; vkGetPhysicalDeviceFeatures2(physical, &query);
            if (rorder.rasterizationOrderColorAttachmentAccess) { rorder.rasterizationOrderDepthAttachmentAccess = rorder.rasterizationOrderStencilAttachmentAccess = VK_FALSE;
                rorder.pNext = (void *)di.pNext; di.pNext = &rorder; device_extensions[nde++] = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME; rorder_ok = 1; } }
        LOG("[vulkan] feedback-loop draws in rasterization order: %s", rorder_ok ? "yes" : ext_rorder ? "no (TOTK_VK_RORDER not set, feedback loops off or feature missing)" : "no (extension absent)"); }
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    {   const char *e = getenv("TOTK_VK_CUSTOM_BORDER");
        if (ext_custom_border && !(e && !atoi(e))) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &border_features}; vkGetPhysicalDeviceFeatures2(physical, &query);
            VkPhysicalDeviceCustomBorderColorPropertiesEXT border_limits = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 limits = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &border_limits}; vkGetPhysicalDeviceProperties2(physical, &limits);
            if (border_features.customBorderColors && border_features.customBorderColorWithoutFormat && border_limits.maxCustomBorderColorSamplers) {
                border_features.pNext = (void *)di.pNext; di.pNext = &border_features; device_extensions[nde++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
                custom_border_max = border_limits.maxCustomBorderColorSamplers; custom_border_ok = 1; } }
        LOG("[vulkan] sampler border colours: %s (up to %u samplers)", custom_border_ok ? "the game's own" : ext_custom_border ? "three fixed (TOTK_VK_CUSTOM_BORDER=0 or a feature missing)" : "three fixed (extension absent)", custom_border_max); }
    /* Pipeline libraries (nvn_vulkan_gpl.inc, TOTK_VK_GPL=0 off): new pipelines fast-linked from per-stage libraries, the
       whole pipeline built behind. Only where the driver links fast (graphicsPipelineLibraryFastLinking): elsewhere a link
       costs about a whole build and gains nothing. */
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    {   const char *e = getenv("TOTK_VK_GPL");
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (ext_gpl && ext_pipeline_library) { VkPhysicalDeviceProperties2 pq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gpl_props}; vkGetPhysicalDeviceProperties2(physical, &pq); }
        if (ext_gpl && ext_pipeline_library && gpl_props.graphicsPipelineLibraryFastLinking && !(e && !atoi(e))) {
            VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &gpl_features}; vkGetPhysicalDeviceFeatures2(physical, &query);
            if (gpl_features.graphicsPipelineLibrary) { gpl_features = (VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = (void *)di.pNext, .graphicsPipelineLibrary = VK_TRUE};
                di.pNext = &gpl_features; device_extensions[nde++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; device_extensions[nde++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; gpl_state = 1; } }
        LOG("[vulkan] graphics pipeline libraries: %s", gpl_state ? "on (new pipelines fast-linked, the whole one built behind)" : ext_gpl ? !ext_pipeline_library ? "off (VK_KHR_pipeline_library absent)" : !gpl_props.graphicsPipelineLibraryFastLinking ? "off (the driver does not link fast)" : "off (TOTK_VK_GPL=0)" : "off (extension absent)"); }
    /* TOTK_VK_BINDLESS: unsized, partially bound arrays updated while bound (nvn_vulkan_bindless.inc) */
    VkPhysicalDeviceDescriptorIndexingFeatures indexing = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    if (bindless_on()) {
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &indexing}; vkGetPhysicalDeviceFeatures2(physical, &query);
        if (!ext_descriptor_indexing || !indexing.runtimeDescriptorArray || !indexing.descriptorBindingPartiallyBound || !indexing.descriptorBindingSampledImageUpdateAfterBind ||
            !indexing.descriptorBindingUpdateUnusedWhilePending) die("TOTK_VK_BINDLESS needs VK_EXT_descriptor_indexing with unsized, partially bound, update-after-bind arrays");
        indexing = (VkPhysicalDeviceDescriptorIndexingFeatures){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, .pNext = (void *)di.pNext,
            .runtimeDescriptorArray = VK_TRUE, .descriptorBindingPartiallyBound = VK_TRUE, .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
            .descriptorBindingUpdateUnusedWhilePending = VK_TRUE};
        di.pNext = &indexing; device_extensions[nde++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    }
    if (nde) { di.enabledExtensionCount = nde; di.ppEnabledExtensionNames = device_extensions; }
#ifdef __ANDROID__
    if(!dm_vk_driver_prepare_device(&vk_api,&di))die("Vulkan subgroup device policy: %s",dm_vk_driver_error(&vk_api));
#endif
    create_result=vkCreateDevice(physical,&di,NULL,&device);
    if (create_result == VK_SUCCESS && pace_ext) { pace_get_refresh = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(device, "vkGetRefreshCycleDurationGOOGLE");
        pace_get_past = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"); }
#ifdef __ANDROID__
    if(create_result!=VK_SUCCESS&&initialize_driver_fallback("vkCreateDevice failed"))goto driver_retry;
#endif
    VK_CHECK(create_result);
#ifdef __ANDROID__
    if(!dm_vk_driver_load_device(&vk_api,device)){if(initialize_driver_fallback(dm_vk_driver_error(&vk_api)))goto driver_retry;die("Vulkan device dispatch: %s",dm_vk_driver_error(&vk_api));}
    // No driver_retry after this point: the driver identity is verified and final. Select the offline driver variant
    // by the actual driverID (nvn_vulkan_shader_variant.inc), before the pipeline cache (whose identity includes the
    // variant) and before any shader is loaded.
    shader_variant_init(vk_api.actual_driver_id);
    /* TOTK_VK_BINDLESS: the global texture arrays, before any pipeline layout */
    bindless_create();
#endif
    vkGetDeviceQueue(device,family,0,&queue);queue_family=family;
    if (ext_eds) {
#define EDS_FN(v, name) v = (void *)vkGetDeviceProcAddr(device, name)
        EDS_FN(eds_depth_test, "vkCmdSetDepthTestEnableEXT"); EDS_FN(eds_depth_write, "vkCmdSetDepthWriteEnableEXT"); EDS_FN(eds_depth_compare, "vkCmdSetDepthCompareOpEXT");
        EDS_FN(eds_stencil_test, "vkCmdSetStencilTestEnableEXT"); EDS_FN(eds_stencil_op, "vkCmdSetStencilOpEXT"); EDS_FN(eds_cull, "vkCmdSetCullModeEXT"); EDS_FN(eds_front, "vkCmdSetFrontFaceEXT");
        if (ext_eds2) EDS_FN(eds_bias_enable, "vkCmdSetDepthBiasEnableEXT");
#undef EDS_FN
        eds_on = eds_depth_test && eds_depth_write && eds_depth_compare && eds_stencil_test && eds_stencil_op && eds_cull && eds_front;
        eds2_on = eds_on && eds_bias_enable;
    }
    LOG("[vulkan] dynamic depth/stencil/cull state: %s, dynamic depth-bias enable: %s", eds_on ? "on (VK_EXT_extended_dynamic_state)" : "off", eds2_on ? "on" : "off");
    overlay_setup(family);
    { void mtl_overlay_start(void); mtl_overlay_start(); }
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VK_CHECK(vkCreateCommandPool(device, &ci, NULL, &commands));
    memprobe_run();
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_CHECK(vkCreateFence(device, &fi, NULL, &gpu_fence));
    pipeline_cache_initialize(&properties);
    VkQueryPoolCreateInfo qci={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1024};VK_CHECK(vkCreateQueryPool(device,&qci,NULL,&draw_queries));
    if(gpu_timestamp_bits&&properties.limits.timestampPeriod>0){
        gpu_timestamp_period=properties.limits.timestampPeriod;qci.queryType=VK_QUERY_TYPE_TIMESTAMP;qci.queryCount=2;
        VK_CHECK(vkCreateQueryPool(device,&qci,NULL,&batch_times));
        LOG("[vulkan] GPU timestamps bits=%u period=%.6f ns",gpu_timestamp_bits,gpu_timestamp_period);
    }
    if (surface) {
        atomic_store(&screen_dirty, 0);
        screen_chain fresh;
#ifdef __ANDROID__
        VkResult made = screen_build_chain(window, VK_NULL_HANDLE, NULL, &fresh);
#else
        VkResult made = screen_build_chain(NULL, VK_NULL_HANDLE, NULL, &fresh);
#endif
        if (made == VK_SUCCESS) screen_install_chain(&fresh);
        // A recoverable failure (zero-sized window etc.) is retried by the game thread at the top of its first present;
        // any other failure dies inside screen_build_chain.
        else { atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_RETRY); LOG("[vulkan-wsi] 【红】initial swapchain deferred result=%d; retried at the first present", made); }
    }
#ifdef __ANDROID__
    // The reference taken at initialisation is handed to screen_window (when there is a window); without a surface
    // (windowless fixtures) it is dropped.
    if (window) { if (surface) { screen_window = window; screen_request_rate(window); } else ANativeWindow_release(window); }
#endif
    screen_live = 1;
    pthread_mutex_unlock(&screen_lock);
    // Start background pipeline warm-up last: read the local accumulation and the shipped seed manifest, moving first
    // driver compilations from gameplay to start-up / the loading screen (nvn_vulkan_warm.inc).
    pso_warm_start();
}
void mtl_init(void) { pthread_once(&once, initialize); }

/* Small buffers (up to BUFFER_SLAB_MAX) live in shared slabs: one VkDeviceMemory of BUFFER_SLAB_BYTES, mapped once, cut
   into blocks of one size class, each block bound to its own VkBuffer. A memory object per buffer, each with its own
   mapping, reaches Android's limit of 65530 mappings per process in the open world (34,000 GPU mappings of ~19 KB after a
   minute), and vkMapMemory then fails. Blocks go back to their class's free list when the buffer is destroyed; slabs stay. */
#define BUFFER_SLAB_MAX (256u << 10)
#define BUFFER_SLAB_BYTES (8u << 20)
typedef struct buffer_slab_block { VkDeviceMemory memory; uint8_t *mapped; uint64_t offset; struct buffer_slab_block *next; } buffer_slab_block;
/* per class: 4 KiB << i, i = 0..6 */
static buffer_slab_block *buffer_slab_free[7];
static uint64_t buffer_slabs, buffer_slab_bytes;
static int buffer_slab_class(uint64_t capacity) { int i = 0; while ((4096ull << i) < capacity) i++; return i; }
/* holding buffer_cache_lock */
static int buffer_slab_take(uint64_t capacity, uint32_t type, VkDeviceMemory *memory, void **mapped, uint64_t *offset) {
    /* the free lists do not record the memory type: slabs serve the first type asked for (every buffer_create buffer has
       the same usage); a buffer needing another gets its own memory */
    static uint32_t slab_type = UINT32_MAX; if (slab_type == UINT32_MAX) slab_type = type; else if (type != slab_type) return 0;
    int c = buffer_slab_class(capacity);
    if (!buffer_slab_free[c]) {
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = BUFFER_SLAB_BYTES, .memoryTypeIndex = type};
        VkDeviceMemory m; void *map;
        if (vkAllocateMemory(device, &mi, NULL, &m)) return 0;
        if (vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &map)) { vkFreeMemory(device, m, NULL); return 0; }
        for (uint64_t o = BUFFER_SLAB_BYTES; o >= capacity; o -= capacity) {
            buffer_slab_block *k = malloc(sizeof *k); if (!k) die("Vulkan buffer slab allocation");
            *k = (buffer_slab_block){m, map, o - capacity, buffer_slab_free[c]}; buffer_slab_free[c] = k;
            if (o == capacity) break; }
        buffer_slabs++; buffer_slab_bytes += BUFFER_SLAB_BYTES;
        if (buffer_slabs <= 4 || !(buffer_slabs % 32)) LOG("[vulkan-buffer-slab] %llu slabs (%llu MiB) for buffers up to %u KiB", (unsigned long long)buffer_slabs,
            (unsigned long long)(buffer_slab_bytes >> 20), BUFFER_SLAB_MAX >> 10);
    }
    buffer_slab_block *k = buffer_slab_free[c]; buffer_slab_free[c] = k->next;
    *memory = k->memory; *mapped = k->mapped + k->offset; *offset = k->offset; free(k); return 1;
}
/* holding buffer_cache_lock */
static void buffer_slab_give(const vk_buffer *b) {
    buffer_slab_block *k = malloc(sizeof *k); if (!k) die("Vulkan buffer slab allocation");
    int c = buffer_slab_class(b->capacity); *k = (buffer_slab_block){b->memory, (uint8_t *)b->mapped - b->slab_offset, b->slab_offset, buffer_slab_free[c]}; buffer_slab_free[c] = k;
}
static vk_buffer buffer_create(uint64_t size) {
    if (!size || size > (1ull << 32)) die("Invalid Vulkan buffer size %llu", (unsigned long long)size);
    uint64_t capacity = 4096; while (capacity < size) capacity *= 2;
    pthread_mutex_lock(&buffer_cache_lock); cached_buffer **link = &buffer_cache;
    // Give each allocation or cache reuse a unique activation so reused handles/owners cannot match stale derived data.
    uint64_t activation=++buffer_activation;if(!activation)die("Vulkan buffer activation overflow");
    while (*link && (*link)->buffer.capacity != capacity) link = &(*link)->next;
    if (*link) { cached_buffer *entry = *link; *link = entry->next; vk_buffer b = entry->buffer;
        buffer_cache_bytes -= capacity; buffer_reuses++; active_buffer_bytes += capacity;
        if (capacity > DM_VK_SMALL_BUFFER_MAX) { buffer_large_cache_bytes -= capacity; buffer_large_reuses++; }
        atomic_store(&b.owner->references,1);b.owner->activation=activation;pthread_mutex_unlock(&buffer_cache_lock); free(entry); b.size = size; return b; }
    buffer_allocations++; active_buffer_bytes += capacity; if (capacity > DM_VK_SMALL_BUFFER_MAX) { buffer_large_allocations++; buffer_large_allocated_bytes += capacity; } pthread_mutex_unlock(&buffer_cache_lock);
    vk_buffer b = {.size = size, .capacity = capacity};b.owner=calloc(1,sizeof *b.owner);if(!b.owner)die("Vulkan buffer ownership allocation");atomic_init(&b.owner->references,1);b.owner->activation=activation;
    uint64_t probe_wall = dm_android_clock_ns(CLOCK_MONOTONIC), probe_cpu = dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID);
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(device, &ci, NULL, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, b.buffer, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { if(type==UINT32_MAX)type=i;if(memory_properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT){type=i;break;} }
    if (type == UINT32_MAX) die("Vulkan has no coherent host-visible buffer memory");
    int slab = 0;
    if (capacity <= BUFFER_SLAB_MAX && req.size <= capacity && !(capacity % req.alignment)) {
        pthread_mutex_lock(&buffer_cache_lock); slab = buffer_slab_take(capacity, type, &b.memory, &b.mapped, &b.slab_offset); pthread_mutex_unlock(&buffer_cache_lock); }
    if (slab) { b.slab = 1; VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, b.slab_offset)); }
    else {
        VkMemoryAllocateInfo mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        VK_CHECK(vkAllocateMemory(device, &mi, NULL, &b.memory)); VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    }
    buffer_create_wall_ns += dm_android_clock_ns(CLOCK_MONOTONIC) - probe_wall;
    buffer_create_cpu_ns += dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID) - probe_cpu;
    return b;
}
// Cross-batch consumers retain the same backing until the last genuinely completed user permits cache reuse.
static vk_buffer buffer_borrow(vk_buffer buffer) {
    if(!buffer.buffer||!buffer.owner)die("Vulkan buffer borrow without backing");
    unsigned references=atomic_fetch_add(&buffer.owner->references,1);if(!references||references==UINT32_MAX)die("Vulkan buffer reference overflow");return buffer;
}
static void buffer_destroy(vk_buffer *b) {
    if (!b->buffer) return;
    if(!b->owner)die("Vulkan buffer missing ownership");unsigned references=atomic_fetch_sub(&b->owner->references,1);if(!references)die("Vulkan buffer ownership underflow");
    if(references>1){memset(b,0,sizeof *b);return;}
    // Return snapshots only after GPU completion; never reuse in-flight buffers. Small buffers (<= 8 MiB) share 384 MiB: in
    // game TOTK frees ~200 a frame, which overflows 64 MiB. Large ones (8-64 MiB) get their own 128 MiB budget: camp/field
    // re-snapshot a ~19.5 MB read-only SSBO (32 MiB capacity) every frame it is dirty, and freeing it to re-create it with
    // vkAllocateMemory each time costs 0.9-5 ms per frame on the S24U in Turnip's kgsl_bo_init ioctl (kernel page
    // allocation + zeroing), 15% of the translation thread's samples in thread mode.
    pthread_mutex_lock(&buffer_cache_lock); active_buffer_bytes -= b->capacity;
    int small = b->capacity <= DM_VK_SMALL_BUFFER_MAX && buffer_cache_bytes - buffer_large_cache_bytes + b->capacity <= (384u << 20);
    int large = !small && b->capacity > DM_VK_SMALL_BUFFER_MAX && b->capacity <= DM_VK_LARGE_BUFFER_MAX && buffer_large_cache_bytes + b->capacity <= DM_VK_LARGE_CACHE_BUDGET;
    if (small || large) {
        cached_buffer *entry = malloc(sizeof *entry); if (!entry) die("Vulkan buffer cache allocation");
        entry->buffer = *b; entry->next = buffer_cache; buffer_cache = entry; buffer_cache_bytes += b->capacity;
        if (large) buffer_large_cache_bytes += b->capacity;
        pthread_mutex_unlock(&buffer_cache_lock); memset(b, 0, sizeof *b); return;
    }
    if (b->capacity > DM_VK_SMALL_BUFFER_MAX) buffer_large_freed++;
    if (b->slab) { buffer_slab_give(b); pthread_mutex_unlock(&buffer_cache_lock); vkDestroyBuffer(device, b->buffer, NULL); free(b->owner); memset(b, 0, sizeof *b); return; }
    pthread_mutex_unlock(&buffer_cache_lock);
    vkUnmapMemory(device, b->memory); vkDestroyBuffer(device, b->buffer, NULL); vkFreeMemory(device, b->memory, NULL);free(b->owner); memset(b, 0, sizeof *b);
}
/* TOTK_VK_POOLSTATS=1: memory pool layout for zero-copy planning (count, sizes, alignment, contiguous clusters, the
   /proc/self/maps regions holding them), every 600 presents */
static void pool_stats_log(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_POOLSTATS"); on = e && atoi(e); }
    if (!on || presents % 600) return;
    pthread_mutex_lock(&pools_lock);
    size_t n = pool_sorted_n; uint64_t total = 0, hist[6] = {0}, histb[6] = {0}, unaligned = 0, clusters = 0, overlap = 0; uintptr_t end = 0;
    uint64_t maxc = 0, cur = 0; uintptr_t cstart = 0;
    for (size_t i = 0; i < n; i++) { const pool_range *p = pool_sorted[i]; uintptr_t b0 = (uintptr_t)p->base; total += p->size;
        int k = p->size < (64 << 10) ? 0 : p->size < (1 << 20) ? 1 : p->size < (4 << 20) ? 2 : p->size < (16 << 20) ? 3 : p->size < (64 << 20) ? 4 : 5; hist[k]++; histb[k] += p->size;
        if ((b0 | p->size) & 4095) unaligned++;
        if (i && b0 < end) overlap++;
        if (!i || b0 > end + 65536) { clusters++; if (cur > maxc) maxc = cur; cstart = b0; }
        if (b0 + p->size > end) end = b0 + p->size; cur = end - cstart; }
    if (cur > maxc) maxc = cur;
    LOG("[poolstats] frame=%llu pools=%zu total=%.1f MiB unaligned=%llu overlapping=%llu clusters(gap<=64K)=%llu largest_cluster=%.1f MiB",
        (unsigned long long)presents, n, total / 1048576.0, (unsigned long long)unaligned, (unsigned long long)overlap, (unsigned long long)clusters, maxc / 1048576.0);
    LOG("[poolstats] sizes <64K %llu (%.1f MiB), <1M %llu (%.1f MiB), <4M %llu (%.1f MiB), <16M %llu (%.1f MiB), <64M %llu (%.1f MiB), >=64M %llu (%.1f MiB)",
        (unsigned long long)hist[0], histb[0] / 1048576.0, (unsigned long long)hist[1], histb[1] / 1048576.0, (unsigned long long)hist[2], histb[2] / 1048576.0,
        (unsigned long long)hist[3], histb[3] / 1048576.0, (unsigned long long)hist[4], histb[4] / 1048576.0, (unsigned long long)hist[5], histb[5] / 1048576.0);
    FILE *f = fopen("/proc/self/maps", "r"); char line[512]; size_t i = 0;
    while (f && fgets(line, sizeof line, f) && i < n) { unsigned long lo, hi; if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        size_t c = 0; uint64_t bytes = 0; while (i < n && (uintptr_t)pool_sorted[i]->base < hi) { if ((uintptr_t)pool_sorted[i]->base >= lo) { c++; bytes += pool_sorted[i]->size; } i++; }
        if (c >= 50 || bytes >= (64ull << 20)) { line[strcspn(line, "\n")] = 0; LOG("[poolstats] mapping %.120s: %zu pools %.1f MiB", line, c, bytes / 1048576.0); } }
    if (f) fclose(f);
    pthread_mutex_unlock(&pools_lock);
}
int compat_guest_overlay(void *address, size_t size, int dmabuf);
int compat_guest_unoverlay(void *address, size_t size);
unsigned compat_overlay_count(void);
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) __attribute__((weak)); void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) __attribute__((weak));
static uint64_t zc_pools, zc_bytes, zc_failures;
static uint64_t zc_pool_hist[5], zc_pool_hist_bytes[5];
static uint32_t zc_memory_type = UINT32_MAX;
static PFN_vkGetMemoryFdKHR zc_get_fd;
/* Back a pool with exported cached Vulkan memory; 0 when not enabled / not possible (the pool stays memfd memory). */
static int zc_pool_back(pool_range *p) {
    static int on = -1; if (on < 0) { const char *z = getenv("TOTK_VK_ZEROCOPY"); on = z && atoi(z) && ext_dma_buf; if (z && atoi(z) && !ext_dma_buf) LOG("[zerocopy] off: the driver has no dma-buf export"); }
    /* Pools under the floor stay guest memory. It is 4 KiB by default: the game's uniform rings and small vertex pools are
       4-64 KiB pools (~1,600 of 2,048 pools are under 64 KiB). Each export is a dma-buf fd and a mapping of its own, and
       dma-buf pools have 64 KiB dirty-tracker blocks. TOTK_VK_ZEROCOPY_MIN=<bytes> sets it. */
    static uint64_t floor_bytes; if (!floor_bytes) { const char *m = getenv("TOTK_VK_ZEROCOPY_MIN"); floor_bytes = m && atoll(m) >= 4096 ? (uint64_t)atoll(m) : 4096; }
    { unsigned c = p->size < 4096 ? 0 : p->size < 16384 ? 1 : p->size < 65536 ? 2 : p->size < (1u << 20) ? 3 : 4; zc_pool_hist[c]++; zc_pool_hist_bytes[c] += p->size; }
    if (!on || p->size < floor_bytes || ((uintptr_t)p->base | p->size) & 4095) return 0;
    VkExternalMemoryBufferCreateInfo xb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &xb, .size = p->size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VkBuffer buf; if (vkCreateBuffer(device, &bi, NULL, &buf)) { zc_failures++; return 0; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(device, buf, &rq);
    if (zc_memory_type == UINT32_MAX) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t t = 0; t < memory_properties.memoryTypeCount && zc_memory_type == UINT32_MAX; t++)
            if ((rq.memoryTypeBits & (1u << t)) && (memory_properties.memoryTypes[t].propertyFlags & want) == want) zc_memory_type = t;
        zc_get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
        LOG("[zerocopy] on: pools of 64 KiB and up are exported Vulkan memory (type %d)", (int)zc_memory_type);
    }
    VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex, .allocationSize = rq.size, .memoryTypeIndex = zc_memory_type};
    VkDeviceMemory mem; int fd = -1;
    if (zc_memory_type == UINT32_MAX || rq.size != p->size || !zc_get_fd || vkAllocateMemory(device, &ai, NULL, &mem)) { vkDestroyBuffer(device, buf, NULL); zc_failures++; return 0; }
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (vkBindBufferMemory(device, buf, mem, 0) || zc_get_fd(device, &gi, &fd) || fd < 0 || compat_guest_overlay((void *)p->base, p->size, fd)) {
        if (fd >= 0) close(fd); vkDestroyBuffer(device, buf, NULL); vkFreeMemory(device, mem, NULL); zc_failures++;
        if (zc_failures <= 8) LOG("[zerocopy] pool %p+%llu stays guest memory (rq %llu)", p->base, (unsigned long long)p->size, (unsigned long long)rq.size);
        return 0; }
    p->zc_memory = mem; p->zc_buffer = buf; p->zc_fd = fd; zc_pools++; zc_bytes += p->size;
    if (!(zc_pools & (zc_pools - 1)) || p->size >= (64u << 20)) LOG("[zerocopy] %llu pools, %.1f MiB backed (latest %p+%.1f MiB); %llu stay guest memory; pools added by size: <4K %llu, <16K %llu (%.1f MiB), <64K %llu (%.1f MiB), <1M %llu, larger %llu",
        (unsigned long long)zc_pools, zc_bytes / 1048576.0, p->base, p->size / 1048576.0, (unsigned long long)zc_failures,
        (unsigned long long)zc_pool_hist[0], (unsigned long long)zc_pool_hist[1], zc_pool_hist_bytes[1] / 1048576.0, (unsigned long long)zc_pool_hist[2], zc_pool_hist_bytes[2] / 1048576.0, (unsigned long long)zc_pool_hist[3], (unsigned long long)zc_pool_hist[4]);
    return 1;
}
/* nvn_dirty.c: is the pool at mem dma-buf memory? Its dirty tracking then works in 64 KiB blocks (dma-buf mappings never
   merge after an mprotect split; see nvn_dirty.c range_t). */
int mtl_pool_zero_copy(const void *mem) {
    int zc = 0; pthread_mutex_lock(&pools_lock);
    for (pool_range *p = pools; p; p = p->next) if (p->base == mem) { zc = p->zc_memory != VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&pools_lock); return zc;
}
static void zc_pool_release(pool_range *p) {
    if (!p->zc_memory) return;
    if (compat_guest_unoverlay((void *)p->base, p->size)) LOG("[zerocopy] pool %p: restoring guest memory failed", p->base);
    close(p->zc_fd); vkDestroyBuffer(device, p->zc_buffer, NULL); vkFreeMemory(device, p->zc_memory, NULL);
    zc_pools--; zc_bytes -= p->size; p->zc_memory = VK_NULL_HANDLE;
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init(); if (!mem || !size || size > UINTPTR_MAX - (uintptr_t)mem) die("Invalid NVN pool");
    if (getenv("DM_VK_TRACE")) { Dl_info info; dladdr((void *)mtl_pool_add, &info); LOG("[vulkan] pool=%p+%llu host=%p thread=%lu stack=%p", mem, (unsigned long long)size, info.dli_fbase, (unsigned long)pthread_self(), __builtin_frame_address(0)); }
    pool_range *p = calloc(1, sizeof *p); if (!p) die("NVN pool allocation");
    p->base = mem; p->size = size; zc_pool_back(p); pthread_mutex_lock(&pools_lock); p->next = pools; pools = p; pool_sorted_rebuild_locked(); pthread_mutex_unlock(&pools_lock); atomic_fetch_add(&pool_adds, 1);
}
void mtl_pool_del(const void *mem) {
    mtl_finish();pthread_mutex_lock(&queue_lock);
    pthread_mutex_lock(&pools_lock); pool_range **link = &pools;
    while (*link && (*link)->base != mem) link = &(*link)->next;
    if (*link) { pool_range *p = *link; *link = p->next; read_cache_remove((uintptr_t)p->base,p->size); zc_pool_release(p); free(p); pool_sorted_rebuild_locked(); atomic_fetch_add(&pool_adds, 1); } pthread_mutex_unlock(&pools_lock);
    pthread_mutex_unlock(&queue_lock);
}
/* The zero-copy buffer behind [addr, addr+len) (VK_NULL_HANDLE when the range is not in a zero-copy pool). Pools are only
   removed after mtl_finish under queue_lock, so the handle stays valid for the recording that asked. */
typedef struct { uint64_t base, size; VkBuffer buffer; } zc_entry;
/* this thread's copy of the sorted pool table (pool_adds is its generation) */
static _Thread_local struct { zc_entry *e; size_t n, cap; uint64_t gen; } zc_local;
static VkBuffer zc_buffer_at(uint64_t addr, uint64_t len, uint64_t *off) {
    uint64_t gen = atomic_load(&pool_adds);
    if (zc_local.gen != gen || !zc_local.e) {
        pthread_mutex_lock(&pools_lock);
        if (zc_local.cap < pool_sorted_n) { zc_local.cap = pool_sorted_n + 256; zc_local.e = realloc(zc_local.e, zc_local.cap * sizeof *zc_local.e); if (!zc_local.e) die("zero-copy pool table"); }
        for (size_t i = 0; i < pool_sorted_n; i++) zc_local.e[i] = (zc_entry){(uintptr_t)pool_sorted[i]->base, pool_sorted[i]->size, pool_sorted[i]->zc_buffer};
        zc_local.n = pool_sorted_n; zc_local.gen = gen;
        pthread_mutex_unlock(&pools_lock);
    }
    size_t lo = 0, hi = zc_local.n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (zc_local.e[m].base <= addr) lo = m + 1; else hi = m; }
    if (lo) { const zc_entry *p = &zc_local.e[lo - 1];
        if (p->buffer && addr >= p->base && addr - p->base <= p->size && len <= p->size - (addr - p->base)) { *off = addr - p->base; return p->buffer; } }
    return VK_NULL_HANDLE;
}
/* in-place fallback diagnosis: where does a range that is not in a zero-copy pool live */
enum { IP_NOPOOL, IP_POOL_4K, IP_POOL_16K, IP_POOL_64K, IP_POOL_BIG_NOZC, IP_ZC_RANGE, IP_CLASSES }; static uint64_t inplace_class[IP_CLASSES]; static uint64_t inplace_class_bytes[IP_CLASSES];
static void inplace_classify(uint64_t addr, uint64_t len) {
    int c = IP_NOPOOL; pthread_mutex_lock(&pools_lock);
    size_t lo = 0, hi = pool_sorted_n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if ((uintptr_t)pool_sorted[m]->base <= addr) lo = m + 1; else hi = m; }
    if (lo) { const pool_range *p = pool_sorted[lo - 1]; uint64_t base = (uintptr_t)p->base;
        if (addr >= base && addr - base < p->size) c = p->zc_buffer ? IP_ZC_RANGE : p->size < 4096 ? IP_POOL_4K : p->size < 16384 ? IP_POOL_16K : p->size < 65536 ? IP_POOL_64K : IP_POOL_BIG_NOZC; }
    pthread_mutex_unlock(&pools_lock); inplace_class[c]++; inplace_class_bytes[c] += len;
}
static pthread_t async_recording_thread;
/* The translation thread's last pool per 64 KiB block, valid while pool_gen is unchanged: mtl_addr runs several times per
   draw and its lock + search is ~19% of snapshot_buffer without this. Only a pool that no later pool overlaps is kept, so
   the memo gives the same pool as the search below for every address inside it. */
static struct { uint64_t gen; uintptr_t base; uint64_t size; } mtl_addr_memo[64];
/* The bytes from addr to the end of its pool, 0 outside the pools. */
static uint64_t mtl_room(uint64_t addr) {
    uint64_t room = 0; pthread_mutex_lock(&pools_lock);
    for (size_t i = 0; i < pool_sorted_n; i++) { uint64_t base = (uintptr_t)pool_sorted[i]->base; if (base > addr) break; if (addr - base < pool_sorted[i]->size && pool_sorted[i]->size - (addr - base) > room) room = pool_sorted[i]->size - (addr - base); }
    pthread_mutex_unlock(&pools_lock); return room;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int mine = pthread_equal(pthread_self(), async_recording_thread);
    if (mine) {
        typeof(mtl_addr_memo[0]) *m = &mtl_addr_memo[(addr >> 16) & 63];
        if (m->size && m->gen == atomic_load_explicit(&pool_gen, memory_order_acquire) && addr >= m->base && addr - m->base <= m->size && len <= m->size - (addr - m->base)) {
            if (buf) *buf = (void *)m->base; if (off) *off = addr - m->base; return 1; }
    }
    int found = 0; pthread_mutex_lock(&pools_lock);
    /* last pool starting at or below addr */
    size_t lo = 0, hi = pool_sorted_n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if ((uintptr_t)pool_sorted[m]->base <= addr) lo = m + 1; else hi = m; }
    #define POOL_HIT(p) ({ uint64_t base_ = (uintptr_t)(p)->base; addr >= base_ && addr - base_ <= (p)->size && len <= (p)->size - (addr - base_); })
    const pool_range *hit = lo && POOL_HIT(pool_sorted[lo - 1]) ? pool_sorted[lo - 1] : NULL;
    /* overlapping pools (rare) */
    if (!hit) for (size_t i = lo; i-- > 0;) if (POOL_HIT(pool_sorted[i])) { hit = pool_sorted[i]; break; }
    #undef POOL_HIT
    if (hit) { if (buf) *buf = (void *)hit->base; if (off) *off = addr - (uintptr_t)hit->base; found = 1;
        if (mine && hit == pool_sorted[lo - 1] && (lo == pool_sorted_n || (uintptr_t)pool_sorted[lo]->base >= (uintptr_t)hit->base + hit->size)) {
            typeof(mtl_addr_memo[0]) *m = &mtl_addr_memo[(addr >> 16) & 63];
            m->gen = atomic_load_explicit(&pool_gen, memory_order_relaxed); m->base = (uintptr_t)hit->base; m->size = hit->size; } }
    pthread_mutex_unlock(&pools_lock); return found;
}
static uint64_t post_batch_serial;
static post_op *post_new(int kind) {
    if (!recording) die("NVN operation outside a submission");
    if(post_order==DM_PENDING_ORDER_ALL-1)die("Vulkan post order overflow");
    post_op *p = calloc(1, sizeof *p); if (!p) die("NVN completion allocation");
    p->kind = kind;p->order=++post_order;p->batch_serial=post_batch_serial; *post_tail = p; post_tail = &p->next;
    if(kind==0){*post_copy_tail=p;post_copy_tail=&p->copy_next;}
    if(kind==1||kind==2)pending_host_sync=1;
    return p;
}

static void cond_pending_done(uint64_t addr);
static void image_writeback_check(post_op *p);static void image_writeback_publish(post_op *p);static void image_writeback_sweep(void);
#include "nvn_vulkan_memprobe.inc"
/* freeze-frame: GPU write-backs of the recorded frame (nvn_vulkan_freeze.inc) */
static int frz_noting; static void frz_note(uintptr_t address, uint64_t size);
static uintptr_t gpu_watch_address(void); static int gpu_watch_hit(uintptr_t a, uint64_t size);
/* TOTK_VK_WATCH_MEM: the first host step that changed the 0x200 bytes from the watched address rounded down to 0x100
   ("post" kind | site << 8 after a batch publication, "image-writeback", "frame" once a frame for any other writer) */
static void gpu_watch_changed(const char *who,uint64_t info) {
    uintptr_t w=gpu_watch_address();if(!w)return;
    static uint8_t last[0x200];static int have;static unsigned shown;const uint8_t *now=(const uint8_t *)(w&~(uintptr_t)0xff);
    if(!have){memcpy(last,now,sizeof last);have=1;return;}
    if(!memcmp(last,now,sizeof last))return;
    for(unsigned o=0;o<sizeof last;o+=8)if(memcmp(last+o,now+o,8)&&shown++<400){uint64_t a,b;memcpy(&a,last+o,8);memcpy(&b,now+o,8);
        LOG("[vulkan-watch] %s %#llx frame %llu: %#llx %#llx -> %#llx",who,(unsigned long long)info,(unsigned long long)presents,(unsigned long long)(uintptr_t)(now+o),(unsigned long long)a,(unsigned long long)b);}
    memcpy(last,now,sizeof last);
}
#include "nvn_vulkan_async.inc"
#include "nvn_vulkan_guest_publish.inc"
#include "nvn_vulkan_forward.inc"

void mtl_sub_begin(void) {
    mtl_init(); pthread_mutex_lock(&queue_lock); if (recording) die("Nested NVN submission");
    snapshot_diag_poll();
    unsigned slot=async_begin_locked();post_batch_serial=async_current->serial;batch_resources_select(slot);forward_reset();forward_scope_marker=NULL;pending_index_report();
    VK_CHECK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    /* only the queries used last time: all 1024 each batch costs 3% of the replay */
    VK_CHECK(vkBeginCommandBuffer(command, &bi)); recording = 1;async_recording_thread=pthread_self();async_recording_owned=1;graphics_command_reset(); post_first = NULL; post_tail = &post_first;post_copy_first=NULL;post_copy_tail=&post_copy_first;pending_host_sync=0;post_order=0; batch_draws = 0;used_queries=0;{uint32_t n=async_current->queries_reset_once?async_current->queries_to_reset:1024;async_current->queries_reset_once=1;if(n)vkCmdResetQueryPool(command,draw_queries,0,n);}
    drawstat_batch_begin((unsigned)(async_current-async_batches),command);
    passtime_batch_begin((unsigned)(async_current-async_batches),command);
    counter_gpu_batch_begin(async_current);
    if(batch_times){vkCmdResetQueryPool(command,batch_times,0,2);vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,batch_times,0);}
    async_current->chunk_started=dm_android_clock_ns(CLOCK_MONOTONIC);
    /* the frame's first batch */
    { static uint64_t swept=UINT64_MAX;if(presents!=swept){swept=presents;gpu_watch_changed("frame",presents);image_writeback_sweep();} }
}
// Handle counter wrap using the queue's advertised bits; never substitute a CPU clock for GPU measurements.
static uint64_t timestamp_delta(uint64_t begin,uint64_t end,unsigned bits) {
    return (end-begin)&(bits>=64?UINT64_MAX:bits?((1ull<<bits)-1):0);
}
static void snapshot_prepare(void) {
    // Ordinary copies have known CPU inputs and can be forwarded; shader writebacks and events/counters must complete first.
    /* 256 draws a batch: 128 and the mid-pass chunk split (async_chunk_due) gain the same ~0.4 ms and do not add up;
       512 and 1024 starve the GPU (chunks cut only between passes) and 64 costs more than it gains.
       TOTK_VK_BATCH_DRAWS sets it for an A/B. */
    static unsigned cap; if (!cap) { const char *e = getenv("TOTK_VK_BATCH_DRAWS"); cap = e && atoi(e) > 0 ? (unsigned)atoi(e) : 256; }
    int flush = batch_draws >= cap || async_batch_temporary_bytes() >= (32u << 20);
    if (flush) { mtl_sub_commit(); mtl_sub_begin(); }
}
typedef struct owned_snapshot_counters {
    uint64_t calls,large_calls,large_bytes,known_calls,known_bytes,safe_calls,safe_requested_bytes,safe_errors;
} owned_snapshot_counters;
static owned_snapshot_counters owned_snapshot_stats;
// How snapshot_read (the generic snapshot read into non-owned destinations) was served: heap lease / safe reader / known copy (see snapshot_read).
typedef struct plain_snapshot_counters { uint64_t safe_calls,safe_bytes,safe_errors,known_calls; } plain_snapshot_counters;
static plain_snapshot_counters plain_snapshot_stats;
// Sample one whole frame every 300 frames under the queue lock, at most 12 frames; fixed tables/ring never allocate or read guest contents.
enum { SNAP_DIAG_HIT,SNAP_DIAG_NO_ENTRY,SNAP_DIAG_DIRTY,SNAP_DIAG_LIMIT,SNAP_DIAG_POOL,SNAP_DIAG_UNTRACKED,SNAP_DIAG_PENDING,SNAP_DIAG_LRU,SNAP_DIAG_REASONS };
typedef struct snapshot_source {
    const char *key; uint32_t stage,set,binding,element,kind,flags,cb_slot,cb_offset,slot,handle,role;
    uint64_t bound_bytes;
} snapshot_source;
typedef struct snapshot_diag_row {
    snapshot_source source; char key[17]; uintptr_t address; uint64_t size,calls,forward[7],reasons[SNAP_DIAG_REASONS],history[SNAP_DIAG_REASONS];
    uint64_t old_entries,old_stamp,new_stamp,known_calls,safe_calls,safe_bytes;
} snapshot_diag_row;
typedef struct snapshot_diag_retirement { uintptr_t address; uint64_t size; unsigned reason; } snapshot_diag_retirement;
static snapshot_diag_row snapshot_diag_rows[32],*snapshot_diag_current;
static const snapshot_source *snapshot_diag_source_current;
static forward_result snapshot_diag_forward;
static snapshot_diag_retirement snapshot_diag_retirements[64];
static uint64_t snapshot_diag_frame,snapshot_diag_overflow_calls,snapshot_diag_overflow_bytes;
static unsigned snapshot_diag_count,snapshot_diag_retire_cursor;
static const char *snapshot_diag_role(unsigned role) {return role==1?"readonly_ssbo":role==2?"compute_ubo_pointer":"readonly_texel";}
static void snapshot_diag_poll(void) {
    int saved_errno=errno;uint64_t next=presents+1;
    if(snapshot_diag_frame&&snapshot_diag_frame!=next){
        LOG("[vulkan-snapshot-sample] frame=%llu rows=%u overflow_calls=%llu overflow_bytes=%llu scope=shader_readonly_over64K write_marks_are_not_content_diff=1",
            (unsigned long long)snapshot_diag_frame,snapshot_diag_count,(unsigned long long)snapshot_diag_overflow_calls,(unsigned long long)snapshot_diag_overflow_bytes);
        for(unsigned i=0;i<snapshot_diag_count;i++){snapshot_diag_row *r=&snapshot_diag_rows[i];snapshot_source *s=&r->source;
            LOG("[vulkan-snapshot-source] frame=%llu row=%u shader=%s stage=%u set=%u binding=%u element=%u kind=%u flags=%#x cb_slot=%u cb_offset=%u nvn_slot=%u handle=%u role=%s address=%#llx bytes=%llu bound_bytes=%llu calls=%llu forward_none/full/partial/unrecorded/alignment/bounds/host_effect=%llu/%llu/%llu/%llu/%llu/%llu/%llu cache_hit=%llu miss_no_entry/dirty/limit/pool/untracked/pending=%llu/%llu/%llu/%llu/%llu/%llu absent_last_retire_dirty/limit/pool/untracked/pending/lru=%llu/%llu/%llu/%llu/%llu/%llu old_entries=%llu last_old_stamp=%llu last_new_stamp=%llu known_calls=%llu safe_calls/bytes=%llu/%llu",
                (unsigned long long)snapshot_diag_frame,i,r->key,s->stage,s->set,s->binding,s->element,s->kind,s->flags,s->cb_slot,s->cb_offset,s->slot,s->handle,snapshot_diag_role(s->role),
                (unsigned long long)r->address,(unsigned long long)r->size,(unsigned long long)s->bound_bytes,(unsigned long long)r->calls,
                (unsigned long long)r->forward[0],(unsigned long long)r->forward[1],(unsigned long long)r->forward[2],(unsigned long long)r->forward[3],(unsigned long long)r->forward[4],(unsigned long long)r->forward[5],(unsigned long long)r->forward[6],
                (unsigned long long)r->reasons[SNAP_DIAG_HIT],(unsigned long long)r->reasons[SNAP_DIAG_NO_ENTRY],(unsigned long long)r->reasons[SNAP_DIAG_DIRTY],(unsigned long long)r->reasons[SNAP_DIAG_LIMIT],(unsigned long long)r->reasons[SNAP_DIAG_POOL],(unsigned long long)r->reasons[SNAP_DIAG_UNTRACKED],(unsigned long long)r->reasons[SNAP_DIAG_PENDING],
                (unsigned long long)r->history[SNAP_DIAG_DIRTY],(unsigned long long)r->history[SNAP_DIAG_LIMIT],(unsigned long long)r->history[SNAP_DIAG_POOL],(unsigned long long)r->history[SNAP_DIAG_UNTRACKED],(unsigned long long)r->history[SNAP_DIAG_PENDING],(unsigned long long)r->history[SNAP_DIAG_LRU],
                (unsigned long long)r->old_entries,(unsigned long long)r->old_stamp,(unsigned long long)r->new_stamp,(unsigned long long)r->known_calls,(unsigned long long)r->safe_calls,(unsigned long long)r->safe_bytes);
        }
        snapshot_diag_frame=0;snapshot_diag_count=0;snapshot_diag_overflow_calls=0;snapshot_diag_overflow_bytes=0;memset(snapshot_diag_rows,0,sizeof snapshot_diag_rows);
    }
    if(!snapshot_diag_frame&&next<=3600&&next%300==0)snapshot_diag_frame=next;
    errno=saved_errno;
}
static snapshot_diag_row *snapshot_diag_source(const snapshot_source *source,uintptr_t address,uint64_t size) {
    if(!snapshot_diag_frame||size<=65536)return NULL;
    int saved_errno=errno;
    for(unsigned i=0;i<snapshot_diag_count;i++){snapshot_diag_row *r=&snapshot_diag_rows[i];const snapshot_source *s=&r->source;
        if(r->address==address&&r->size==size&&s->binding==source->binding&&s->element==source->element&&s->stage==source->stage&&s->set==source->set&&s->role==source->role&&s->slot==source->slot&&s->handle==source->handle&&s->bound_bytes==source->bound_bytes&&!strcmp(r->key,source->key)){r->calls++;errno=saved_errno;return r;}}
    if(snapshot_diag_count==32){snapshot_diag_overflow_calls++;snapshot_diag_overflow_bytes+=size;errno=saved_errno;return NULL;}
    snapshot_diag_row *r=&snapshot_diag_rows[snapshot_diag_count++];r->source=*source;memcpy(r->key,source->key,17);r->source.key=r->key;r->address=address;r->size=size;r->calls=1;errno=saved_errno;return r;
}
static snapshot_diag_row *snapshot_diag_miss_row(uintptr_t address,uint64_t size) {
    if(snapshot_diag_current||!snapshot_diag_source_current||!snapshot_diag_frame||size<=65536)return snapshot_diag_current;
    int saved_errno=errno;
    // An actual miss may replace a hit-only row when full; replaced observations count as overflow so limits remain visible.
    for(unsigned i=0;i<snapshot_diag_count;i++){snapshot_diag_row *r=&snapshot_diag_rows[i];uint64_t misses=0;for(unsigned j=1;j<SNAP_DIAG_REASONS;j++)misses+=r->reasons[j];
        if(!misses){snapshot_diag_overflow_calls+=r->calls;snapshot_diag_overflow_bytes+=r->calls*r->size;memset(r,0,sizeof *r);r->source=*snapshot_diag_source_current;memcpy(r->key,r->source.key,17);r->source.key=r->key;r->address=address;r->size=size;r->calls=1;r->forward[snapshot_diag_forward]++;snapshot_diag_current=r;
            // This call was counted as overflow of the full table before it got this source row.
            snapshot_diag_overflow_calls--;snapshot_diag_overflow_bytes-=size;errno=saved_errno;return r;}}
    errno=saved_errno;return NULL;
}
static void snapshot_diag_retired(const read_snapshot *entry,unsigned reason) {
    if(entry->buffer.size<=65536)return;
    snapshot_diag_retirements[snapshot_diag_retire_cursor++%64]=(snapshot_diag_retirement){entry->address,entry->buffer.size,reason};
}
static void snapshot_diag_absent(snapshot_diag_row *row) {
    if(!row)return;
    // Report only the latest retirement actually recorded in the ring; no record means unknown history, not proven first creation.
    unsigned count=snapshot_diag_retire_cursor<64?snapshot_diag_retire_cursor:64;
    for(unsigned i=0;i<count;i++){const snapshot_diag_retirement *r=&snapshot_diag_retirements[(snapshot_diag_retire_cursor-1-i)%64];if(r->address==row->address&&r->size==row->size){row->history[r->reason]++;break;}}
}
static void snapshot_read_account_at(uint64_t size, const void *caller);
/* TOTK_VK_FRAMEPROF: snapshot bytes per call site this frame */
static struct { const void *caller; uint64_t bytes, calls; } snap_callers[32];
#define snapshot_read_account(size) snapshot_read_account_at((size), __builtin_return_address(0))
static void snapshot_read_account_at(uint64_t size, const void *caller) {
    { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_FRAMEPROF"); on = e && atoi(e); }
      if (on) { unsigned i = 0; while (i < 32 && snap_callers[i].caller && snap_callers[i].caller != caller) i++;
          if (i < 32) { snap_callers[i].caller = caller; snap_callers[i].bytes += size; snap_callers[i].calls++; } } }
    // Keep logical snapshot byte accounting; report avoided guest reads/redundant overlays separately, never as eliminated GPU uploads.
    static uint64_t metric_frame,metric_hits,metric_reads,metric_overlays,metric_final;
    if(presents&&presents%60==0&&presents!=metric_frame){uint64_t frames=presents-metric_frame;
        LOG("[vulkan-known-copy] frame=%llu hits/frame=%.1f guest_read_saved=%.2f MiB/frame overlay_write_saved=%.2f MiB/frame final_copy=%.2f MiB/frame",(unsigned long long)presents,
            (snapshot_known_copy_hits-metric_hits)/(double)frames,(snapshot_known_copy_read_saved-metric_reads)/(frames*1048576.),
            (snapshot_known_copy_overlay_saved-metric_overlays)/(frames*1048576.),(snapshot_known_copy_final_bytes-metric_final)/(frames*1048576.));
        metric_frame=presents;metric_hits=snapshot_known_copy_hits;metric_reads=snapshot_known_copy_read_saved;metric_overlays=snapshot_known_copy_overlay_saved;metric_final=snapshot_known_copy_final_bytes;
    }
    static uint64_t owned_frame;static owned_snapshot_counters previous;
    if(presents&&presents%60==0&&presents!=owned_frame){uint64_t frames=presents-owned_frame;
        const owned_snapshot_counters *s=&owned_snapshot_stats;
        LOG("[vulkan-owned-snapshot] frame=%llu span=%llu calls=%.1f/frame large_read_candidate=%.1f/frame large_candidate_bytes=%.3f MiB/frame known_copy=%.1f/frame known_bytes=%.3f MiB/frame safe_read=%.1f/frame safe_requested=%.3f MiB/frame safe_errors=%llu (CPU snapshot bytes, not GPU upload)",
            (unsigned long long)presents,(unsigned long long)frames,
            (s->calls-previous.calls)/(double)frames,(s->large_calls-previous.large_calls)/(double)frames,
            (s->large_bytes-previous.large_bytes)/(frames*1048576.),
            (s->known_calls-previous.known_calls)/(double)frames,(s->known_bytes-previous.known_bytes)/(frames*1048576.),
            (s->safe_calls-previous.safe_calls)/(double)frames,(s->safe_requested_bytes-previous.safe_requested_bytes)/(frames*1048576.),
            (unsigned long long)(s->safe_errors-previous.safe_errors));
        owned_frame=presents;previous=*s;
    }
    static uint64_t plain_frame;static plain_snapshot_counters plain_previous;
    if(presents&&presents%60==0&&presents!=plain_frame){uint64_t frames=presents-plain_frame;const plain_snapshot_counters *p=&plain_snapshot_stats;
        LOG("[vulkan-plain-snapshot] frame=%llu span=%llu safe_read=%.1f/frame safe_bytes=%.3f MiB/frame safe_errors=%llu known_copy=%.1f/frame (snapshot_read: copy sources, storage seeds, texture shadows)",
            (unsigned long long)presents,(unsigned long long)frames,(p->safe_calls-plain_previous.safe_calls)/(double)frames,
            (p->safe_bytes-plain_previous.safe_bytes)/(frames*1048576.),(unsigned long long)(p->safe_errors-plain_previous.safe_errors),
            (p->known_calls-plain_previous.known_calls)/(double)frames);
        plain_frame=presents;plain_previous=*p;
    }
    snapshot_bytes += size;
}
static int snapshot_read(uintptr_t address, void *destination, uint64_t size) {
    if (size > UINTPTR_MAX - address) return -1;
    // Overlay pending copy results in command order; guest memory still changes only when submission completes.
    if(!async_read_known_full_copy(address,destination,size)){
        plain_snapshot_stats.safe_calls++;plain_snapshot_stats.safe_bytes+=size;
        if(dm_android_read_memory(address,destination,size)){plain_snapshot_stats.safe_errors++;return -1;}
        async_overlay_copies(address,destination,size);
    }else plain_snapshot_stats.known_calls++;
    snapshot_read_account(size);return 0;
}
static int snapshot_read_owned(uintptr_t address,const vk_buffer *destination,uint64_t size) {
    // Accept only a complete fresh CPU target from buffer_create: zero offset and exclusive owner, never a cache borrow or stack/shadow.
    if(!destination||!destination->buffer||!destination->memory||!destination->mapped||!destination->owner||
       !destination->owner->activation||atomic_load(&destination->owner->references)!=1||!size||destination->size!=size||
       size>destination->capacity||size>SIZE_MAX||size>UINTPTR_MAX-address||size>UINTPTR_MAX-(uintptr_t)destination->mapped)return -1;
    owned_snapshot_stats.calls++;
    if(size>65536){owned_snapshot_stats.large_calls++;owned_snapshot_stats.large_bytes+=size;}
    if(async_read_known_full_copy(address,destination->mapped,size)){
        owned_snapshot_stats.known_calls++;owned_snapshot_stats.known_bytes+=size;
    }else{
        owned_snapshot_stats.safe_calls++;owned_snapshot_stats.safe_requested_bytes+=size;
        if(dm_android_read_memory(address,destination->mapped,size)){owned_snapshot_stats.safe_errors++;return -1;}
        async_overlay_copies(address,destination->mapped,size);
    }
    snapshot_read_account(size);return 0;
}
static int snapshot_pending(uintptr_t address, uint64_t size) {
    return async_copy_pending(address,size);
}
// Hash indexing plus a doubly linked LRU: hits/retirement avoid full-cache scans; pool removal still invalidates actual address ranges.
static unsigned read_bucket(uintptr_t address,uint64_t size) {
    uint64_t key=(address>>4)^(size*0x9e3779b97f4a7c15ull);key^=key>>33;key*=0xff51afd7ed558ccdull;key^=key>>33;return key&8191;
}
static unsigned read_snapshot_bucket(const read_snapshot *entry) {return read_bucket(entry->address,entry->buffer.size);}
static void read_lru_front(read_snapshot *entry) {
    if(read_snapshots==entry)return;
    if(entry->prev)entry->prev->next=entry->next;
    if(entry->next)entry->next->prev=entry->prev;
    if(read_snapshot_tail==entry)read_snapshot_tail=entry->prev;
    entry->prev=NULL;entry->next=read_snapshots;if(read_snapshots)read_snapshots->prev=entry;else read_snapshot_tail=entry;read_snapshots=entry;
}
static void read_cache_retire(read_snapshot *entry) {
    unsigned bucket=read_snapshot_bucket(entry);read_snapshot **link=&read_index[bucket];while(*link&&*link!=entry)link=&(*link)->hash_next;
    if(!*link)die("Vulkan read cache index corruption");*link=entry->hash_next;
    if(entry->prev)entry->prev->next=entry->next;else read_snapshots=entry->next;
    if(entry->next)entry->next->prev=entry->prev;else read_snapshot_tail=entry->prev;
    read_cache_bytes-=entry->buffer.capacity;if(recording)post_new(3)->source=entry->buffer;else buffer_destroy(&entry->buffer);free(entry);
}
static void read_cache_remove(uintptr_t address,uint64_t size) {
    for(read_snapshot *entry=read_snapshots,*next;entry;entry=next){next=entry->next;if(address<entry->address+entry->buffer.size&&entry->address<address+size){snapshot_diag_retired(entry,SNAP_DIAG_POOL);read_cache_retire(entry);}}
}
/* the last snapshot_buffer answer is a read-cache entry (it outlives the current batch) */
static int snapshot_last_cached;
static vk_buffer snapshot_buffer(uintptr_t address,uint64_t size) {
    snapshot_last_cached=0;
    // Cache only read-only snapshots in registered pools; dirty pages, pending copies and pool destruction invalidate them.
    // Keep this short-circuit order and these query counts; diagnostics never infer rejection from unexecuted later gates.
    int limit_ok,pool_ok=-1,tracked_ok=-1,pending=-1;
    int cache=(limit_ok=size<=(64u<<20))&&(pool_ok=mtl_addr(address,size,NULL,NULL))&&(tracked_ok=nvn_dirty_tracked((void*)address,size))&&!(pending=snapshot_pending(address,size));
    unsigned reason=!limit_ok?SNAP_DIAG_LIMIT:!pool_ok?SNAP_DIAG_POOL:!tracked_ok?SNAP_DIAG_UNTRACKED:pending>0?SNAP_DIAG_PENDING:SNAP_DIAG_NO_ENTRY;
    snapshot_diag_row *row=snapshot_diag_current;int old_entry=0;
    unsigned bucket=read_bucket(address,size);
    for(read_snapshot *entry=read_index[bucket];entry;entry=entry->hash_next)if(entry->address==address&&entry->buffer.size==size){
        int dirty=cache?nvn_dirty_since((void*)address,size,entry->stamp):0;old_entry=1;
        if(cache&&!dirty){if(row){row->reasons[SNAP_DIAG_HIT]++;row->old_entries++;row->old_stamp=entry->stamp;}entry->used=++read_cache_clock;read_lru_front(entry);read_cache_hits++;snapshot_last_cached=1;return entry->buffer;}
        row=snapshot_diag_miss_row(address,size);if(cache)reason=SNAP_DIAG_DIRTY;
        if(row){row->old_entries++;row->old_stamp=entry->stamp;}
        snapshot_diag_retired(entry,reason);read_cache_retire(entry);break;
    }
    // Protect before taking the generation/reading, avoiding untracked guest writes between a read and protection.
    row=snapshot_diag_miss_row(address,size);if(row){row->reasons[reason]++;if(!old_entry)snapshot_diag_absent(row);}
    read_cache_misses++;if(cache)nvn_dirty_protect((void*)address,size);uint64_t stamp=nvn_dirty_now();vk_buffer buffer=buffer_create(size);
    owned_snapshot_counters before;if(row){row->new_stamp=stamp;before=owned_snapshot_stats;}
    if(snapshot_read_owned(address,&buffer,size))die("Vulkan read snapshot failed");
    if(row){row->known_calls+=owned_snapshot_stats.known_calls-before.known_calls;row->safe_calls+=owned_snapshot_stats.safe_calls-before.safe_calls;row->safe_bytes+=owned_snapshot_stats.safe_requested_bytes-before.safe_requested_bytes;}
    if(!cache){post_new(3)->source=buffer;return buffer;}
    while(read_cache_bytes+buffer.capacity>(256u<<20)){snapshot_diag_retired(read_snapshot_tail,SNAP_DIAG_LRU);read_cache_retire(read_snapshot_tail);}
    read_snapshot *entry=calloc(1,sizeof *entry);if(!entry)die("Vulkan read snapshot allocation");entry->address=address;entry->stamp=stamp;entry->used=++read_cache_clock;entry->buffer=buffer;
    entry->hash_next=read_index[bucket];read_index[bucket]=entry;read_lru_front(entry);read_cache_bytes+=buffer.capacity;snapshot_last_cached=1;return buffer;
}
#include "nvn_vulkan_index.inc"

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    if (!mtl_addr(dst, n, NULL, NULL)) die("NVN copy destination outside pools: %#llx + %llu", (unsigned long long)dst, (unsigned long long)n);
    snapshot_prepare();
    forward_slice slice;
    if (!(n&3) && forward_find(src,n,4,NULL,&slice)==FORWARD_FULL && !slice.host_written) {
        // Copy an independent GPU-result version; guest writeback still follows real completion and earlier consumers retain their old backing.
        graphics_pass_end();forward_read_barrier(&slice,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);forward_hold(&slice);
        post_op *p=post_new(0);p->site=POST0_SITE_COPY_GPU;p->destination=(void*)(uintptr_t)dst;p->size=n;p->output=buffer_create(n);post_register_range(p);
        VkBufferCopy copy={.srcOffset=slice.offset,.size=n};vkCmdCopyBuffer(command,slice.buffer,p->output.buffer,1,&copy);
        VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->output.buffer,.size=n};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
        forward_mark_gpu_recorded(p,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,1);forward_copy_bytes+=n;copy_bytes+=n;return;
    }
    dependencies_prepare_range(src,n);
    post_op *p = post_new(0); p->site = POST0_SITE_COPY_CPU; p->size = n;
    p->source = buffer_create(n); p->output = p->source;
    if (snapshot_read(src, p->source.mapped, n)) die("NVN copy source unreadable: %#llx", (unsigned long long)src);
    p->destination = (void *)(uintptr_t)dst;
    post_register_range(p);
    // Retain known CPU input until GPU completion publishes it; avoid redundant temporary-buffer copies.
    copy_bytes += n;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t value) { post_op *p = post_new(1); p->event = val; p->mode = mode; p->value = value;post_register_range(p); }
/* TOTK conditional rendering (SetRenderEnableConditional): rendering is enabled when the NVNcounterData.counter values at
   addr and addr+16 are equal (mode 0) or differ (mode 1). TOTK gates most of its colour clears with it: the CPU writes
   counter 1 to request a clear and the timestamp report right after the clear writes 0, so each requested clear runs
   once. Treated as always enabled, every gated clear would run every frame and erase G-buffer and lighting inputs. The
   condition is evaluated at replay, in GPU order: counter reports recorded but not yet written are tracked here
   (a timestamp report will write 0; a sample count is unknown, and an unknown value renders, as the Metal backend). */
typedef struct { uint64_t addr, value; unsigned pending; int known; } cond_pending;
static cond_pending cond_table[1024]; static pthread_mutex_t cond_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cond_skipped, cond_unknown;
static cond_pending *cond_slot_locked(uint64_t addr, int create) {
    unsigned h = (unsigned)(addr >> 3) & 1023;
    for (unsigned i = 0; i < 1024; i++) { cond_pending *c = &cond_table[(h + i) & 1023];
        if (c->addr == addr) return c;
        if (!c->addr) { if (!create) return NULL; c->addr = addr; return c; } }
    return NULL;
}
static void cond_pending_add(uint64_t addr, int known, uint64_t value) {
    pthread_mutex_lock(&cond_lock); cond_pending *c = cond_slot_locked(addr, 1);
    if (c) { c->pending++; c->known = known; c->value = value; } pthread_mutex_unlock(&cond_lock);
}
static void cond_pending_done(uint64_t addr) {
    pthread_mutex_lock(&cond_lock); cond_pending *c = cond_slot_locked(addr, 0); if (c && c->pending) c->pending--; pthread_mutex_unlock(&cond_lock);
}
/* 1 known, 0 unknown */
static int cond_counter(uint64_t addr, uint64_t *value) {
    pthread_mutex_lock(&cond_lock); cond_pending *c = cond_slot_locked(addr, 0); int r = -1;
    if (c && c->pending) { r = c->known; *value = c->value; } pthread_mutex_unlock(&cond_lock);
    if (r >= 0) return r;
    if (!mtl_addr(addr, 8, NULL, NULL)) return 0;
    memcpy(value, (const void *)(uintptr_t)addr, 8); return 1;
}
static int cond_enabled(const p_render_cond *c) {
    uint64_t x = 0, y = 0; int kx = cond_counter(c->addr, &x), ky = cond_counter(c->addr + 16, &y);
    if (!kx || !ky) { cond_unknown++; return 1; }
    int enabled = c->mode == 0 ? x == y : x != y; if (!enabled) cond_skipped++;
    /* flip watch: an address whose outcome differs from its previous evaluation (cheap; no frame slowdown) */
    { static struct { uint64_t addr, frame; int result; } seen[512]; unsigned h = (unsigned)(c->addr >> 4) & 511;
      if (seen[h].addr == c->addr && seen[h].frame != presents && seen[h].result != enabled) {
          static uint64_t flips; flips++;
          if (flips <= 60 || !(flips % 500)) LOG("[vulkan-cond-flip] frame %llu addr %#llx mode %d x=%llu y=%llu -> %s (flip %llu)", (unsigned long long)presents, (unsigned long long)c->addr, c->mode,
              (unsigned long long)x, (unsigned long long)y, enabled ? "render" : "skip", (unsigned long long)flips); }
      seen[h].addr = c->addr; seen[h].frame = presents; seen[h].result = enabled; }
    return enabled;
}
/* A timestamp report's counter word (0) is written at replay, in command order: written at GPU completion it could land
   after the game's next clear request (counter 1 at the same address) and erase it, losing that clear (flicker). The
   timestamp word is still published at completion. */
/* Reports do not end the render pass (that would be ~48 pass breaks a frame in the cave): a samples report splits the
   occlusion query in place, a timestamp is written in the pass and converted at the next pass end (counter_gpu_record). */
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) samples_report();
    if (type != NVN_COUNTER_TYPE_SAMPLES_PASSED && !(addr & 7) && mtl_addr(addr, 8, NULL, NULL)) { dm_guest_write_prepare((void *)(uintptr_t)addr, 8); *(volatile uint64_t *)(uintptr_t)addr = 0; }
    else cond_pending_add(addr, 0, 0);post_op *p = post_new(2); p->destination = (void *)(uintptr_t)addr; p->value = type;p->size=16;post_register_range(p);counter_gpu_record(p); }
static void mo_pending_flush(void);
uint64_t mtl_sub_commit(void) {
    /* deferred clears never cross a batch (fences and write-backs are published per batch) */
    pclear_flush_all();
    if (!recording) die("NVN commit without begin");
    graphics_pass_end();
    /* object motion's listed draws, while their sets and buffers are this batch's */
    mo_pending_flush();
    if(graphics_host_writes){VkMemoryBarrier writes={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&writes,0,NULL,0,NULL);graphics_host_writes=0;}
    if(batch_times)vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,batch_times,1);
    async_record_query_readback_locked();
    VK_CHECK(vkEndCommandBuffer(command));
    uint64_t fence=async_commit_locked();pthread_mutex_unlock(&queue_lock);return fence;
}

uint64_t mtl_last_fence(void) { return atomic_load(&submitted); }
int mtl_wait(uint64_t fence, uint64_t timeout) {
    uint64_t wait_started=dm_android_clock_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&completion_lock); int ready = atomic_load(&completed) >= fence,blocked=!ready&&timeout;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); uint64_t seconds = timeout / 1000000000ull, ns = until.tv_nsec + timeout % 1000000000ull;
    if (timeout != UINT64_MAX) { until.tv_sec += seconds + ns / 1000000000ull; until.tv_nsec = ns % 1000000000ull; }
    while (!ready && timeout) { int e = timeout == UINT64_MAX ? pthread_cond_wait(&completion_condition, &completion_lock) : pthread_cond_timedwait(&completion_condition, &completion_lock, &until);
        ready = atomic_load(&completed) >= fence; if (e == ETIMEDOUT) break; if (e) die("NVN fence condition: %d", e); }
    pthread_mutex_unlock(&completion_lock);
    if(blocked)atomic_fetch_add(&fence_wait_unlocked_ns,dm_android_clock_ns(CLOCK_MONOTONIC)-wait_started);return ready;
}
void mtl_finish(void) { mtl_wait(mtl_last_fence(), UINT64_MAX); }
void mtl_drain(void) { mtl_init();mtl_finish(); pthread_mutex_lock(&queue_lock); VK_CHECK(vkQueueWaitIdle(queue)); pthread_mutex_unlock(&queue_lock);pipeline_disk_stop_join(); }

#include "nvn_vulkan_tex.inc"
#include "nvn_vulkan_spv_arena.inc"
#include "nvn_vulkan_shader.inc"
#include "nvn_vulkan_counter_gpu.inc"
#include "nvn_vulkan_upscale.inc"
#include "nvn_vulkan_framegen.inc"
#include "nvn_vulkan_motion.inc"
#include "nvn_vulkan_taa.inc"
#include "nvn_vulkan_asr.inc"
#include "nvn_vulkan_lsfg.inc"
#include "nvn_vulkan_smaa.inc"
#include "nvn_vulkan_record.inc"
#include "nvn_vulkan_latency.inc"
#include "overlay.h"
/* The overlay's live settings (overlay.h). The frame cap lives in the NVN front end (hle_nvn.c). */
int nvn_frame_cap_get(void) __attribute__((weak)); void nvn_frame_cap_set(int hz) __attribute__((weak));
void dm_android_quit(void) __attribute__((weak));
void dm_android_motion(int on) __attribute__((weak));
void dm_android_face_buttons(int by_label) __attribute__((weak));
void dm_android_controls(int8_t *actions, int set) __attribute__((weak));
void *dm_android_asset(const char *name, size_t *size) __attribute__((weak));
void dm_android_report(int start, const char *details, char *out, size_t size) __attribute__((weak));
void compat_audio_set_gain(float gain) __attribute__((weak));
static void ov_apply(const char *key, const char *value) {
    LOG("[overlay] %s=%s", key, value);
    if (!strcmp(key, "TOTK_UNCAPPED_HZ")) fg_cap_set(atoi(value));
    else if (!strcmp(key, "TOTK_UPSCALER")) { upscale_ready(); upscale.mode = upscale_mode_of(value); LOG("[upscale] present upscaler: %s", upscale_names[upscale.mode]); }
    else if (!strcmp(key, "TOTK_FSR_SHARPNESS")) { upscale_ready(); upscale.sharpness = upscale_sharpness(value); }
    else if (!strcmp(key, "TOTK_SCALEFX_RES")) { upscale_ready(); upscale.sfx_res = upscale_sfx_res(value); LOG("[upscale] ScaleFX output: %s", value); }
    else if (!strcmp(key, "TOTK_AA")) { smaa_on(); smaa.on = !strcmp(value, "smaa") || !strcmp(value, "smaat2x"); taa_aa_mode(!strcmp(value, "smaat2x")); fg_track(); }
    else if (!strcmp(key, "TOTK_SMAA_ULTRA")) smaa_ultra_on = atoi(value) > 0;
    /* 0, <rate> (built-in) or lsfg<rate>x<multiplier>: the game's rendered rate (fg_parse) */
    else if (!strcmp(key, "TOTK_FRAMEGEN")) {
        int use_lsfg, hz, mult; fg_parse(value, &use_lsfg, &hz, &mult);
        fg_enabled(); fg.on = !use_lsfg && hz > 0; fg.prev_ok = fg.last_ok = 0;
        lsfg_enabled(); lsfg.on = use_lsfg && hz > 0; fg_track(); fg.lsfg = lsfg.on;
        if (lsfg.on) { lsfg_configure(fg_adaptive ? LSFG_MAX_OUT + 1 : mult, -1, -1); lsfg.adaptive = fg_adaptive; }
        /* chosen but cannot run: the game renders every frame shown */
        int lsfg_off = lsfg.on && (!lsfg_device_ok || lsfg.failed);
        if (lsfg_off) LOG("[lsfg] not available on this device / driver (see [lsfg] at start)");
        fg_shown = hz > 0 ? hz * (use_lsfg ? mult : 2) : 0;
        if (hz > 0) fg_cap_set(lsfg_off ? fg_shown : hz);
    }
    else if (!strcmp(key, "TOTK_RECORD")) recorder_set(!strcmp(value, "on"));
    /* LSFG skips the frame where its inputs switch (lsfg.last_scene) */
    else if (!strcmp(key, "TOTK_FG_HUD")) { fg.hud_generated = !strcmp(value, "generated"); fg.scene_valid[0] = fg.scene_valid[1] = 0; }
    else if (!strcmp(key, "TOTK_LSFG_FLOW")) lsfg_configure(0, -1, (float)atof(value));
    else if (!strcmp(key, "TOTK_LSFG_PERF")) lsfg_configure(0, atoi(value), -1);
    else if (!strcmp(key, "TOTK_DISPLAY_HZ")) { setenv("TOTK_DISPLAY_HZ", value, 1); if (screen_window) screen_request_rate(screen_window); }
    else if (!strcmp(key, "TOTK_VOLUME")) { if (compat_audio_set_gain) compat_audio_set_gain((float)atoi(value) / 100.0f); }
    else if (!strcmp(key, "TOTK_MOTION")) { if (dm_android_motion) dm_android_motion(strcmp(value, "off") != 0); }
    else if (!strcmp(key, "TOTK_FACE_BUTTONS")) { if (dm_android_face_buttons) dm_android_face_buttons(!strcmp(value, "label")); }
    else if (!strcmp(key, "TOTK_ANISO")) { pthread_mutex_lock(&descriptor_lock); sampler_aniso = (float)atof(value); sampler_forget_all_locked(); pthread_mutex_unlock(&descriptor_lock); }
}
static PFN_vkVoidFunction ov_proc(const char *name, void *user) {
    (void)user; PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name);
    return f ? f : vk_api.vkGetInstanceProcAddr(instance, name);
}
static void overlay_setup(uint32_t family) {
    overlay_vk vk = {instance, physical, device, family, queue, ov_proc};
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(physical, &pp);
    /* the driver's own name and version (VK_KHR_driver_properties, core since 1.2): "turnip Mesa 26.3.0-devel (git-...)" */
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES}; VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(physical, &p2);
    static char driver[192]; { char first[64]; snprintf(first, sizeof first, "%s", drv.driverName); first[strcspn(first, " ")] = 0; snprintf(driver, sizeof driver, "%s %s", first[0] ? first : "driver", drv.driverInfo); }
    static char info[640];
    snprintf(info, sizeof info, "GPU: %s\nDriver: %s (Vulkan %u.%u.%u)\nDriver version: %s\nShader cache: %s\nGame: Tears of the Kingdom 1.4.3",
             pp.deviceName, getenv("DM_VK_DRIVER_NAME") ? "Turnip (Mesa)" : "Qualcomm (system)", VK_API_VERSION_MAJOR(pp.apiVersion),
             VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), driver, getenv("DM_VK_SHADERS") ? getenv("DM_VK_SHADERS") : "none");
    overlay_settings set = {ov_apply, dm_android_quit ? dm_android_quit : NULL, getenv("TOTK_FILES_DIR"), info,
                            dm_android_asset ? dm_android_asset : NULL, dm_android_controls ? dm_android_controls : NULL,
                            dm_android_report ? dm_android_report : NULL, driver};
    overlay_configure(&vk, &set);
}
#include "nvn_vulkan_batch_resources.inc"
#include "nvn_vulkan_probe.inc"
#include "nvn_vulkan_freeze.inc"

/* TOTK_VK_BARRIERSTAT=1: what the game's barriers inside our passes precede. A barrier followed by SetRenderTargets to
   other targets is the game's pass-boundary synchronisation (the one we re-derive from image hazards, 741 image barriers
   a frame); one followed by a draw orders a feedback read. bits are the NVN barrier flags (ORDER_PRIMITIVES 1,
   ORDER_FRAGMENTS 2, ORDER_FRAGMENTS_TILED 4, ORDER_INDIRECT_DATA 8, INVALIDATE_TEXTURE 0x10, _SHADER 0x20,
   _TEXTURE_DESCRIPTOR 0x40, _ZCULL 0x80, _L2_CACHE 0x100). */
static struct { int on, pending, bits; uint64_t set_rt_changed, set_rt_same, draw, dispatch, end, barrier, between; struct { int bits; uint64_t n; } hist[16]; } barstat = {.on = -1};
static int barstat_on(void) { if (barstat.on < 0) { const char *x = getenv("TOTK_VK_BARRIERSTAT"); barstat.on = x && atoi(x); } return barstat.on; }
static void barstat_note(int bits, int in_pass) {
    if (!barstat_on()) return;
    if (!in_pass) { barstat.between++; return; }
    if (barstat.pending) barstat.barrier++;
    barstat.pending = 1; barstat.bits = bits;
    for (int i = 0; i < 16; i++) { if (barstat.hist[i].n && barstat.hist[i].bits != bits) continue; barstat.hist[i].bits = bits; barstat.hist[i].n++; break; }
    static uint64_t last; if (presents >= last + 300) { last = presents;
        LOG("[vulkan-barrierstat] in-pass barriers followed by: SetRT other targets %llu, SetRT same %llu, draw %llu, dispatch %llu, recording end %llu, another barrier %llu; between passes %llu",
            (unsigned long long)barstat.set_rt_changed, (unsigned long long)barstat.set_rt_same, (unsigned long long)barstat.draw, (unsigned long long)barstat.dispatch, (unsigned long long)barstat.end, (unsigned long long)barstat.barrier, (unsigned long long)barstat.between);
        char line[512]; int k = 0; for (int i = 0; i < 16 && barstat.hist[i].n; i++) k += snprintf(line + k, sizeof line - k, " %#x:%llu", barstat.hist[i].bits, (unsigned long long)barstat.hist[i].n);
        LOG("[vulkan-barrierstat] in-pass barrier bits:%s", line);
        memset(&barstat.set_rt_changed, 0, (char *)&barstat.hist - (char *)&barstat.set_rt_changed); memset(barstat.hist, 0, sizeof barstat.hist); }
}
#define BARSTAT_FOLLOW(field) do { if (barstat.pending) { barstat.pending = 0; barstat.field++; } } while (0)
/* TOTK_VK_STATELEAK=1: state a draw depends on that was not set within the recording (command buffer) it is in, i.e.
   inherited from an earlier recording or from the caller of a CALL. A layer that translates each recording as it is
   made knows only what the recording itself set; this counts what it would miss. Reset at the start of every recording
   executed (top level and called), restored to the caller's at its end. */
static void stateleak_draw(unsigned depth) {
    if (!stateleak_on()) return;
    sl.draws++; if (depth) sl.called++;
    uint32_t need = RM_RT | RM_PROGRAM | RM_COLOR | RM_MASK | RM_MS | RM_POLY | RM_DEPTH | RM_VIEWPORT | RM_SCISSOR | RM_DEPTHRANGE | RM_TEXPOOL | RM_SAMPPOOL;
    if (state.color.blend_en) need |= RM_BLEND | RM_BLENDCOLOR;
    if (state.polygon.offset_en) need |= RM_POLYOFFSET;
    if (state.depth.st) need |= RM_STENCIL;
    uint32_t missing = need & ~rec_mask.set; int leak = missing != 0;
    for (unsigned b = 0; b < RM_COUNT_; b++) if (missing >> b & 1) sl.cat[b]++;
    if (state.program) for (unsigned i = 0; i < state.program->nstage && i < 6; i++) {
        shader_entry *sh = program_shader(state.program, i); unsigned st = sh->stage; if (st >= 6) continue;
        int lu = 0, ls = 0, lt = 0; char det[512]; int dk = 0; int detail = SL_DETAIL();
        for (unsigned k = 0; k < sh->count; k++) { const shader_binding *b = &sh->bindings[k]; const shader_binding_plan *plan = &sh->binding_plan[k];
            if (descriptor_type_uniform(plan->type)) { if (b->kind != 0 && b->slot >= 3 && b->slot < 19 && state.ubo[st][b->slot - 3].addr && !(rec_mask.ubo[st] >> (b->slot - 3) & 1)) { lu = 1; if (detail && dk < (int)sizeof det - 64) dk += snprintf(det + dk, sizeof det - dk, " ubo%d@%#llx+%llu", b->slot - 3, (unsigned long long)state.ubo[st][b->slot - 3].addr, (unsigned long long)state.ubo[st][b->slot - 3].size); } }
            else if (plan->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) { if (plan->cb_pointer < 0 && plan->storage_slot >= 0 && plan->storage_slot < 16 && !(plan->uniform_pointer ? rec_mask.ubo[st] >> plan->storage_slot & 1 : rec_mask.ssbo[st] >> plan->storage_slot & 1)) { ls = 1; const p_bind_buf *bb = plan->uniform_pointer ? &state.ubo[st][plan->storage_slot] : &state.ssbo[st][plan->storage_slot]; if (detail && dk < (int)sizeof det - 64) dk += snprintf(det + dk, sizeof det - dk, " %s%d@%#llx+%llu", plan->uniform_pointer ? "ubo(ptr)" : "ssbo", plan->storage_slot, (unsigned long long)bb->addr, (unsigned long long)bb->size); } }
            else if (plan->type != VK_DESCRIPTOR_TYPE_SAMPLER) for (unsigned j = 0; j < b->count; j++) { int slot = shader_texture_slot(b, j); if (slot >= 0 && slot < 128 && !(rec_mask.tex[st][slot >> 6] >> (slot & 63) & 1)) { lt = 1; if (detail && dk < (int)sizeof det - 64) dk += snprintf(det + dk, sizeof det - dk, " tex%d=%#llx", slot, (unsigned long long)state.textures[st][slot]); } }
        }
        if (st == 0) for (unsigned a = 0; a < 16; a++) if (state.attributes[a].fmt && sh->attributes[a]) {
            if (!(rec_mask.vattr >> a & 1)) { sl.vattr++; leak = 1; }
            unsigned str = state.attributes[a].stream; if (str < 16) { if (!(rec_mask.vstream >> str & 1)) { sl.vstream++; leak = 1; } if (!(rec_mask.vbuf >> str & 1)) { sl.vbuf++; leak = 1; } } }
        sl.ubo += lu; sl.ssbo += ls; sl.tex += lt; leak |= lu | ls | lt;
        if ((lu | ls | lt) && sl.piece < 32) sl.pleak[sl.piece]++;
        if ((lu | ls | lt) && detail) LOG("[stateleak-detail] f%llu piece #%d draw %llu stage %u shader %s inherits:%s", (unsigned long long)presents, sl.piece, (unsigned long long)(sl.piece < 32 ? sl.pdraws[sl.piece] : 0), st, sh->key, det);
    }
    if (sl.piece < 32) sl.pdraws[sl.piece]++;
    if (leak) { sl.leaking++; if (depth) sl.called_leaking++; }
    static uint64_t last; if (presents >= last + 300) { last = presents;
        static const char *names[RM_COUNT_] = {"rt", "program", "blend", "color", "mask", "multisample", "polygon", "depth", "viewport", "scissor", "depthrange", "texpool", "samppool", "blendcolor", "polyoffset", "stencil", "renderenable", "depthclamp", "rasterdiscard", "rendercond"};
        char line[1024]; int k = 0; for (unsigned b = 0; b < RM_COUNT_; b++) if (sl.cat[b]) k += snprintf(line + k, sizeof line - k, " %s %.1f", names[b], sl.cat[b] / 300.0);
        LOG("[vulkan-stateleak] a frame: %.0f draws in %.0f recordings, %.1f draws (%.1f%%) depend on state from another recording (%.1f of %.1f inside CALLs); per category:%s; per stage: ubo %.1f ssbo %.1f tex %.1f; vertex: attr %.1f stream %.1f buffer %.1f",
            sl.draws / 300.0, sl.recordings / 300.0, sl.leaking / 300.0, sl.draws ? 100.0 * sl.leaking / sl.draws : 0, sl.called_leaking / 300.0, sl.called / 300.0, line, sl.ubo / 300.0, sl.ssbo / 300.0, sl.tex / 300.0, sl.vattr / 300.0, sl.vstream / 300.0, sl.vbuf / 300.0);
        char ops[1024]; int kk = 0; for (unsigned o = 1; o < OP_COUNT_; o++) if (sl.main_ops[o]) kk += snprintf(ops + kk, sizeof ops - kk, " op%u:%.1f", o, sl.main_ops[o] / 300.0);
        LOG("[vulkan-stateleak] the submission's own ops a frame (outside copied pieces; draws %.1f of %.1f):%s", sl.main_draws / 300.0, (sl.main_draws + sl.piece_draws) / 300.0, ops);
        char pp[1024]; int pk = 0; for (int i = 0; i < 32 && pk < (int)sizeof pp - 40; i++) if (sl.pdraws[i] || sl.pleak[i]) pk += snprintf(pp + pk, sizeof pp - pk, " #%d:%.0f/%.1f", i, sl.pdraws[i] / 300.0, sl.pleak[i] / 300.0);
        LOG("[vulkan-stateleak] per piece ordinal: draws a frame / draws inheriting a slot:%s", pp);
        LOG("[vulkan-stateleak] dependencies a frame: GPU-output forwards from the same piece %.1f, another piece of this submission %.1f, an older submission %.1f; image reads after a write in the same piece %.1f, another piece %.1f, older %.1f",
            sl.fwd_same / 300.0, sl.fwd_cross / 300.0, sl.fwd_older / 300.0, sl.img_same / 300.0, sl.img_cross / 300.0, sl.img_older / 300.0);
        int pd = sl.piece_depth; memset(&sl, 0, sizeof sl); sl.piece_depth = pd; sl.detail_until = presents + 3; }
}
static void execute(const uint8_t *p, unsigned depth) {
    rec_masks rec_saved = rec_mask; if (stateleak_on()) { rec_mask = (rec_masks){0}; sl.recordings++; }
    if (depth > 32) die("NVN command call recursion");
    for (uint64_t count = 0; count < 10000000; ++count) {
        op_hdr h; memcpy(&h, p, sizeof h); const uint8_t *a = p + sizeof h;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("Malformed NVN packet op=%u size=%u", h.op, h.size);
        uint64_t v[3] = {0}; size_t payload = h.size - 8; memcpy(v, a, payload < sizeof v ? payload : sizeof v);
        if (stateleak_state > 0 && !sl.piece_depth && h.op < OP_COUNT_) { sl.main_ops[h.op]++; if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) sl.main_draws++; } else if (stateleak_state > 0 && (h.op == OP_DRAW || h.op == OP_DRAW_IDX)) sl.piece_draws++;
        /* who binds the slots the world piece inherits (vertex SSBO 3, vertex texture 8) */
        if (SL_DETAIL() && sl.piece_depth && (h.op == OP_BIND_SSBO || h.op == OP_BIND_TEX)) {
            if (h.op == OP_BIND_SSBO) { const p_bind_buf *b = (const void *)a; if (b->stage == 0 && b->index == 3) LOG("[stateleak-detail] f%llu piece #%d draw %llu bind-ssbo stage 0 slot 3 @%#llx+%llu", (unsigned long long)presents, sl.piece, (unsigned long long)(sl.piece < 32 ? sl.pdraws[sl.piece] : 0), (unsigned long long)b->addr, (unsigned long long)b->size); }
            else { const p_bind_handles *b = (const void *)a; if (b->stage == 0 && b->first <= 8 && b->first + b->n > 8) LOG("[stateleak-detail] f%llu piece #%d draw %llu bind-tex stage 0 slot 8 (first %d n %d)", (unsigned long long)presents, sl.piece, (unsigned long long)(sl.piece < 32 ? sl.pdraws[sl.piece] : 0), b->first, b->n); }
        }
        /* the root's own ops in order, piece markers, and every uniform update */
        if (SL_DETAIL() && (!sl.piece_depth || h.op == OP_UPDATE_UB || h.op == OP_BEGIN)) {
            if (h.op == OP_BIND_UBO || h.op == OP_BIND_SSBO) { const p_bind_buf *b = (const void *)a; LOG("[stateleak-detail] f%llu depth %d piece #%d op%u %s stage %d slot %d @%#llx+%llu", (unsigned long long)presents, sl.piece_depth, sl.piece, h.op, h.op == OP_BIND_UBO ? "bind-ubo" : "bind-ssbo", b->stage, b->index, (unsigned long long)b->addr, (unsigned long long)b->size); }
            else if (h.op == OP_UPDATE_UB) LOG("[stateleak-detail] f%llu depth %d piece #%d op%u update-ub dst %#llx+%llu", (unsigned long long)presents, sl.piece_depth, sl.piece, h.op, (unsigned long long)v[0], (unsigned long long)v[1]);
            else if (h.op == OP_BEGIN && h.size >= 16) { if (!sl.piece_depth || v[0] == 2) LOG("[stateleak-detail] f%llu piece #%d %s (draws so far %llu)", (unsigned long long)presents, sl.piece + (v[0] == 1 && !sl.piece_depth), v[0] == 1 ? "begins" : "ends", (unsigned long long)(sl.piece < 32 ? sl.pdraws[sl.piece] : 0)); }
            else LOG("[stateleak-detail] f%llu root op%u size %u v %#llx %#llx %#llx", (unsigned long long)presents, h.op, h.size, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2]);
        }
        switch (h.op) {
            case OP_END: BARSTAT_FOLLOW(end); rec_mask = rec_saved; return;
            /* a piece marker (inline_recording): a copied recording begins (1: with its copy and serial) / ends (2) */
            case OP_BEGIN:
                if (h.size >= 32 && v[0] == 1) plan_piece_enter(v[1], v[2]); else if (h.size >= 16 && v[0] == 2) plan_piece_leave();
                if (stateleak_on() && h.size >= 16) { if (v[0] == 1) { rec_mask = (rec_masks){0}; sl.recordings++; sl.piece_depth++; if (sl.piece_depth == 1) sl.piece++; } else if (v[0] == 2 && sl.piece_depth) sl.piece_depth--; } break;
            case OP_JUMP: p = (const uint8_t *)(uintptr_t)v[0]; continue;
            case OP_CALL: execute((const uint8_t *)(uintptr_t)v[0], depth + 1); break;
            case OP_SIGNAL_EVENT: { uint32_t mode, value; memcpy(&mode, a + 8, 4); memcpy(&value, a + 12, 4); mtl_sub_event((void *)(uintptr_t)v[0], mode, value); break; }
            case OP_REPORT_COUNTER: mtl_sub_counter(v[1], v[0]); break;
            case OP_COPY_B2B: mtl_sub_copy(v[0], v[1], v[2]); break;
            case OP_UPDATE_UB: if (v[1] > payload - 16) die("Short NVN uniform packet"); inplace_updates++; if (graphics_active_frame) inplace_updates_inpass++; mtl_sub_copy((uintptr_t)(a + 16), v[0], v[1]); break;
            /* TOTK issues ~40 nvnCommandBufferBarrier calls per frame, mostly inside render passes. Ending the pass for each
               would store and reload every attachment (the G-buffer's five full-screen targets dozens of times a frame).
               Inside a pass they are not needed: image hazards (sampling a target, feedback reads) end the pass through
               image_access, buffer hazards are ordered by the forwarding barriers. Between passes the barrier stays. */
            case OP_BARRIER: { static uint64_t kept, skipped; game_sync_epoch++; sc.barriers++; barstat_note((int)v[0], graphics_active_frame != NULL); if (graphics_active_frame && !live_range("inpassbarrier", draw_count - live_frame_base)) { skipped++; ssbo_barrier_epoch++; if (!((kept + skipped) % 20000)) LOG("[vulkan] barriers: %llu inside passes (skipped), %llu between passes", (unsigned long long)skipped, (unsigned long long)kept); break; } kept++;
                graphics_pass_end();VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT}; vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL); break; }
            case OP_SET_TEX_POOL: rec_mask.set |= RM_TEXPOOL; nvn_state_apply(&state, h.op, a); break;
            case OP_SET_SAMP_POOL: rec_mask.set |= RM_SAMPPOOL; nvn_state_apply(&state, h.op, a); break;
            case OP_BIND_PROGRAM: rec_mask.set |= RM_PROGRAM; nvn_state_apply(&state, h.op, a); break;
            case OP_SET_RT: { rec_mask.set |= RM_RT; const p_set_rt *r = (const p_set_rt *)a;
                if (barstat.pending) { if (r->n == state.rt.n && r->depth == state.rt.depth && !memcmp(r->color, state.rt.color, sizeof r->color)) BARSTAT_FOLLOW(set_rt_same); else BARSTAT_FOLLOW(set_rt_changed); }
                nvn_state_apply(&state, h.op, a); break; }
            case OP_CLEAR_COLOR: if (!state.render_disabled) clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: if (!state.render_disabled) clear_depth((const p_clear_ds *)a); break;
            case OP_DEPTH_CLAMP: rec_mask.set |= RM_DEPTHCLAMP; nvn_state_apply(&state, h.op, a); break;
            case OP_RENDER_ENABLE: rec_mask.set |= RM_RENDERENABLE; nvn_state_apply(&state, h.op, a); break;
            case OP_RASTER_DISCARD: rec_mask.set |= RM_RASTERDISCARD; nvn_state_apply(&state, h.op, a); break;
            case OP_RENDER_COND: rec_mask.set |= RM_RENDERCOND; state.render_disabled = state.render_cond_off = !cond_enabled((const p_render_cond *)a);
                { static uint64_t said; if (!(said++ % 600)) LOG("[vulkan-totk] conditional rendering: %llu evaluations, %llu skipped, %llu unknown (rendered)", (unsigned long long)said, (unsigned long long)cond_skipped, (unsigned long long)cond_unknown); } break;
            case OP_COPY_B2T: copy_buffer_texture((const p_copy_b2t *)a); break;
            case OP_COPY_T2B: copy_texture_buffer((const p_copy_b2t *)a); break;
            case OP_CLEAR_TEX: clear_texture((const p_clear_tex *)a); break;
            case OP_COPY_T2T: copy_texture_texture((const p_copy_t2t *)a); break;
            case OP_SCISSOR: rec_mask.set |= RM_SCISSOR; nvn_state_apply(&state, h.op, a); break;
            case OP_VIEWPORTS: rec_mask.set |= RM_VIEWPORT; nvn_state_apply(&state, h.op, a); break;
            case OP_DEPTH_RANGE: rec_mask.set |= RM_DEPTHRANGE; nvn_state_apply(&state, h.op, a); break;
            case OP_BIND_STATE: { const p_bind_state *b = (const void *)a;
                { static const uint32_t bits[6] = {RM_BLEND, RM_COLOR, RM_MASK, RM_MS, RM_POLY, RM_DEPTH}; if (b->kind >= 0 && b->kind < 6) rec_mask.set |= bits[b->kind]; }
                nvn_state_apply(&state, h.op, a); break; }
            case OP_BIND_UBO: case OP_BIND_SSBO: { const p_bind_buf *b = (const void *)a; if (b->stage < 0 || b->stage >= 6 || b->index < 0 || b->index >= 16) die("NVN buffer binding bounds"); if (h.op == OP_BIND_UBO) rec_mask.ubo[b->stage] |= (uint16_t)(1u << b->index); else rec_mask.ssbo[b->stage] |= (uint16_t)(1u << b->index);
                p_bind_buf *dst=h.op==OP_BIND_UBO?&state.ubo[b->stage][b->index]:&state.ssbo[b->stage][b->index];
                // c0 depends only on bound addresses/sizes; compute also includes the UBO table.
                if((h.op==OP_BIND_SSBO||b->stage==5)&&(dst->addr!=b->addr||dst->size!=b->size))uniform_drivers[b->stage][0].state_valid=0;
                nvn_state_apply(&state, h.op, a); break; }
            case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { const p_bind_handles *b = (const void *)a; int max = h.op == OP_BIND_IMG ? 32 : 128; if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->first + b->n > max) die("NVN handle binding bounds"); if (h.op == OP_BIND_TEX) for (int i = b->first; i < b->first + b->n; i++) rec_mask.tex[b->stage][i >> 6] |= 1ull << (i & 63); uint64_t *dst = h.op == OP_BIND_IMG ? state.images[b->stage] : h.op == OP_BIND_TEX ? state.textures[b->stage] : state.samplers[b->stage];
                // Driver tables encode low 32 bits and only eight image slots; resource binding still uses full state.
                int limit=h.op==OP_BIND_IMG?8:128;
                if(uniform_drivers[b->stage][1].state_valid)for(int i=0;i<b->n&&b->first+i<limit;i++)if((uint32_t)dst[b->first+i]!=(uint32_t)b->h[i]){uniform_drivers[b->stage][1].state_valid=0;break;}
                nvn_state_apply(&state, h.op, a); break; }
            case OP_BIND_VATTR: { const p_bind_vattr *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex attribute bounds"); for (int i = b->first; i < b->first + b->n; i++) rec_mask.vattr |= (uint16_t)(1u << i); nvn_state_apply(&state, h.op, a); break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex stream bounds"); for (int i = b->first; i < b->first + b->n; i++) rec_mask.vstream |= (uint16_t)(1u << i); nvn_state_apply(&state, h.op, a); break; }
            case OP_BIND_VBUF: { const p_bind_vbuf *b = (const void *)a; if (b->first < 0 || b->n < 0 || b->first + b->n > 16) die("NVN vertex buffer bounds"); for (int i = b->first; i < b->first + b->n; i++) rec_mask.vbuf |= (uint16_t)(1u << i); nvn_state_apply(&state, h.op, a); break; }
            case OP_COPY_STRIDE: nvn_state_apply(&state, h.op, a); break;
            // The Vulkan buffer<->texture copies do not read the image stride (nvnCommandBufferSetCopyImageStride):
            // nvn_copy_pitch.h's semantics are not ported.
            case OP_BLEND_COLOR: rec_mask.set |= RM_BLENDCOLOR; nvn_state_apply(&state, h.op, a); break;
            case OP_POLY_OFFSET: rec_mask.set |= RM_POLYOFFSET; nvn_state_apply(&state, h.op, a); break;
            case OP_STENCIL: rec_mask.set |= RM_STENCIL; nvn_state_apply(&state, h.op, a); break;
            case OP_RESET_COUNTER: if (v[0] == NVN_COUNTER_TYPE_SAMPLES_PASSED) {samples_reset();post_new(4);} break;
            case OP_DISPATCH: BARSTAT_FOLLOW(dispatch); dispatch_compute((const p_dispatch *)a); break;
            case OP_DRAW: { const draw_plan *pl = plan_next_draw(); BARSTAT_FOLLOW(draw); stateleak_draw(depth); drawsig_note((const p_draw *)a, NULL); draw_graphics((const p_draw *)a, NULL, pl); break; }
            case OP_DRAW_IDX: { const draw_plan *pl = plan_next_draw(); BARSTAT_FOLLOW(draw); stateleak_draw(depth); drawsig_note(NULL, (const p_draw_idx *)a); draw_graphics(NULL, (const p_draw_idx *)a, pl); break; }
            default: die("Vulkan NVN packet not implemented: op=%u size=%u (draw=%u indexed=%u dispatch=%u)", h.op, h.size, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH);
        }
        p += h.size;
    }
    die("NVN command traversal exceeds limit");
}
void mtl_exec(const uint8_t *handle) { execute(handle, 0); }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_index(const uint64_t *handles, int n) { (void)handles; (void)n; }

void mtl_prog_release(prog_t *p) {pthread_mutex_lock(&queue_lock);program_remove(p);pthread_mutex_unlock(&queue_lock);}
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
typedef struct frame_stability_sample { uint64_t frame,ns[8]; } frame_stability_sample;
static _Atomic uint64_t present_texture_wait_calls,present_texture_wait_blocks,present_texture_wait_ns;
static int stability_compare(const void *a,const void *b) {
    uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);
}
static void frame_stability_record(uint64_t frame,uint64_t prev_acquire,uint64_t acquire,uint64_t present,uint64_t present_lock) {
    // Sample only present boundaries; fixed 120-frame windows retain worst-frame attribution without changing waits or timing draws.
    static frame_stability_sample samples[120];static unsigned count;
    static uint64_t last_wall,last_cpu,last_ring,last_worker;static pthread_t thread;
    uint64_t ring,worker;pthread_mutex_lock(&queue_lock);ring=stability_ring_wait_ns;worker=stability_worker_publish_ns;pthread_mutex_unlock(&queue_lock);
    uint64_t wall=dm_android_clock_ns(CLOCK_MONOTONIC),cpu=dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID);pthread_t current=pthread_self();
    if(last_wall&&pthread_equal(thread,current)&&wall>=last_wall&&cpu>=last_cpu){
        samples[count++]=(frame_stability_sample){frame,{wall-last_wall,cpu-last_cpu,prev_acquire,acquire,present,present_lock,ring-last_ring,worker-last_worker}};
    }else count=0;
    thread=current;last_wall=wall;last_cpu=cpu;last_ring=ring;last_worker=worker;
    if(count!=120)return;
    static const char *names[]={"interval","thread_cpu","prev_acquire","acquire","present","present_lock","ring","worker"};
    uint64_t ordered[120];unsigned over33=0,over50=0,worst=0;
    for(unsigned i=0;i<count;i++){over33+=samples[i].ns[0]>33333333;over50+=samples[i].ns[0]>50000000;if(samples[i].ns[0]>samples[worst].ns[0])worst=i;}
    char summary[1536];size_t used=0;
    for(unsigned field=0;field<8;field++){
        uint64_t sum=0;for(unsigned i=0;i<count;i++){ordered[i]=samples[i].ns[field];sum+=ordered[i];}qsort(ordered,count,sizeof *ordered,stability_compare);
        int written=snprintf(summary+used,sizeof summary-used," %s=%.3f/%.3f/%.3f/%.3f",names[field],sum/(count*1e6),ordered[59]/1e6,ordered[113]/1e6,ordered[119]/1e6);
        if(written<0||(size_t)written>=sizeof summary-used)break;used+=(size_t)written;
    }
    LOG("[vulkan-frame-stability] frame=%llu samples=%u over33ms=%u over50ms=%u mean/p50/p95/max_ms:%s",(unsigned long long)frame,count,over33,over50,summary);
    const frame_stability_sample *s=&samples[worst];
    LOG("[vulkan-frame-worst] frame=%llu interval=%.3f thread_cpu=%.3f prev_acquire=%.3f acquire=%.3f present=%.3f present_lock=%.3f ring=%.3f worker=%.3f ms boundary=present_return_not_display worker_overlaps=1 ring_subset_of_fence_wait=1",(unsigned long long)s->frame,s->ns[0]/1e6,s->ns[1]/1e6,s->ns[2]/1e6,s->ns[3]/1e6,s->ns[4]/1e6,s->ns[5]/1e6,s->ns[6]/1e6,s->ns[7]/1e6);
    count=0;
}
/* RenderDoc frame capture (APK built with TOTK_RENDERDOC=1, layer enabled with tools/gpu_debug.sh renderdoc): when
   <files>/rdc.trigger appears (its content: number of frames, default 1) the next frames are captured to <files>/rdc/. */
#include "renderdoc_app.h"
/* the game's submit calls as the translation thread takes them: the gap between calls (the game's own pace) and the
   wait in the queue (ours); the frame profile prints both */
static uint64_t job_ns, job_ns_prev, job_taken_ns;
void mtl_sub_job_time(uint64_t ns) { job_ns_prev = job_ns; job_ns = ns; job_taken_ns = dm_android_clock_ns(CLOCK_MONOTONIC); }
/* The hitch log, always on: a frame much longer than its neighbours (over twice the running usual, and over 20 ms)
   is logged with what happened in it, to the log ("[hitch]") and to <files>/profile/hitches.log, which keeps every
   session (a timestamp and the frame first). The fields are the frame profile's: pipelines created on the translation
   thread in the frame and their time (a compile hitch), pipelines queued for the pipeline thread and draws skipped
   for one not ready, shaders translated at run time, images created, texture uploads (count, MB, ms), dependency
   waits, the frame's GPU time, the game's own gap between submits, the queue wait, snapshot MB. */
static void hitch_log(double interval_ms, uint64_t pipes, uint64_t sync_count, double sync_ms, uint64_t skipped, uint64_t translated, uint64_t images, uint64_t uploads, double upload_mb, double upload_ms, uint64_t depc, double dep_ms, double gpu_ms, double gap_ms, double qwait_ms, double snap_mb) {
    static double usual; static FILE *f; static int opened; static uint64_t hitches;
    /* usual = the median of the last 64 frames, every frame counted: an average that skips hitches sticks at the faster
       rate when the game slows down for good (a 120 fps stretch, then 40 fps with frame generation, logs every frame) */
    enum { RECENT = 64 }; static double recent[RECENT]; static unsigned nrecent, next;
    if (interval_ms <= 0) return;
    int hitch = nrecent >= 16 && interval_ms > 2.0 * usual && interval_ms > 20.0;
    recent[next] = interval_ms; next = (next + 1) % RECENT; if (nrecent < RECENT) nrecent++;
    { double sorted[RECENT]; memcpy(sorted, recent, nrecent * sizeof *sorted);
      for (unsigned i = 1; i < nrecent; i++) { double v = sorted[i]; unsigned j = i; while (j && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; } sorted[j] = v; }
      usual = sorted[nrecent / 2]; }
    if (!hitch) return;
    uint64_t q = atomic_load(&async_pso_queued), done = atomic_load(&async_pso_built) + atomic_load(&async_pso_failed);
    char line[512];
    snprintf(line, sizeof line, "frame=%llu t=%llu %.1f ms (usual %.1f): pipelines %llu created, %llu on this thread in %.1f ms, %llu compiling, %llu draws skipped; shaders translated %llu; images %llu; uploads %llu (%.2f MB, %.1f ms); dependency waits %llu/%.1f ms; GPU %.1f ms; game gap %.1f ms; queue wait %.1f ms; snapshot %.2f MB",
        (unsigned long long)presents, (unsigned long long)dm_android_clock_ns(CLOCK_MONOTONIC), interval_ms, usual, (unsigned long long)pipes, (unsigned long long)sync_count, sync_ms, (unsigned long long)(q > done ? q - done : 0), (unsigned long long)skipped,
        (unsigned long long)translated, (unsigned long long)images, (unsigned long long)uploads, upload_mb, upload_ms, (unsigned long long)depc, dep_ms, gpu_ms, gap_ms, qwait_ms, snap_mb);
    LOG("[hitch] %s", line); hitches++;
    if (!opened) { opened = 1; const char *dir = getenv("TOTK_FILES_DIR"); char path[600]; if (dir) { snprintf(path, sizeof path, "%s/profile/hitches.log", dir); f = fopen(path, "a"); }
        if (f) { time_t now = time(NULL); char when[64]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now)); fprintf(f, "# session %s\n", when); } }
    if (f) { time_t now = time(NULL); char when[32]; strftime(when, sizeof when, "%H:%M:%S", localtime(&now)); fprintf(f, "%s %s\n", when, line); if (hitches % 8 == 0) fflush(f); }
}
/* TOTK_LOAD_CAP=<hz> (default 0 = off): a loading screen -- 20 frames in a row with fewer than 50 draws -- runs at this
   frame cap; the cap in force comes back with the first frame that draws again. Loading is bound by the three virtual
   cores (threads.c): every frame the loading screen renders takes core time from the loaders (cold starts of the bench
   save: loading screen 10.3 s at the default cap, 12.5 s uncapped). */
static void idle_keepers(int on);
/* threads.c: one more spinning waiter while loading */
void runtime_sync_spin_boost(int on) __attribute__((weak));
static void load_cap(uint64_t draws) {
    static int cap = -1, low = 0, applied = 0, saved = 0, loading = 0;
    if (cap < 0) { const char *e = getenv("TOTK_LOAD_CAP"); cap = e ? atoi(e) : 0; }
    if (draws < 50) { if (++low >= 20 && !loading) { loading = 1; LOG("[load] loading screen: waiters spin until it ends"); idle_keepers(1); if (runtime_sync_spin_boost) runtime_sync_spin_boost(1); if (cap) { saved = nvn_frame_cap_get(); nvn_frame_cap_set(cap); applied = 1; } } }
    else { low = 0; if (loading) { loading = 0; idle_keepers(0); if (runtime_sync_spin_boost) runtime_sync_spin_boost(0); if (applied) { nvn_frame_cap_set(saved); applied = 0; } } }
}
/* TOTK_IDLE_KEEPERS=<n> (default 0 = off): while a loading screen shows (the load_cap detector), n threads at SCHED_IDLE,
   one per host core from cpu 0, spin so the cores never enter their deep idle state (cpuidle exit latency on the Odin:
   222 us on cores 0-5, 302 us on the prime cores). Loading hands virtual cores between the game's threads thousands of
   times a second (6,000-11,000 waits a thread a second, ~150 us each, with 1-1.7 of the 3 cores
   idle on average): a thread woken onto a sleeping core pays that exit latency every time. A SCHED_IDLE spinner is
   preempted by any normal thread at once and only uses time nobody else wants; it also keeps the clocks up. */
#include <sched.h>
static _Atomic int idle_keepers_active; static pthread_mutex_t idle_keepers_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t idle_keepers_cv = PTHREAD_COND_INITIALIZER;
static void *idle_keeper_main(void *arg) {
    int cpu = (int)(intptr_t)arg; cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); sched_setaffinity(0, sizeof set, &set);
    struct sched_param sp = {0}; sched_setscheduler(0, SCHED_IDLE, &sp);
    for (;;) {
        pthread_mutex_lock(&idle_keepers_mu); while (!atomic_load(&idle_keepers_active)) pthread_cond_wait(&idle_keepers_cv, &idle_keepers_mu); pthread_mutex_unlock(&idle_keepers_mu);
        while (atomic_load_explicit(&idle_keepers_active, memory_order_relaxed)) for (int i = 0; i < 256; i++) __asm__ volatile("yield");
    }
    return NULL;
}
static void idle_keepers(int on) {
    static int n = -1; static int started;
    if (n < 0) { const char *e = getenv("TOTK_IDLE_KEEPERS"); n = e ? atoi(e) : 0; if (n > 16) n = 16; }
    if (n <= 0 || atomic_load(&idle_keepers_active) == on) return;
    if (!started) { started = 1; for (int i = 0; i < n; i++) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, idle_keeper_main, (void *)(intptr_t)i); pthread_attr_destroy(&a); } }
    pthread_mutex_lock(&idle_keepers_mu); atomic_store(&idle_keepers_active, on); pthread_cond_broadcast(&idle_keepers_cv); pthread_mutex_unlock(&idle_keepers_mu);
    LOG("[load] idle keepers %s (%d cores)", on ? "on" : "off", n);
}
/* TOTK_VK_FRAMEPROF=1: one line per presented frame: translation-thread CPU, draws, submits, dependency waits (calls / ms),
   snapshot MB, pipeline / image creations, texture uploads (find periodic slow frames) */
static void frame_profile(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_FRAMEPROF"); on = e && atoi(e); }
    static uint64_t cpu0, draws0, sub0, depc0, depw0, snap0, pipe0, img0, up0, wall0, sync0, syncn0, skip0, xl0;
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); uint64_t cpu = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    uint64_t wall = dm_android_clock_ns(CLOCK_MONOTONIC), depc = 0, depw = 0;
    for (unsigned i = 1; i < DEP_COUNT; i++) { depc += dependency_times[i].calls; depw += dependency_times[i].wait_ns + dependency_times[i].commit_ns; }
    uint64_t sub = atomic_load(&submitted);
    static double gpu0; static uint64_t upb0, upns0;
    if (wall0) hitch_log((wall - wall0) / 1e6, pipeline_creations - pipe0, pipeline_sync_count - syncn0, (pipeline_sync_ns - sync0) / 1e6, atomic_load(&async_pso_skipped) - skip0, totk_translated - xl0,
        image_creations - img0, texture_uploads - up0, (texture_upload_bytes - upb0) / 1048576.0, (texture_upload_ns - upns0) / 1e6, depc - depc0, (depw - depw0) / 1e6, (gpu_total_ns - gpu0) / 1e6,
        job_ns_prev ? (job_ns - job_ns_prev) / 1e6 : 0.0, job_ns ? (job_taken_ns - job_ns) / 1e6 : 0.0, (snapshot_bytes - snap0) / 1048576.0);
    sync0 = pipeline_sync_ns; syncn0 = pipeline_sync_count; skip0 = atomic_load(&async_pso_skipped); xl0 = totk_translated;
    load_cap(draw_count - draws0);
    /* gap: between the game's submit calls; qwait: the call's wait for this thread; gpu: batch GPU time completed since the
           last line; upload: texture bytes copied in and the thread's time doing it */
    if (on && wall0) LOG("[frameprof] frame=%llu wall=%.2f cpu=%.2f ms draws=%llu submits=%llu dep=%llu/%.2f ms snap=%.2f MB pipelines=%llu images=%llu uploads=%llu gap=%.2f qwait=%.2f gpu=%.2f upload_mb=%.2f upload_ms=%.2f",
        (unsigned long long)presents, (wall - wall0) / 1e6, (cpu - cpu0) / 1e6, (unsigned long long)(draw_count - draws0), (unsigned long long)(sub - sub0),
        (unsigned long long)(depc - depc0), (depw - depw0) / 1e6, (snapshot_bytes - snap0) / 1048576.0, (unsigned long long)(pipeline_creations - pipe0),
        (unsigned long long)(image_creations - img0), (unsigned long long)(texture_uploads - up0),
        job_ns_prev ? (job_ns - job_ns_prev) / 1e6 : 0.0, job_ns ? (job_taken_ns - job_ns) / 1e6 : 0.0, (gpu_total_ns - gpu0) / 1e6, (texture_upload_bytes - upb0) / 1048576.0, (texture_upload_ns - upns0) / 1e6);
    gpu0 = gpu_total_ns; upb0 = texture_upload_bytes; upns0 = texture_upload_ns;
    if (on) {
    if (snapshot_bytes - snap0 > (12u << 20)) for (int i = 0; i < 32 && snap_callers[i].caller; i++) if (snap_callers[i].bytes >= (256u << 10)) {
        Dl_info di; uintptr_t off = dladdr(snap_callers[i].caller, &di) && di.dli_fbase ? (uintptr_t)snap_callers[i].caller - (uintptr_t)di.dli_fbase : 0;
        LOG("[frameprof]   caller libtotk+%#lx: %.2f MB in %llu reads", (unsigned long)off, snap_callers[i].bytes / 1048576.0, (unsigned long long)snap_callers[i].calls); }
    memset(snap_callers, 0, sizeof snap_callers);
    if (dm_cpu_record_phases > 0) { static uint64_t prev[DM_CPU_PHASE_COUNT]; char line[512]; size_t n = 0;
        for (unsigned ph = 0; ph < DM_CPU_PHASE_COUNT && n < sizeof line - 40; ph++) { double ms = (dm_cpu_phase_ns[ph] - prev[ph]) / 1e6; prev[ph] = dm_cpu_phase_ns[ph];
            if (ms >= 0.2) n += (size_t)snprintf(line + n, sizeof line - n, " %s=%.1f", dm_cpu_phase_names[ph], ms); }
        line[n] = 0; LOG("[frameprof]   phases%s", line); }
    { static uint64_t prev[7]; double d[7], sum = 0; for (int i = 0; i < 7; i++) { d[i] = (draw_seg_ns[i] - prev[i]) / 1e6; prev[i] = draw_seg_ns[i]; sum += d[i]; }
      if (sum > 15) LOG("[frameprof]   draw wall: pre %.1f bind %.1f targets %.1f pipeline %.1f wait %.1f pass %.1f record %.1f", d[0], d[1], d[2], d[3], d[4], d[5], d[6]); }
    { static uint64_t a0, ab0, v0, vn0, f0, fn0, b0; uint64_t b = buffer_create_wall_ns;
      if (arrival_alloc_ns - a0 + arrival_view_ns - v0 + arrival_fb_ns - f0 + b - b0 > 2000000)
          LOG("[frameprof]   new objects: image memory %.1f ms (%.1f MB), views %.1f ms (%llu), framebuffers %.1f ms (%llu), buffers %.1f ms", (arrival_alloc_ns - a0) / 1e6, (arrival_alloc_bytes - ab0) / 1048576.0,
              (arrival_view_ns - v0) / 1e6, (unsigned long long)(arrival_views - vn0), (arrival_fb_ns - f0) / 1e6, (unsigned long long)(arrival_fbs - fn0), (b - b0) / 1e6);
      a0 = arrival_alloc_ns; ab0 = arrival_alloc_bytes; v0 = arrival_view_ns; vn0 = arrival_views; f0 = arrival_fb_ns; fn0 = arrival_fbs; b0 = b; }
    }
    cpu0 = cpu; draws0 = draw_count; sub0 = sub; depc0 = depc; depw0 = depw; snap0 = snapshot_bytes; pipe0 = pipeline_creations; img0 = image_creations; up0 = texture_uploads; wall0 = wall;
}
static void renderdoc_poll(void) {
    /* 0 unknown, 1 available, -1 absent */
    static RENDERDOC_API_1_6_0 *api; static int state;
    if (state < 0) return;
    if (!state) {
        void *layer = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW | RTLD_NOLOAD);
        pRENDERDOC_GetAPI get = layer ? (pRENDERDOC_GetAPI)dlsym(layer, "RENDERDOC_GetAPI") : NULL;
        if (!get || !get(eRENDERDOC_API_Version_1_6_0, (void **)&api) || !api) { state = -1; return; }
        const char *dir = getenv("DM_VK_SHADERS"); char path[1100];
        if (dir) { snprintf(path, sizeof path, "%s/../rdc", dir); mkdir(path, 0700); snprintf(path, sizeof path, "%s/../rdc/totk", dir); api->SetCaptureFilePathTemplate(path); }
        api->MaskOverlayBits(0, 0); state = 1; LOG("[renderdoc] capture API available; touch files/rdc.trigger to capture");
    }
    if (presents % 15) return;
    const char *dir = getenv("DM_VK_SHADERS"); char path[1100];
    if (!dir || snprintf(path, sizeof path, "%s/../rdc.trigger", dir) >= (int)sizeof path) return;
    FILE *f = fopen(path, "r"); if (!f) return;
    unsigned n = 1; if (fscanf(f, "%u", &n) != 1 || !n || n > 16) n = 1; fclose(f); unlink(path);
    api->TriggerMultiFrameCapture(n); LOG("[renderdoc] capturing %u frames from frame %llu", n, (unsigned long long)presents);
}
/* the game's frame rate */
static float present_fps, present_ms;
/* pictures shown per game frame (generated ones too), smoothed: the HUD counts them all */
static float present_shown = 1;
/* source: present this image (the whole of it) instead of t's picture, without counting a game frame (frame generation) */
static uint64_t present_one(const tex_t *t, double period, const int crop[4], image_entry *source, int repeat) {
    /* not a game frame: no counters, hooks or statistics */
    int inner = source || repeat;
    uint64_t acquire_ns=0,present_ns=0,present_lock_ns=0;
    uint64_t present_total=dm_cpu_phase_begin_always();
    // Present queues display work; real sync/finish and texture-reuse waits retain CPU-visible completion.
    // Commit only a recording this thread itself has open: while another thread (e.g. another game thread's ReadTexels) records it holds
    // queue_lock, and the mtl_sub_begin / queue_lock below queue behind it; never commit it on its behalf (that would end and submit someone
    // else's command buffer and unlock their mutex without holding it).
    if(async_recording_owned){if(!recording)die("Vulkan present: owned recording flag without recording");uint64_t phase=dm_cpu_phase_begin_always();mtl_sub_commit();dm_cpu_phase_end(DM_CPU_PHASE_SUBMIT,phase);}
    unsigned acquire_slot=screen_acquire_slot;screen_acquire_slot=(acquire_slot+1)%SCREEN_ACQUIRE_RING;screen_acquired=screen_acquire_ring[acquire_slot];
    uint64_t completion_ticket=mtl_last_fence(),previous_acquire=screen_acquire_ring_ticket[acquire_slot];
    uint64_t previous_start=dm_cpu_phase_begin_always();
    if(previous_acquire&&!mtl_wait(previous_acquire,UINT64_MAX))die("Vulkan prior acquire consumer wait failed");
    uint64_t previous_acquire_ns=dm_android_clock_ns(CLOCK_MONOTONIC)-previous_start;
    dm_cpu_phase_end(DM_CPU_PHASE_WAIT_PRIOR,previous_start);
    PASSLOG("present serial %llu %ux%u fmt %#x",(unsigned long long)t->serial,t->d.width,t->d.height,t->d.format);
    if (!inner) { renderdoc_poll(); frame_profile(); pool_stats_log(); }
    uint64_t ticket=inner?presents:++presents;if(!inner){graphics_probe_capture(ticket);graphics_probe_poll();}
    if(!(ticket%60))LOG("[vulkan-prior-acquire] frame=%llu queued=%llu prior_consumer=%llu wait=%.3f ms scope=prior_binary_acquire_consumer current_guest_publication=async",(unsigned long long)ticket,(unsigned long long)completion_ticket,(unsigned long long)previous_acquire,previous_acquire_ns/1e6);
    // Top of the frame, holding no acquired image: the only place the game thread may tear down and rebuild the
    // swapchain (the other is right after a failed acquire, when no image is held either). After a surface loss, try at
    // most once a second to rebuild on the same window; same window resized / out of date / extent changed / last
    // attempt failed -> rebuild the swapchain on the current surface.
    // The previous frame's blit has completed (the wait above): if the current swapchain's retirement evidence has
    // arrived, reclaim the old swapchains and semaphores first, then decide whether to rebuild.
    screen_retire_collect();
#ifdef __ANDROID__
    if (!surface) screen_surface_retry();
#endif
    if (surface) {
        // Compare the raw window size with the one of the last build every frame (not relying on SUBOPTIMAL: devices whose
        // present returns VK_SUCCESS — natural-landscape tablets with an IDENTITY transform, desktop windowing — never get
        // it). ANativeWindow_getWidth/Height go through window->query and may reach the producer (an in-process locked query
        // from API 31, a binder call to SurfaceFlinger on API 29-30); the time is measured and printed with the 600-frame
        // line. While the last build was clamped, the capabilities are also re-read at most every 60 frames
        // (screen_clamp_moved).
        int known = 0, ww = 0, wh = 0;
        if (swapchain) {
            int moved = screen_window_changed(&known, &ww, &wh);
            if (!moved && known && screen_built_clamped && presents - screen_clamp_checked >= 60) { screen_clamp_checked = presents; moved = screen_clamp_moved(ww, wh); }
            if (moved) atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_EXTENT);
        }
        int dirty = atomic_exchange(&screen_dirty, 0);
        if (dirty || !swapchain) {
            char why[96]; snprintf(why, sizeof why, "%s%s%s%s%s", !swapchain ? "no_swapchain " : "", dirty & SCREEN_DIRTY_RESIZED ? "window_resized " : "", dirty & SCREEN_DIRTY_OUT_OF_DATE ? "out_of_date " : "",
                                   dirty & SCREEN_DIRTY_EXTENT ? "extent_changed " : "", dirty & SCREEN_DIRTY_RETRY ? "retry " : "");
            screen_recreate(why, dirty);
        }
    }
    if (!swapchain) wsi_skipped++;
    if (swapchain) {
        // The prior blit consumed the acquire token; a separate returned GPU ticket tracks this frame's completion.
        if(screen_acquire_ring_ticket[acquire_slot]&&!mtl_wait(screen_acquire_ring_ticket[acquire_slot],0))die("Vulkan acquire token reused before its consumer completed");
        uint32_t index = 0; VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
        for (int attempt = 0; swapchain && attempt < 2; attempt++) {
            uint64_t acquire_start=dm_android_clock_ns(CLOCK_MONOTONIC);
            result = vkAcquireNextImageKHR(device, swapchain, SCREEN_ACQUIRE_TIMEOUT_NS, screen_acquired, VK_NULL_HANDLE, &index);
            acquire_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-acquire_start;
            dm_cpu_phase_end(DM_CPU_PHASE_ACQUIRE,acquire_start);
            wsi_count(wsi_acquire, result);
            // Out of date: a failed acquire signals nothing and holds no image, so rebuild on the same surface right here
            // and acquire once more (a single retry).
            if (result != VK_ERROR_OUT_OF_DATE_KHR || attempt || !screen_recreate("acquire_out_of_date", SCREEN_DIRTY_OUT_OF_DATE)) break;
        }
        if (result == VK_SUBOPTIMAL_KHR) screen_note_suboptimal("acquire");
        else if (result == VK_ERROR_OUT_OF_DATE_KHR) atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_OUT_OF_DATE);
        else if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            // A timeout does not mean the swapchain is broken (most likely nobody is consuming the buffer queue for the
            // moment): skip this frame only, no rebuild (rebuilding every frame would idle the queue).
            if (wsi_acquire[WSI_TIMEOUT] <= 8 || !(wsi_acquire[WSI_TIMEOUT] % 64)) LOG("[vulkan-wsi] 【红】acquire returned %d after %.0f ms (frame %llu, #%llu): frame not presented", result, acquire_ns / 1e6, (unsigned long long)ticket, (unsigned long long)wsi_acquire[WSI_TIMEOUT]);
        }
        else if (result == VK_ERROR_SURFACE_LOST_KHR) { if (swapchain) screen_surface_lost("acquire"); }
        else if (result != VK_SUCCESS) die("Vulkan acquire: %d", result);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) wsi_skipped++;
        else {
        if(index>=screen_count)die("Vulkan acquire image bounds");screen_rendered=screen_present_wait[index];
        // Retirement evidence: an image of the current swapchain already submitted to vkQueuePresentKHR (screen_initialized,
        // one table per chain, set at the blit right before the present; a rejected present still counts as enqueued) is
        // acquired again; once the blit consuming this acquire completes, its previous present has completed (see screen_retire).
        int retire_evidence = screen_retire_count && !screen_retire_ticket && screen_initialized[index];
        uint64_t blit_phase=dm_cpu_phase_begin_always();
        mtl_sub_begin(); image_entry *e = source ? source : image_get(t); image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        /* TOTK_VK_PRESENT_PROBE=1: every 120 frames, read back 4x4 texels at the centre of the presented rectangle (debug) */
        static int probe_on = -1; if (probe_on < 0) { const char *pe = getenv("TOTK_VK_PRESENT_PROBE"); probe_on = pe && atoi(pe); }
        vk_buffer probe = {0}; int probe_bpb = 0;
        int probe_x = crop && crop[2] > 0 ? crop[0] + crop[2] / 2 : (int)t->d.width / 2, probe_y = crop && crop[3] > 0 ? crop[1] + crop[3] / 2 : (int)t->d.height / 2;
        if (probe_on && !(ticket % 120)) { int bw, bh; probe_bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
            if (bw == 1 && bh == 1 && probe_bpb > 0 && probe_bpb <= 16 && t->d.width >= 4 && t->d.height >= 4) { probe = buffer_create(16 * probe_bpb);
                VkBufferImageCopy pc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {probe_x - 2, probe_y - 2, 0}, .imageExtent = {4, 4, 1}};
                vkCmdCopyImageToBuffer(command, e->image, VK_IMAGE_LAYOUT_GENERAL, probe.buffer, 1, &pc); } }
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = screen_initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = screen_images[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        // Connect the first layout transition to the acquire TRANSFER wait, after the presentation engine's image use.
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue black = {.float32 = {0,0,0,1}}; vkCmdClearColorImage(command, screen_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &barrier.subresourceRange);
        VkMemoryBarrier clear_order = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &clear_order, 0, NULL, 0, NULL);
        int sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0, sw = crop && crop[2] > 0 ? crop[2] : t->d.width, sh = crop && crop[3] > 0 ? crop[3] : t->d.height;
        if (source) { sx = sy = 0; sw = (int)source->texture.d.width; sh = (int)source->texture.d.height; }
        if (!repeat) lat_probe_record(e, t, sx, sy, sw, sh, pace_id + 1);
        int fit[4];
        dm_android_present_fit(sw, sh, (int)screen_extent.width, (int)screen_extent.height, fit);
        int dw = fit[2], dh = fit[3], x = fit[0], y = fit[1];
        { static uint32_t lw, lh; if (lw != (uint32_t)sw || lh != (uint32_t)sh) { lw = sw; lh = sh; LOG("[vulkan-present] picture %dx%d (texture %ux%u nvn format %#x vk %d) -> %dx%d of %ux%u", sw, sh, t->d.width, t->d.height, t->d.format, (int)e->format, dw, dh, screen_extent.width, screen_extent.height); } }
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .srcOffsets = {{sx,sy,0},{sx+sw,sy+sh,1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .dstOffsets = {{x,y,0},{x+dw,y+dh,1}}};
        /* FSR 1, else the linear stretch */
        if (!upscale_present(e, sx, sy, sw, sh, screen_images[index], x, y, dw, dh))
        vkCmdBlitImage(command, e->image, VK_IMAGE_LAYOUT_GENERAL, screen_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        /* bug-report recording (nvn_vulkan_record.inc), before the menu and HUD */
        if (!repeat) rec_picture(e, sx, sy, sw, sh);
        if (screen_color_attachment) { dv_drain();
            uint64_t q = atomic_load(&async_pso_queued), done = atomic_load(&async_pso_built) + atomic_load(&async_pso_failed);
            overlay_stats st = {pipeline_creations, q > done ? q - done : 0, atomic_load(&async_pso_skipped), totk_translated, pace_screen_hz()};
            overlay_record(command, screen_images[index], screen_format, screen_extent.width, screen_extent.height, present_fps * present_shown, present_ms / present_shown, &st); }
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0; barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        screen_submission = 1; completion_ticket=screen_acquire_ticket=screen_acquire_ring_ticket[acquire_slot]=mtl_sub_commit(); screen_submission = 0; screen_initialized[index] = 1;
        rec_submitted(screen_acquire_ticket);
        lat_probe_collect(screen_acquire_ticket);
        if (retire_evidence) { screen_retire_ticket = screen_acquire_ticket; screen_retire_image = index; }
        if (passlog_samples) { mtl_wait(screen_acquire_ticket, UINT64_MAX); passlog_print(); }
        /* experiment: no CPU/GPU overlap */
        { static int sync = -1; if (sync < 0) sync = exp_env("sync");
          if (sync) mtl_wait(screen_acquire_ticket, UINT64_MAX); }
        if (probe.buffer) { mtl_wait(screen_acquire_ticket, UINT64_MAX); const uint8_t *px = probe.mapped; char hex[3 * 16 + 1] = {0};
            for (int i = 0; i < probe_bpb && i < 16; i++) sprintf(hex + 3 * i, "%02x ", px[i]);
            unsigned nonzero = 0; for (int i = 0; i < 16 * probe_bpb; i++) nonzero += px[i] != 0;
            LOG("[vulkan-present-probe] frame=%llu texture %ux%u format %#x serial %llu crop %d,%d %dx%d populated=%d gpu_written=%d centre texel: %s(nonzero bytes %u of %d)",
                (unsigned long long)ticket, t->d.width, t->d.height, t->d.format, (unsigned long long)t->serial, sx, sy, sw, sh, e->populated, e->gpu_written, hex, nonzero, 16 * probe_bpb);
            buffer_destroy(&probe); }
        dm_cpu_phase_end(DM_CPU_PHASE_BLIT,blit_phase);
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &screen_rendered, .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
        uint64_t present_lock_start=dm_android_clock_ns(CLOCK_MONOTONIC);pthread_mutex_lock(&queue_lock);
        uint64_t present_start=dm_android_clock_ns(CLOCK_MONOTONIC);present_lock_ns=present_start-present_lock_start;
        VkPresentTimeGOOGLE present_time; VkPresentTimesInfoGOOGLE present_times;
        /* a present id on every present when the extension is there: Android records a present's timing only then, and
           the on-screen histogram, TL_SHOWN and the latency estimate (pace_learn) need the timings also without pacing */
        if (pace_ext && pace_get_past) { present_time = (VkPresentTimeGOOGLE){++pace_id, pace_on() && pace_desired > 0 ? (uint64_t)pace_desired : 0}; pace_submit[pace_id % 64] = present_start;
            present_times = (VkPresentTimesInfoGOOGLE){.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &present_time}; pi.pNext = &present_times; }
        result = vkQueuePresentKHR(queue, &pi);present_ns=dm_android_clock_ns(CLOCK_MONOTONIC)-present_start;present_block_ns+=acquire_ns+present_ns+previous_acquire_ns;
        pace_learn();
        dm_cpu_phase_end(DM_CPU_PHASE_PRESENT,present_lock_start);
        pthread_mutex_unlock(&queue_lock);
        wsi_count(wsi_present, result);
        // The semaphore waits of an out-of-date / lost present request still execute (per the spec), so tearing down is
        // safe here: rebuild at the top of the next frame, or release the surface right away.
        if (result == VK_SUBOPTIMAL_KHR) screen_note_suboptimal("present");
        else if (result == VK_ERROR_OUT_OF_DATE_KHR) atomic_fetch_or(&screen_dirty, SCREEN_DIRTY_OUT_OF_DATE);
        else if (result == VK_ERROR_SURFACE_LOST_KHR) screen_surface_lost("present");
        else if (result != VK_SUCCESS) die("Vulkan present: %d", result);
        if(ticket<=3||!(ticket%600))LOG("[vulkan-present] frame=%llu image=%u submit=%llu completed=%llu semaphore_per_image=1 queue_idle=0",(unsigned long long)ticket,index,(unsigned long long)screen_acquire_ticket,(unsigned long long)atomic_load(&completed));
        if (ticket <= 3 || ticket == 30) LOG("[vulkan] Android game frame %llu presented", (unsigned long long)ticket);
        }
    }
    if (ticket <= 3 || !(ticket % 600))
        LOG("[vulkan-wsi] frame=%llu acquire ok/suboptimal/out_of_date/timeout/lost=%llu/%llu/%llu/%llu/%llu present=%llu/%llu/%llu/%llu/%llu rebuilds=%llu failed=%llu surface_rebuilds=%llu unpresented_frames=%llu extent=%ux%u retired/freed/pending/overflow=%llu/%llu/%u/%llu deferred=%llu same_size_skips=%llu window_query n/mean/max=%llu/%.1f/%.1f us",
            (unsigned long long)ticket, (unsigned long long)wsi_acquire[WSI_OK], (unsigned long long)wsi_acquire[WSI_SUBOPTIMAL], (unsigned long long)wsi_acquire[WSI_OUT_OF_DATE], (unsigned long long)wsi_acquire[WSI_TIMEOUT], (unsigned long long)wsi_acquire[WSI_SURFACE_LOST],
            (unsigned long long)wsi_present[WSI_OK], (unsigned long long)wsi_present[WSI_SUBOPTIMAL], (unsigned long long)wsi_present[WSI_OUT_OF_DATE], (unsigned long long)wsi_present[WSI_TIMEOUT], (unsigned long long)wsi_present[WSI_SURFACE_LOST],
            (unsigned long long)wsi_recreates, (unsigned long long)wsi_recreate_failures, (unsigned long long)wsi_surface_rebuilds, (unsigned long long)wsi_skipped, screen_extent.width, screen_extent.height,
            (unsigned long long)wsi_retired, (unsigned long long)wsi_retire_freed, screen_retire_count, (unsigned long long)wsi_retire_overflow, (unsigned long long)wsi_retire_deferred, (unsigned long long)wsi_same_size_skips,
            (unsigned long long)wsi_wq_n, wsi_wq_n ? wsi_wq_ns_sum / 1e3 / (double)wsi_wq_n : 0.0, wsi_wq_ns_max / 1e3);
    const char *capture = getenv("DM_VK_CAPTURE");
    if (capture && (ticket <= 8 || ticket == 30 || ticket == 60)) {
        if (t->d.format != NVN_FORMAT_RGBA8) die("Vulkan capture format %#x pending", t->d.format);
        unsigned w = t->d.width, h = t->d.height; uint8_t *pixels = malloc((size_t)w * h * 4); if (!pixels) die("Vulkan capture allocation");
        NVNcopyRegion r = {0, 0, 0, w, h, 1}; mtl_read_texels(t, 0, 0, &r, pixels, w * 4, 0);
        char path[1024]; snprintf(path, sizeof path, "%s/frame-%05llu.ppm", capture, (unsigned long long)ticket); FILE *file = fopen(path, "wb"); if (!file) die("Vulkan capture open %s", path);
        fprintf(file, "P6\n%u %u\n255\n", w, h); uint64_t nonblack = 0;
        for (uint64_t i = 0; i < (uint64_t)w * h; ++i) { fwrite(pixels + i * 4, 1, 3, file); nonblack += !!(pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]); }
        fclose(file); free(pixels); LOG("[vulkan] game frame %llu capture=%s nonblack=%llu draws=%llu", (unsigned long long)ticket, path, (unsigned long long)nonblack, (unsigned long long)draw_count);
    }
    dm_cpu_phase_end(DM_CPU_PHASE_PRESENT_TOTAL,present_total);
    static uint64_t perf_time,perf_submit,perf_bytes,perf_allocations,perf_hits,perf_passes,perf_barriers,perf_create_wall,perf_create_cpu;
    static double perf_gpu,perf_wait;
    uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);
    pthread_mutex_lock(&queue_lock);snapshot_diag_poll();dependencies_report();counter_gpu_report();graphics_pipeline_report();pipeline_disk_poll();double current_gpu=gpu_total_ns,current_wait=fence_wait_total_ns+(double)atomic_load(&fence_wait_unlocked_ns);
    static uint64_t driver_fast_previous,driver_build_previous,driver_bytes_previous,driver_clear_previous;
    if(ticket==1||ticket%60==0){if(ticket!=1){unsigned frames=ticket==60?59:60;
        LOG("[vulkan-driver-table] frame=%llu fast_hits/frame=%.1f builds/frame=%.1f avoided_table_MiB/frame=%.3f avoided_clear_MiB/frame=%.3f",(unsigned long long)ticket,(uniform_driver_fast_hits-driver_fast_previous)/(double)frames,(uniform_driver_builds-driver_build_previous)/(double)frames,(uniform_driver_avoided_bytes-driver_bytes_previous)/(frames*1048576.),(uniform_clear_avoided_bytes-driver_clear_previous)/(frames*1048576.));}
        driver_fast_previous=uniform_driver_fast_hits;driver_build_previous=uniform_driver_builds;driver_bytes_previous=uniform_driver_avoided_bytes;driver_clear_previous=uniform_clear_avoided_bytes;}
    pthread_mutex_unlock(&queue_lock);
    if(ticket==1){perf_time=now;perf_submit=atomic_load(&submitted);perf_bytes=snapshot_bytes;perf_allocations=buffer_allocations;perf_hits=read_cache_hits;perf_create_wall=buffer_create_wall_ns;perf_create_cpu=buffer_create_cpu_ns;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;dm_cpu_phase_report(frames);LOG("[vulkan-perf] frame=%llu fps=%.2f submits/frame=%.1f snapshots/frame=%.1f MB allocations/frame=%.1f new_buf_wall_ms/frame=%.3f new_buf_cpu_ms/frame=%.3f vertex_hits=%llu cache=%llu MB",(unsigned long long)ticket,frames*1e9/(now-perf_time),(double)(atomic_load(&submitted)-perf_submit)/frames,(double)(snapshot_bytes-perf_bytes)/(frames*1048576.),(double)(buffer_allocations-perf_allocations)/frames,(double)(buffer_create_wall_ns-perf_create_wall)/(frames*1e6),(double)(buffer_create_cpu_ns-perf_create_cpu)/(frames*1e6),(unsigned long long)(read_cache_hits-perf_hits),(unsigned long long)(read_cache_bytes>>20));LOG("[vulkan-images] frame=%llu created=%llu reused=%llu free_list=%llu MiB",(unsigned long long)ticket,(unsigned long long)image_creations,(unsigned long long)image_reuses,(unsigned long long)(free_image_bytes>>20));{static uint64_t c0,b0,u0;if(zc_pools)LOG("[zerocopy] frame=%llu in-place reads/frame=%.1f (%.2f MB) still snapshotted/frame=%.1f; pools exported=%llu (%.0f MiB) refused=%llu overlays=%u",(unsigned long long)ticket,(double)(zc_bound_calls-c0)/frames,(double)(zc_bound_bytes-b0)/(frames*1048576.),(double)(zc_unbound_calls-u0)/frames,(unsigned long long)zc_pools,zc_bytes/1048576.0,(unsigned long long)zc_failures,compat_overlay_count());
      /* the dirty tracker's cost on the game's threads */
      { static uint64_t f0,p0,mc0,mp0,mn0;uint64_t f=0,pr=0,mc=0,mpg=0,mns=0;if(nvn_dirty_stats)nvn_dirty_stats(&f,&pr);if(nvn_dirty_mp_stats)nvn_dirty_mp_stats(&mc,&mpg,&mns);
        LOG("[dirty] frame=%llu write faults/frame=%.1f protects/frame=%.1f mprotect calls/frame=%.1f pages/frame=%.0f ms/frame=%.3f",(unsigned long long)ticket,(double)(f-f0)/frames,(double)(pr-p0)/frames,(double)(mc-mc0)/frames,(double)(mpg-mp0)/frames,(double)(mns-mn0)/(frames*1e6));f0=f;p0=pr;mc0=mc;mp0=mpg;mn0=mns; }c0=zc_bound_calls;b0=zc_bound_bytes;u0=zc_unbound_calls;}perf_time=now;perf_submit=atomic_load(&submitted);perf_bytes=snapshot_bytes;perf_allocations=buffer_allocations;perf_hits=read_cache_hits;perf_create_wall=buffer_create_wall_ns;perf_create_cpu=buffer_create_cpu_ns;}
    if(ticket==1||ticket%60==0){static uint64_t large_alloc,large_bytes,large_reuse,large_freed;
        pthread_mutex_lock(&buffer_cache_lock);uint64_t a=buffer_large_allocations,ab=buffer_large_allocated_bytes,r=buffer_large_reuses,f=buffer_large_freed,cached=buffer_large_cache_bytes;pthread_mutex_unlock(&buffer_cache_lock);
        if(ticket!=1){unsigned frames=ticket==60?59:60;
            LOG("[vulkan-buffer-large] frame=%llu allocations/frame=%.2f allocated=%.2f MiB/frame reuses/frame=%.2f freed/frame=%.2f cached=%llu MiB budget=%llu MiB (buffers > %llu MiB)",
                (unsigned long long)ticket,(a-large_alloc)/(double)frames,(ab-large_bytes)/(frames*1048576.),(r-large_reuse)/(double)frames,(f-large_freed)/(double)frames,
                (unsigned long long)(cached>>20),(unsigned long long)(DM_VK_LARGE_CACHE_BUDGET>>20),(unsigned long long)(DM_VK_SMALL_BUFFER_MAX>>20));}
        large_alloc=a;large_bytes=ab;large_reuse=r;large_freed=f;}
    if(ticket==1){perf_gpu=current_gpu;perf_wait=current_wait;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;LOG("[vulkan-time] frame=%llu GPU=%.2f ms/frame fence_wait=%.2f ms/frame (GPU batch spans; exclude present/vsync)",(unsigned long long)ticket,batch_times?(current_gpu-perf_gpu)/(frames*1e6):-1,(current_wait-perf_wait)/(frames*1e6));perf_gpu=current_gpu;perf_wait=current_wait;}
    // Abnormal-frame log: take this frame's interval, thread CPU and GPU at every present boundary; only frames
    // over the thresholds are written to their own file.
    {static uint64_t abnormal_wall,abnormal_cpu,abnormal_gpu;
     uint64_t abnormal_now=dm_android_clock_ns(CLOCK_MONOTONIC),abnormal_thread=dm_android_clock_ns(CLOCK_THREAD_CPUTIME_ID);
     if(abnormal_wall&&abnormal_now>=abnormal_wall&&abnormal_thread>=abnormal_cpu)
         dm_abnormal_frame(ticket,abnormal_now-abnormal_wall,abnormal_thread-abnormal_cpu,current_gpu-abnormal_gpu,draw_count,render_passes,texture_uploads,uniform_version_read_bytes,uniform_version_misses,descriptor_version_misses);
     abnormal_wall=abnormal_now;abnormal_cpu=abnormal_thread;abnormal_gpu=current_gpu;}
    if(!inner&&ticket%600==0)LOG("[vulkan-memory] frame=%llu %lld memory objects live (driver limit %u)",(unsigned long long)ticket,(long long)atomic_load(&vk_memory_objects),gpu_limits.maxMemoryAllocationCount);
    if(ticket==1){perf_passes=render_passes;perf_barriers=image_barriers;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;LOG("[vulkan-passes] frame=%llu passes/frame=%.1f image_barriers/frame=%.1f UBO_arena=%.1f MB driver_hits=%llu layout/pass/frame objects=%llu/%llu/%llu",(unsigned long long)ticket,(render_passes-perf_passes)/(double)frames,(image_barriers-perf_barriers)/(double)frames,uniform_arena_bytes/1048576.,(unsigned long long)uniform_driver_hits,(unsigned long long)graphics_layout_creations,(unsigned long long)graphics_pass_creations,(unsigned long long)graphics_frame_creations);perf_passes=render_passes;perf_barriers=image_barriers;}
    static uint64_t binding_hits,binding_misses,descriptor_hits,descriptor_misses,descriptor_memo,binding_bytes,binding_compare,binding_copy,dependency_waits,binding_overwrites,binding_uploads,binding_unregistered;
    if(ticket==1){binding_hits=uniform_version_hits;binding_misses=uniform_version_misses;descriptor_hits=descriptor_version_hits;descriptor_misses=descriptor_version_misses;descriptor_memo=descriptor_memo_hits;binding_bytes=uniform_version_read_bytes;binding_compare=uniform_version_compare_bytes;binding_copy=uniform_version_copy_bytes;dependency_waits=dependency_flushes;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;static uint64_t binding_reuses;LOG("[vulkan-bindings] frame=%llu UBO reuse/byte_hits/miss=%.1f/%.1f/%.1f descriptors memo/hits/miss=%.1f/%.1f/%.1f UBO_read=%.2f MB/frame UBO_compare=%.2f MB/frame UBO_arena_copy=%.2f MB/frame dependency_wait=%.1f/frame gpu_overwrite=%.2f/frame uploads=%.2f/frame unregistered_tex=%.2f/frame",(unsigned long long)ticket,(uniform_version_reuse_hits-binding_reuses)/(double)frames,(uniform_version_hits-binding_hits)/(double)frames,(uniform_version_misses-binding_misses)/(double)frames,(descriptor_memo_hits-descriptor_memo)/(double)frames,(descriptor_version_hits-descriptor_hits)/(double)frames,(descriptor_version_misses-descriptor_misses)/(double)frames,(uniform_version_read_bytes-binding_bytes)/(frames*1048576.),(uniform_version_compare_bytes-binding_compare)/(frames*1048576.),(uniform_version_copy_bytes-binding_copy)/(frames*1048576.),(dependency_flushes-dependency_waits)/(double)frames,(image_gpu_overwrites-binding_overwrites)/(double)frames,(texture_uploads-binding_uploads)/(double)frames,(gpu_unregistered_textures-binding_unregistered)/(double)frames);binding_unregistered=gpu_unregistered_textures;binding_reuses=uniform_version_reuse_hits;binding_hits=uniform_version_hits;binding_misses=uniform_version_misses;descriptor_hits=descriptor_version_hits;descriptor_misses=descriptor_version_misses;descriptor_memo=descriptor_memo_hits;binding_bytes=uniform_version_read_bytes;binding_compare=uniform_version_compare_bytes;binding_copy=uniform_version_copy_bytes;dependency_waits=dependency_flushes;binding_overwrites=image_gpu_overwrites;binding_uploads=texture_uploads;}
    static uniform_read_counters uniform_read_previous;
    if(ticket==1)uniform_read_previous=uniform_read_stats;
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;const uniform_read_counters *s=&uniform_read_stats;
        LOG("[vulkan-ubo-read] frame=%llu known_copy=%.1f/frame known_bytes=%.3f MiB/frame safe_fallback=%.1f/frame safe_requested=%.3f MiB/frame safe_errors=%llu",(unsigned long long)ticket,(s->known_calls-uniform_read_previous.known_calls)/(double)frames,(s->known_bytes-uniform_read_previous.known_bytes)/(frames*1048576.),(s->safe_calls-uniform_read_previous.safe_calls)/(double)frames,(s->safe_requested_bytes-uniform_read_previous.safe_requested_bytes)/(frames*1048576.),(unsigned long long)(s->safe_errors-uniform_read_previous.safe_errors));uniform_read_previous=*s;}
    static uint64_t command_emits,command_skips,gpu_reads,gpu_seeds,gpu_references;
    if(ticket==1){command_emits=graphics_command_emitted;command_skips=graphics_command_reused;gpu_reads=forward_read_bytes;gpu_seeds=forward_seed_bytes;gpu_references=forward_references;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;LOG("[vulkan-forward] frame=%llu driver_commands/frame=%.1f reused/frame=%.1f GPU_read=%.2f MB/frame GPU_seed=%.2f MB/frame retained_backings=%.1f/frame",(unsigned long long)ticket,(graphics_command_emitted-command_emits)/(double)frames,(graphics_command_reused-command_skips)/(double)frames,(forward_read_bytes-gpu_reads)/(frames*1048576.),(forward_seed_bytes-gpu_seeds)/(frames*1048576.),(forward_references-gpu_references)/(double)frames);command_emits=graphics_command_emitted;command_skips=graphics_command_reused;gpu_reads=forward_read_bytes;gpu_seeds=forward_seed_bytes;gpu_references=forward_references;}
    static uint64_t index_hits,index_misses,index_scan_bytes;
    if(ticket==1){index_hits=index_maximum_hits;index_misses=index_maximum_misses;index_scan_bytes=index_maximum_scan_bytes;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;LOG("[vulkan-indices] frame=%llu maximum_hits/miss=%.1f/%.1f scan=%.2f MB/frame",(unsigned long long)ticket,(index_maximum_hits-index_hits)/(double)frames,(index_maximum_misses-index_misses)/(double)frames,(index_maximum_scan_bytes-index_scan_bytes)/(frames*1048576.));index_hits=index_maximum_hits;index_misses=index_maximum_misses;index_scan_bytes=index_maximum_scan_bytes;}
    static uint64_t texel_hits,texel_creations,texel_reads,texel_forwarded;
    if(ticket==1){texel_hits=texel_view_hits;texel_creations=texel_view_creations;texel_reads=texel_cpu_read_bytes;texel_forwarded=texel_forward_bytes;}
    else if(!inner&&ticket%60==0){unsigned frames=ticket==60?59:60;LOG("[vulkan-texel] frame=%llu view_hits/new=%.1f/%.1f CPU_read=%.2f MB/frame forwarded=%.2f MB/frame",(unsigned long long)ticket,(texel_view_hits-texel_hits)/(double)frames,(texel_view_creations-texel_creations)/(double)frames,(texel_cpu_read_bytes-texel_reads)/(frames*1048576.),(texel_forward_bytes-texel_forwarded)/(frames*1048576.));texel_hits=texel_view_hits;texel_creations=texel_view_creations;texel_reads=texel_cpu_read_bytes;texel_forwarded=texel_forward_bytes;}
    dm_android_perf_hint_frame();
    static uint64_t texture_calls_previous,texture_blocks_previous,texture_ns_previous;
    if(ticket==1||ticket%60==0){uint64_t calls=atomic_load(&present_texture_wait_calls),blocks=atomic_load(&present_texture_wait_blocks),ns=atomic_load(&present_texture_wait_ns);
        if(ticket!=1){unsigned frames=ticket==60?59:60;LOG("[vulkan-texture-reuse-wait] frame=%llu calls/frame=%.2f initially_pending/frame=%.2f wall_ms/frame=%.3f completion=GPU_fence_and_guest_publication",(unsigned long long)ticket,(calls-texture_calls_previous)/(double)frames,(blocks-texture_blocks_previous)/(double)frames,(ns-texture_ns_previous)/(frames*1e6));}
        texture_calls_previous=calls;texture_blocks_previous=blocks;texture_ns_previous=ns;}
    frame_stability_record(ticket,previous_acquire_ns,acquire_ns,present_ns,present_lock_ns);
    return completion_ticket;
}
static const tex_t *present_last; static double present_last_period; static int present_last_crop[4], present_last_has_crop;
void runtime_audio_hold(int on) __attribute__((weak));
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    pthread_mutex_lock(&present_serial);
    /* The mixer starts silent until the renderer says the game is up: nvn_mtl.m does it after its shader warm-up, which this
       backend does not have, so the first picture does. Without this the game is silent. */
    { static int released; if (!released) { released = 1; if (runtime_audio_hold) runtime_audio_hold(0); } }
    shader_setup_start();
    { static uint64_t prev; uint64_t now = dm_android_clock_ns(CLOCK_MONOTONIC);
      if (prev && now > prev) { float ms = (now - prev) / 1e6f; present_ms = present_ms ? present_ms * 0.9f + ms * 0.1f : ms; present_fps = present_ms > 0 ? 1000.0f / present_ms : 0; }
      prev = now; }
    image_entry *gen[LSFG_MAX_OUT]; int ngen = lsfg_prepare(t, crop, gen);
    /* frame generation: the picture between the previous and this frame first */
    image_entry *mid = lsfg_enabled() ? NULL : fg_prepare(t, crop);
    present_shown = present_shown * 0.9f + (1.0f + (ngen > 0 ? (float)ngen : mid ? 1.0f : 0.0f)) * 0.1f;
    /* nvn_vulkan_pacing.inc: even parts of a game frame at the frame cap */
    int64_t desired[LSFG_MAX_OUT + 1]; int planned = ngen > 0 ? ngen : mid ? 1 : 0;
    { int cap = nvn_frame_cap_get ? nvn_frame_cap_get() : 0; pace_plan(planned, cap > 0 ? 1000.0 / cap : 1000.0 / 30, desired); }
    if (mid) { pace_desired = desired[0]; present_one(t, period, crop, mid, 0); }
    for (int i = 0; i < ngen; i++) { pace_desired = desired[i]; present_one(t, period, crop, gen[i], 0); }
    pace_desired = desired[planned];
    uint64_t ticket = present_one(t, period, crop, NULL, 0);
    pace_desired = -1;
    fg_frame_end(t, crop);
    present_last = t; present_last_period = period; present_last_has_crop = crop != NULL; if (crop) memcpy(present_last_crop, crop, sizeof present_last_crop);
    pthread_mutex_unlock(&present_serial);
    return ticket;
}
/* The startup screen's backdrop: the game's picture when the menu opens (before the menu is drawn on it), as
   <files>/backdrop.ppm. The player's own frame, kept on the device. */
static void backdrop_save(const tex_t *t, const int crop[4]) {
    const char *files = getenv("TOTK_FILES_DIR"); char path[600], tmp[620];
    if (!files || (crop && (crop[0] || crop[1]))) return;
    uint32_t w = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, h = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    snprintf(path, sizeof path, "%s/backdrop.ppm", files); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (image_save_ppm(image_get(t), w, h, tmp)) rename(tmp, path);
}
/* While the menu is open the game waits at frame acquire: re-present its last picture with the menu, about 60 times a
   second. */
static void *overlay_present_thread(void *arg) {
    (void)arg; int saved = 0;
    for (;;) {
        struct timespec ts = {0, 16000000}; nanosleep(&ts, NULL);
        if (!overlay_wants_present() || !present_last) { saved = 0; continue; }
        pthread_mutex_lock(&present_serial);
        /* prepare mode shows its panel over the boot frame: the player's last frame stays */
        if (!saved++ && !prepare_mode()) backdrop_save(present_last, present_last_has_crop ? present_last_crop : NULL);
        present_one(present_last, present_last_period, present_last_has_crop ? present_last_crop : NULL, NULL, 1);
        pthread_mutex_unlock(&present_serial);
    }
    return NULL;
}
void mtl_overlay_start(void) {
    static int started; if (started++) return;
    pthread_t th; if (!pthread_create(&th, NULL, overlay_present_thread, NULL)) pthread_detach(th);
}
void mtl_present_wait(uint64_t ticket) {
    if(!ticket)return;
    /* experiment TOTK_VK_EXP=nopresentwait: the window texture's next rendering is ordered after its present copy on the
       one Vulkan queue, so the CPU need not wait for that copy (it blocks the game's acquire ~22 ms per frame) */
    { static int skip=-1;if(skip<0)skip=exp_env("nopresentwait");if(skip)return; }
    if(ticket>mtl_last_fence())die("Vulkan unknown present completion ticket %llu",(unsigned long long)ticket);
    uint64_t started=dm_android_clock_ns(CLOCK_MONOTONIC);int pending=atomic_load(&completed)<ticket;
    if(!mtl_wait(ticket,UINT64_MAX))die("Vulkan texture reuse wait failed");
    atomic_fetch_add(&present_texture_wait_calls,1);atomic_fetch_add(&present_texture_wait_blocks,pending);
    atomic_fetch_add(&present_texture_wait_ns,dm_android_clock_ns(CLOCK_MONOTONIC)-started);
}
int mtl_display_hz(void) {
#ifdef __ANDROID__
    // Report the queried refresh rate only with a real surface and frontend; headless or frontend-free fixtures remain zero.
    extern int dm_android_display_hz(void) __attribute__((weak));
    if(surface&&dm_android_display_hz)return dm_android_display_hz();
#endif
    return 0;
}
int mtl_has_window(void) { return surface != VK_NULL_HANDLE; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout, unsigned frame_ticks) { (void)timeout; (void)frame_ticks; return 0; }
int mtl_warm_wait(int timeout) {
    // Called by hle_nvn at the first run-entry loading screen: wait for warm-up here (the loading screen stays a bit
    // longer) so pipelines are not compiled on the fly after entering the run.
    if(!atomic_load(&pso_warm_running))return 0;
    uint64_t start=dm_android_clock_ns(CLOCK_MONOTONIC),limit=(uint64_t)(timeout>0?timeout:0)*1000000ull;
    while(atomic_load(&pso_warm_running)&&dm_android_clock_ns(CLOCK_MONOTONIC)-start<limit)usleep(2000);
    return (int)((dm_android_clock_ns(CLOCK_MONOTONIC)-start)/1000000ull)+1;
}
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
void mtl_set_frame(uint64_t frame) { (void)frame; }
void mtl_ub_present(double interval) { (void)interval; }
double mtl_gpu_ms(uint64_t frame) { (void)frame; return -1; }
int mtl_gpu_span(uint64_t frame, uint64_t *t0, uint64_t *t1) { (void)frame; *t0 = *t1 = 0; return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = render_passes; *draws = draw_count; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = resource_created_count[0]; *pso = resource_created_count[1] + resource_created_count[2]; }
void mtl_create_ns(uint64_t *lib, uint64_t *pso) { *lib = resource_created_ns[0]; *pso = resource_created_ns[1] + resource_created_ns[2]; }
uint64_t mtl_upload_bytes(void) { return 0; }
void mtl_alloc_stats(uint64_t *wait, uint64_t *grow, int *n) { *wait = *grow = 0; *n = 1; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb, int ok, unsigned long long total, unsigned long long free_size, unsigned long long max) { (void)ok; (void)total; (void)free_size; (void)max; LOG("[vulkan] frame=%llu resident=%llu MB submits=%llu allocations=%llu reuse=%llu snapshots=%llu MB pipelines=%llu/%llu", frame, mb, (unsigned long long)atomic_load(&submitted), (unsigned long long)buffer_allocations, (unsigned long long)buffer_reuses, (unsigned long long)(snapshot_bytes >> 20), (unsigned long long)pipeline_creations, (unsigned long long)pipeline_reuses); }
void dm_gfx_shot_at(long frame, const char *path) { (void)frame; dm_shot_written(path, 0); }
