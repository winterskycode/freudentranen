/* TOTK glue for the Vulkan backend: the parts of the backend interface TOTK's hle_nvn.c calls that the D2R backend does
   not have (statistics, diagnostics, picture modes, submit-time uniform capture, CPU texel writes, pool invalidation),
   and the window hand-over from the Android front end. Placeholders log once where real work is still missing. */
#include "runtime.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <android/native_window.h>
#include <stdio.h>
#include <string.h>

#define ONCE(...) do { static int said_; if (!said_++) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- window ---- */
void *android_vi_window(void);
void *front_metal_layer(void) { return android_vi_window(); }   /* the ANativeWindow, NULL until the activity has one */


/* ---- not yet implemented in the Vulkan backend ---- */
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    (void)p; (void)off; (void)n;
    ONCE("[vulkan-totk] MemoryPoolInvalidateMappedRange: GPU writes reach guest memory through the backend's write-back\n");
}
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
void mtl_note_world(void) {}

/* ---- the Mac's picture modes (MetalFX, TAA): off here; this backend has its own upscalers and AA ---- */
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
int mfxup_output_enabled(void) { return 0; }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}

/* ---- statistics TOTK's frame reports read (zero until the backend counts them) ---- */
uint64_t g_draw_skip[16];
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_pool_res_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_upload_wait_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }

/* ---- diagnostics hooks ---- */
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
void dm_gfx_anomdump(void) {}
void dm_gfx_ashot(const char *path) { (void)path; }   /* the Metal backend's screenshot of the current frame (runtime/input.c "ashot" on Breath of the Wild): none here */
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_nvn_probe_prediction(void) {}
void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }
