/* Android answers to hooks MacNativeGames' shared runtime calls in its Mac front end (gamepad.m, frontend.m) and Metal backend
   (nvn_mtl.m, totk_shader_rt.m). Weak: the Vulkan backend or the host replaces any of them it implements. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* runtime/gamepad.m: a one-word setting file next to controls.txt (rumble.txt, gyro.txt); here in the app's files folder
   (TOTK_FILES_DIR). 1 with the word, 0 when there is none. */
__attribute__((weak)) int runtime_setting_word(const char *name, char *word, size_t size) {
    const char *dir = getenv("TOTK_FILES_DIR"); char path[1024];
    if (!dir || !size || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int n = fscanf(f, "%63s", word) == 1; fclose(f);
    if (!n) return 0;
    word[size - 1] = 0;
    for (char *p = word; *p; p++) *p = (char)tolower((unsigned char)*p);
    return 1;
}
/* Key 1 opens the Mac's settings panel; the Android app has its own in-game menu (vk/overlay.cpp). */
__attribute__((weak)) void runtime_settings_toggle(void) {}

/* Metal backend statistics for the frame TSV (hle_nvn.c): none here. */
__attribute__((weak)) void totk_rt_submit_stats(uint64_t *wait_n, uint64_t *wait_ns, uint64_t *sync_n, uint64_t *sync_ns) { *wait_n = *wait_ns = *sync_n = *sync_ns = 0; }
__attribute__((weak)) void mtl_wait_stats(uint64_t *plib_ns, uint64_t *pso_ns) { *plib_ns = *pso_ns = 0; }
__attribute__((weak)) void mtl_scan_stats(uint64_t *shader_ns, uint64_t *pso_ns) { *shader_ns = *pso_ns = 0; }
__attribute__((weak)) uint64_t mtl_view_stats(void) { return 0; }
__attribute__((weak)) void mtl_variant_stats(uint64_t *n, uint64_t *ns, uint64_t *pre, uint64_t *bg, uint64_t *plain) { *n = *ns = *pre = *bg = *plain = 0; }
/* TOTK_SHADER_PREFETCH (Metal): the Vulkan backend translates variants as draws meet them. */
/* mtl_prog_prefetch: the Vulkan backend has its own (vk/nvn_vulkan_prefetch.inc) */
/* Metal: small render targets the CPU polls are written back once their fence passes. The Vulkan backend writes them
   back through its own read-back path. */
__attribute__((weak)) void mtl_wb_fence_done(void) {}
/* hle_nvn.c synchronous replay (TOTK_ASYNC_REPLAY=0): the Metal backend's upload batch follows the submitting thread. The
   Vulkan backend has no per-thread batch. */
__attribute__((weak)) void mtl_take_submit_thread(void) {}
/* Packaging tool for the iPhone's pipeline archive (loader.c "pipeline-archive"): Mac only. */
__attribute__((weak)) int mtl_pipeline_archive_tool(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "pipeline-archive: Mac only\n"); return 2; }
/* Stereo by replay (hle_nvn.c, Metal backend): never on here, so the submit path replays once and takes none of the
   others; they exist for the link. */
__attribute__((weak)) int mtl_stereo_replay(void) { return 0; }
__attribute__((weak)) int mtl_stereo_project(void) { return 0; }
__attribute__((weak)) void mtl_eye_pass(int e) { (void)e; }
__attribute__((weak)) void *mtl_ucap_dup(const void *base) { (void)base; return NULL; }
__attribute__((weak)) int mtl_ucap_eye(void *cap, int eye) { (void)cap; (void)eye; return 0; }
__attribute__((weak)) void mtl_ucap_free(void *c) { (void)c; }
__attribute__((weak)) void mtl_eye_capture(int eye) { (void)eye; }
/* Live button prompt icons (Metal, totk_prompt_icons.m): the Android prompts are the field prompt patches, so no atlas
   is kept and a pool's removal has nothing to drop. */
__attribute__((weak)) void totk_prompt_remove_pool(const void *memory, size_t size) { (void)memory; (void)size; }
/* input.c's "settings <page>" test command opens the Mac's settings panel; the Android menu is vk/overlay.cpp. */
__attribute__((weak)) void runtime_settings_page(int page) { (void)page; }
/* input.c's live VR resolution command: rejected, there is no headset output here. */
__attribute__((weak)) int vr_set_resolution(int w, int h) { (void)w; (void)h; return 0; }
/* loader.c tool modes of the Mac build: seed-shaders (nvn_mtl.m) and the Quest field-of-view probe. */
__attribute__((weak)) void runtime_shader_seed(const char *corpus, const char *list) { (void)corpus; (void)list; fprintf(stderr, "seed-shaders: Mac only\n"); exit(2); }
__attribute__((weak)) void runtime_totk_projection_probe(void) { fprintf(stderr, "probe-quest-fov: Mac only\n"); }
/* the VR compositor's camera search hook (totk_vr.m on the Mac) */
void (*g_vr_find_camera)(const float *);
/* MacNativeGames' Metal renderer since upstream 3edfb0b6, not in the Vulkan backend: the guest queue ordering
   (nvn_queue_order.inc, on for Ocarina of Time only: never called here), the pool address map for Metal argument
   buffers, the write-back storage reuse note, the no-read probe and the dispatch-barrier statistics. */
__attribute__((weak)) void mtl_dispatch_barrier_stats(uint64_t v[5]) { for (int i = 0; i < 5; i++) v[i] = 0; }
__attribute__((weak)) void mtl_noread_mark_frame(void) {}
__attribute__((weak)) void mtl_pool_addressed(const void *owner, const void *mem, uint64_t size) { (void)owner; (void)mem; (void)size; }
__attribute__((weak)) void mtl_pool_forget_address(const void *owner) { (void)owner; }
__attribute__((weak)) void mtl_wb_storage_reused(const uint8_t *mem, uint64_t n) { (void)mem; (void)n; }
__attribute__((weak)) int mtl_qo_event_ready(volatile uint32_t *val, uint32_t mode, uint32_t target) { (void)val; (void)mode; (void)target; return 1; }
__attribute__((weak)) void mtl_qo_cpu_signal(volatile uint32_t *val) { (void)val; }
__attribute__((weak)) int mtl_qo_event_gpu_projected(volatile uint32_t *val) { (void)val; return 0; }
__attribute__((weak)) void mtl_qo_arm(int on) { (void)on; }
__attribute__((weak)) int mtl_qo_stopped(void) { return 0; }
__attribute__((weak)) int mtl_qo_cursor(const uint8_t **out, int max) { (void)out; (void)max; return 0; }
__attribute__((weak)) void mtl_qo_set_callers(const uint8_t *const *outer_first, int n) { (void)outer_first; (void)n; }
__attribute__((weak)) void *mtl_qo_state_save(void) { return NULL; }
__attribute__((weak)) void mtl_qo_state_restore(void *p) { (void)p; }
__attribute__((weak)) void *mtl_ucap_overlay(const void *base, const void *over) { (void)base; (void)over; return NULL; }
__attribute__((weak)) void mtl_sub_snapshot_ubos_rest(const uint64_t *h, int n, int ncur, uint64_t top, void *cap) { (void)h; (void)n; (void)ncur; (void)top; (void)cap; }
/* the same since upstream 7c5026cb: the frame record's counters of the Metal renderer, the display link's pacing of a
   present (no display link here), and the cutscene switch of the MetalFX interpolation. */
__attribute__((weak)) uint64_t mtl_shader_scan_ns_take(void) { return 0; }
__attribute__((weak)) int mtl_present_diag(uint64_t n, double *t_in, double *tick, double *target, double *blit_end, int *merged) { (void)n; (void)t_in; (void)tick; (void)target; (void)blit_end; (void)merged; return 0; }
__attribute__((weak)) void mtl_pool_gen_stats(uint64_t out[9]) { for (int i = 0; i < 9; i++) out[i] = 0; }
__attribute__((weak)) void mtl_pend_overlay_stats(uint64_t v[8]) { for (int i = 0; i < 8; i++) v[i] = 0; }
__attribute__((weak)) void mtl_par_open_stats(uint64_t *n, uint64_t *ns) { *n = *ns = 0; }
__attribute__((weak)) void mtl_migrate_stats(uint64_t *n, uint64_t *rows, uint64_t *bytes, uint64_t *ns) { *n = *rows = *bytes = *ns = 0; }
__attribute__((weak)) void mfx_cutscene(int on) { (void)on; }
__attribute__((weak)) void runtime_front_hud_shot(const char *path) { (void)path; fprintf(stderr, "hud shot: Mac only\n"); }
