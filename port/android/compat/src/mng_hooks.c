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
/* zeldaVR stereo B2 (hle_nvn.c, Metal backend): stereo is never on here (game_patches_v143.c's Android stubs), so the
   submit path never takes these; they exist for the link. */
__attribute__((weak)) void mtl_eye_pass(int e) { (void)e; }
__attribute__((weak)) void vr_b2_pair_begin(void) {}
__attribute__((weak)) void vr_b2_eye_begin(void) {}
/* input.c's live VR resolution command: rejected, there is no headset output here. */
__attribute__((weak)) int vr_set_resolution(int w, int h) { (void)w; (void)h; return 0; }
/* loader.c tool modes of the Mac build: seed-shaders (nvn_mtl.m) and the Quest field-of-view probe. */
__attribute__((weak)) void runtime_shader_seed(const char *corpus, const char *list) { (void)corpus; (void)list; fprintf(stderr, "seed-shaders: Mac only\n"); exit(2); }
__attribute__((weak)) void runtime_totk_projection_probe(void) { fprintf(stderr, "probe-quest-fov: Mac only\n"); }
__attribute__((weak)) void mtl_eye_capture(int eye) { (void)eye; }
__attribute__((weak)) void mtl_eye_done(int left, int last) { (void)left; (void)last; }
__attribute__((weak)) int mtl_b2_right_first(void) { return 0; }
__attribute__((weak)) int mtl_b2_submit_uniforms_only(void) { return 0; }
__attribute__((weak)) void *mtl_ucap_overlay(const void *base, const void *over) { (void)base; (void)over; return NULL; }
__attribute__((weak)) void mtl_ucap_free(void *c) { (void)c; }
__attribute__((weak)) void vr_b2_present_eye(int eye) { (void)eye; }
/* the VR compositor's camera search hook (totk_vr.m on the Mac) */
void (*g_vr_find_camera)(const float *);
