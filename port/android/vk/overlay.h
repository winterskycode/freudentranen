/* In-game overlay: a quick menu, the full settings and the field controls, drawn with Dear ImGui over the presented
   picture, opened by the system's Back key (or a touch on the top-right corner), and an optional performance HUD.
   While the menu is open the game is paused at frame acquire and receives no input; the menu takes the controller
   and touch. overlay.cpp draws; nvn_vulkan.c records it into each present and re-presents the last picture while
   paused; app.c feeds input. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; uint32_t family; VkQueue queue;
    /* Vulkan function loader for the ImGui backend */
    PFN_vkVoidFunction (*proc)(const char *name, void *user);
} overlay_vk;

/* Settings are environment-style keys (TOTK_UNCAPPED_HZ, TOTK_UPSCALER, ...), listed in the APK's assets/settings.tsv, the
   table the startup screen also reads: the menu starts from the process environment (launch.env), calls apply() for a
   setting that changes live, and writes the values changed to <files>/overlay.env, which the startup screen imports into
   its own settings (so restart-only ones take effect at the next start). */
typedef struct {
    /* live settings */
    void (*apply)(const char *key, const char *value);
    /* back to the startup screen */
    void (*quit)(void);
    /* where overlay.env goes */
    const char *files_dir;
    /* System tab: driver, build */
    const char *info;
    /* a file of the APK's assets/ (malloc'd, the caller frees) */
    void *(*asset)(const char *name, size_t *size);
    /* The field controls (runtime/game_actions.h: the physical button, host/controller_map.h index, of each of the
       GAME_ACTIONS actions, -1 none): set = 0 reads the ones in use, 1 applies and saves new ones. */
    void (*controls)(int8_t *actions, int set);
    /* A bug report (host/report.c): start = 1 begins one, 2 one with a frame capture (the menu closes for it), with
       details for it; out gets its progress for the menu. */
    void (*report)(int start, const char *details, char *out, size_t size);
    /* HUD: the graphics driver and its version, one short line */
    const char *driver;
} overlay_settings;

/* Counters for the HUD's shader activity: pipelines requested so far (built on the translation thread or queued for
   the pipeline thread), pipelines queued and not yet built, draws skipped for a pipeline not yet built, shaders
   translated at run time (misses of the translated set). */
/* screen_hz: the panel's current rate, 0 unknown */
typedef struct { uint64_t pipelines, pipelines_queued, draws_skipped, shaders_translated; uint32_t screen_hz; } overlay_stats;

void overlay_configure(const overlay_vk *vk, const overlay_settings *settings);
/* Records the overlay into image (in TRANSFER_DST_OPTIMAL before and after) when the menu is open or the HUD is on;
   returns 1 when it drew. fps / frame_ms: the game's current rate, for the readouts. */
int overlay_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h, float fps, float frame_ms, const overlay_stats *stats);
/* image (a swapchain's) is about to be destroyed and its last use has completed: drop the view and framebuffer made for
   it, before a new image can reuse its handle. Under the same serialisation as overlay_record. */
void overlay_forget_image(VkImage image);
/* Prepare mode (the startup screen's "Prepare shaders", nvn_vulkan.c): while active the game stays paused at its first
   frame and the overlay shows a progress panel instead of the menu; no input is taken. fraction 0..1, < 0 unknown. */
void overlay_prepare(int active, const char *stage, const char *detail, float fraction);
/* the menu is open (the game is paused) */
int overlay_open(void);
/* the menu is open: keep presenting while the game is paused */
int overlay_wants_present(void);
void overlay_toggle(void);
/* Input from the app's input thread: Switch-order buttons (bit 0 A, 1 B, 2 X, 3 Y, 6 L, 7 R, 10 +, 11 -, 12-15 d-pad
   left/up/right/down), left stick (-32767..32767); touch in pixels of the presented image (down = 1 while touching). */
void overlay_pad(uint32_t buttons, int32_t lx, int32_t ly);
void overlay_touch(float x, float y, int down);

#ifdef __cplusplus
}
#endif
