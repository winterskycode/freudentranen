/* Android app host (libtotk.so in GameActivity, a NativeActivity): the window, the built-in controller and motion
   sensors, logging, and the game thread that runs MacNativeGames' loader main().

   Settings come from the launcher as <files>/launch.env (VAR=value lines, applied with setenv before the game starts);
   paths default to the layout push_data.sh creates under /sdcard/tok. */
#include "game_version.h"
/* The field-controls remap and the button prompts are Tears of the Kingdom patches (game_patches_v143.c); a Breath of the
   Wild build (tools/build_botw_apk.sh) has neither, so the launcher's settings for them do nothing there. */
void runtime_actions_set(const uint64_t *table) __attribute__((weak));
void runtime_prompts_set(const uint8_t *field, int swap_faces, const uint16_t *text) __attribute__((weak));
#include "runtime.h"
#include "player_files.h"
#include "controller_map.h"
#include "game_actions.h"
#include "../vk/frame_rule.h"
#include "totk_timeline.h"
#include "world_survey.h"
#include <malloc.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "totk"
void android_vi_set_window(void *window);
void *android_vi_override(const char *name);
int main(int argc, char **argv);

/* ---- logging: stdout/stderr -> logcat and <profile>/totk.log ---- */
static int log_file = -1;
static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg; char buffer[4096], line[4096]; size_t n = 0;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof buffer);
        if (got <= 0) { if (got < 0 && errno == EINTR) continue; break; }
        if (log_file >= 0) write(log_file, buffer, (size_t)got);
        for (ssize_t i = 0; i < got; i++) {
            if (buffer[i] == '\n' || n == sizeof line - 1) { line[n] = 0; __android_log_write(ANDROID_LOG_INFO, TAG, line); n = 0; }
            if (buffer[i] != '\n') line[n++] = buffer[i];
        }
    }
    return NULL;
}
static void start_logging(const char *profile) {
    char path[1024]; snprintf(path, sizeof path, "%s/totk.log", profile);
    log_file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int p[2]; if (pipe(p)) return;
    dup2(p[1], 1); dup2(p[1], 2); close(p[1]);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t; pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]); pthread_detach(t);
}

static ANativeActivity *the_activity;
/* A file of the APK's assets/ (settings.tsv, fonts/), NUL-terminated in malloc'd memory the caller owns; NULL if absent. */
void *dm_android_asset(const char *name, size_t *size) {
    AAsset *a = the_activity ? AAssetManager_open(the_activity->assetManager, name, AASSET_MODE_BUFFER) : NULL;
    if (!a) return NULL;
    size_t n = (size_t)AAsset_getLength(a); char *p = malloc(n + 1);
    if (p && AAsset_read(a, p, n) == (int)n) { p[n] = 0; if (size) *size = n; } else { free(p); p = NULL; }
    AAsset_close(a); return p;
}

/* ---- settings ---- */
static void load_env(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0; setenv(line, eq + 1, 1);
        fprintf(stderr, "[android] setting %s=%s\n", line, eq + 1);
    }
    fclose(f);
}
static void default_env(const char *name, const char *value) { if (!getenv(name)) setenv(name, value, 1); }
/* The frame rate settings as the backend's (vk/frame_rule.h), before debug.env, whose switches win. */
static void frame_settings(void) {
    char hz[16], framegen[32]; frame_rule(getenv("TOTK_FPS_CAP"), getenv("TOTK_FG"), getenv("TOTK_FG_MODE"), hz, framegen);
    setenv("TOTK_UNCAPPED_HZ", hz, 1); setenv("TOTK_FRAMEGEN", framegen, 1);
}

/* ---- translated-shader cache ----
   The on-device translator (libtotk_xlate.so) writes <key>.spv and its companions into the shader cache, and nothing there
   records which translator made them. SHADER_CACHE_REVISION goes up with a translator change that alters existing
   translations; a cache whose xlate-revision.txt is older loses the affected entries (all files of the key) and the game
   translates them again when it meets them. Revision 2: the gather coordinate bias (Program.cs
   QueryHostGatherBiasPrecision), so only translations with a gather go. */
#define SHADER_CACHE_REVISION 2
static int spirv_has_gather(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = bytes >= 20 ? malloc((size_t)bytes) : NULL; size_t n = w ? fread(w, 4, (size_t)bytes / 4, f) : 0; fclose(f);
    int found = 0;
    for (size_t i = 5; i < n && !found;) { uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
        /* OpImageGather, OpImageDrefGather */
        found = op == 96 || op == 97;
        if (!count) break; i += count; }
    free(w); return found;
}
static int key_compare(const void *a, const void *b) { return memcmp(a, b, 16); }
static void shader_cache_revision(const char *dir) {
    char path[1100]; snprintf(path, sizeof path, "%s/xlate-revision.txt", dir);
    FILE *f = fopen(path, "r"); int have = 0; if (f) { if (fscanf(f, "%d", &have) != 1) have = 0; fclose(f); }
    if (have >= SHADER_CACHE_REVISION) return;
    DIR *d = opendir(dir); if (!d) return;
    /* the variants to drop (<key16>.spv with a gather), then every file of theirs: .spv .meta .deps .code .res.json _v.json
       .early (a variant whose .deps is gone is translated again on demand, nvn_vulkan_totk_shader.inc) */
    char (*keys)[16] = NULL; size_t nkeys = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 20 || strcmp(e->d_name + 16, ".spv")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (have < 2 && spirv_has_gather(path)) {
            if (nkeys == cap) { cap = cap ? 2 * cap : 256; keys = realloc(keys, cap * 16); if (!keys) break; }
            memcpy(keys[nkeys++], e->d_name, 16); } }
    /* each variant's .deps goes first (a variant without one is not in the cache), then its other files, named in a
       second pass and removed after it: a directory read while its entries are removed may skip some, and a .deps left
       without its .meta stops the game ("Missing Vulkan shader") */
    char (*names)[64] = NULL; size_t nnames = 0, ncap = 0;
    if (keys && nkeys) { qsort(keys, nkeys, 16, key_compare);
        for (size_t i = 0; i < nkeys; i++) { snprintf(path, sizeof path, "%s/%.16s.deps", dir, keys[i]); unlink(path); }
        rewinddir(d);
        while ((e = readdir(d))) if (strlen(e->d_name) > 16 && strlen(e->d_name) < 64 && (e->d_name[16] == '.' || e->d_name[16] == '_') && bsearch(e->d_name, keys, nkeys, 16, key_compare)) {
            if (nnames == ncap) { ncap = ncap ? 2 * ncap : 1024; names = realloc(names, ncap * 64); if (!names) break; }
            snprintf(names[nnames++], 64, "%s", e->d_name); } }
    closedir(d); free(keys);
    for (size_t i = 0; names && i < nnames; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); unlink(path); }
    free(names);
    snprintf(path, sizeof path, "%s/xlate-revision.txt", dir); f = fopen(path, "w"); if (f) { fprintf(f, "%d\n", SHADER_CACHE_REVISION); fclose(f); }
    fprintf(stderr, "[android] shader cache %s: revision %d -> %d, %zu translations dropped\n", dir, have, SHADER_CACHE_REVISION, nkeys);
}

/* ---- the game thread ---- */
static char data_root[512], rewritten_dir[600];
static void *game_thread(void *arg) {
    (void)arg;
    char *argv[] = {"totk", rewritten_dir, "boot", NULL};
    fprintf(stderr, "[android] game thread: %s boot\n", rewritten_dir);
    int r = main(3, argv);
    fprintf(stderr, "[android] game main returned %d\n", r);
    exit(r);
    return NULL;
}
static void start_game(ANativeActivity *activity) {
    static int started; if (started) return; started = 1;
    char profile[600], env[700], path[700];
    snprintf(profile, sizeof profile, "%s/profile", activity->internalDataPath);
    mkdir(activity->internalDataPath, 0700); mkdir(profile, 0700);
    start_logging(profile);
    snprintf(env, sizeof env, "%s/launch.env", activity->internalDataPath);
    load_env(env);
    frame_settings();
    /* developer switches (adb run-as), kept across launches */
    snprintf(env, sizeof env, "%s/debug.env", activity->internalDataPath); load_env(env);
    /* TOTK_MALLOC_DECAY=1 makes the allocator keep freed memory mapped for a while (Android's default releases it at
       once). Large snapshots and staging copies are mapped afresh each time, 760 MB of churn and the matching page
       faults in 50 s of the Zora Domain scene, but an A/B shows no change in the hitches there: it stays at Android's
       default unless set. */
    { const char *d = getenv("TOTK_MALLOC_DECAY"); if (d) mallopt(M_DECAY_TIME, atoi(d)); }
    /* a freeze-frame flag (frz.sh on) left by a run that died while frozen would freeze this launch's first frame (a black screen) */
    snprintf(env, sizeof env, "%s/freeze", activity->internalDataPath); unlink(env);
    const char *v = getenv("TOTK_VERSION_DIR") ? getenv("TOTK_VERSION_DIR") : TOTK_GAME_DIR;
    snprintf(data_root, sizeof data_root, "%s/%s", getenv("TOTK_DATA_ROOT") ? getenv("TOTK_DATA_ROOT") : "/sdcard/tok", v);
    snprintf(rewritten_dir, sizeof rewritten_dir, "%s/rewritten", data_root);
    /* From the player's own files nothing the PC made is read: the index the game writes is private, no overlays are loaded,
       and the folder of the HeapDef overlays is one that does not exist, so that the host makes the file (host/prompts_romfs.c) */
    if (player_on && player_on()) {
        snprintf(path, sizeof path, "%s/player-romfs", activity->internalDataPath); default_env("TOTK_ROMFS", path);
        default_env("TOTK_ROMFS_PATCHES", "0");
        snprintf(path, sizeof path, "%s/no-romfs-patches-out", activity->internalDataPath); default_env("TOTK_ROMFS_PATCHES_OUT", path);
    }
    snprintf(path, sizeof path, "%s/romfs", data_root); default_env("TOTK_ROMFS", path);
    snprintf(path, sizeof path, "%s/romfs-patches", data_root); default_env("TOTK_ROMFS_PATCHES", path);
    snprintf(path, sizeof path, "%s/romfs-patches-out", data_root); default_env("TOTK_ROMFS_PATCHES_OUT", path);
    /* tables made from the game files (tools/gen_runtime.py) */
    snprintf(path, sizeof path, "%s/game-data", data_root); default_env("TOTK_GAME_DATA", path);
    /* Breath of the Wild: the add-on content volumes <index>/ (runtime/aoc_romfs.c) */
    snprintf(path, sizeof path, "%s/aoc", data_root); default_env("TOTK_AOC", path);
    /* the in-game menu's overlay.env */
    default_env("TOTK_FILES_DIR", activity->internalDataPath);
    { void totk_pgo_start(void); totk_pgo_start(); }
    /* the field controls (actions_parse; the launcher and the in-game menu write it) */
    snprintf(path, sizeof path, "%s/actions.txt", activity->internalDataPath); default_env("TOTK_CONTROLS", path);
    default_env("TOTK_SHARED_FONTS", "/sdcard/tok/fonts");
    default_env("TOTK_PROFILE_DIR", profile);
    default_env("TOTK_OPERATION_MODE", "handheld");
    /* the APK's native library folder: adrenotools hooks for a custom GPU driver */
    { Dl_info info; if (dladdr((void *)default_env, &info) && info.dli_fname) {
        snprintf(path, sizeof path, "%s", info.dli_fname); char *slash = strrchr(path, '/'); if (slash) { *slash = 0; default_env("DM_ANDROID_LIBDIR", path); } } }
    /* The GPU driver (the launcher's GPU driver page, TOTK_GPU_DRIVER): the bundled Turnip (Mesa) when the APK carries it and
       the GPU is an Adreno (Qualcomm's kernel driver, /dev/kgsl-3d0), the system's driver, or one the player installed from
       a zip (TOTK_GPU_DRIVER=zip: launch.env names its folder, library and shader cache). Turnip's shaders run 128 wide, so
       they have their own translation cache with a 128 subgroup profile; Qualcomm's driver is fixed at 64. */
    { const char *libdir = getenv("DM_ANDROID_LIBDIR"), *choice = getenv("TOTK_GPU_DRIVER"); char lib[1100];
      if (libdir && (!choice || !strcmp(choice, "turnip")) && !access("/dev/kgsl-3d0", F_OK)
          && snprintf(lib, sizeof lib, "%s/libvulkan_freedreno.so", libdir) < (int)sizeof lib && !access(lib, F_OK)) {
          default_env("DM_VK_DRIVER_DIR", libdir); default_env("DM_VK_DRIVER_NAME", "libvulkan_freedreno.so");
          snprintf(path, sizeof path, "%s/shader-cache-turnip", activity->internalDataPath); mkdir(path, 0700); default_env("DM_VK_SHADERS", path);
          snprintf(lib, sizeof lib, "%s/host-subgroup-size.txt", path);
          if (access(lib, F_OK)) { FILE *f = fopen(lib, "w"); if (f) { fputs("128\n", f); fclose(f); } } } }
    snprintf(path, sizeof path, "%s/shader-cache", activity->internalDataPath); default_env("DM_VK_SHADERS", path);
    /* both drivers' caches: translations from an older translator */
    shader_cache_revision(path);
    snprintf(path, sizeof path, "%s/shader-cache-turnip", activity->internalDataPath); shader_cache_revision(path);
    /* Compiled pipelines survive the session: the backend's VkPipelineCache file and the list of pipelines it rebuilds
       while the next session loads (nvn_vulkan_pipeline_cache.inc, nvn_vulkan_warm.inc; files named per device and
   driver), and Mesa's own shader cache for Turnip. Without them every pipeline compiles again in each session. */
    snprintf(path, sizeof path, "%s/pipeline-cache", activity->internalDataPath); mkdir(path, 0700); default_env("DM_VK_PIPELINE_CACHE_DIR", path);
    snprintf(path, sizeof path, "%s/mesa-cache", activity->internalDataPath); mkdir(path, 0700); default_env("MESA_SHADER_CACHE_DIR", path);
    /* Mesa keeps it off on Android by default: the pipeline libraries the prepare step builds live there
       (vk/nvn_vulkan_gpl.inc) */
    default_env("MESA_SHADER_CACHE_DISABLE", "false");
    /* translation thread: the game's submit call returns before the replay */
    default_env("TOTK_ASYNC_REPLAY", "1");
    /* pace by the 120 Hz cap, not the game's present interval (it chooses 3 = 25 ms in the cave: 40 -> 51 fps); the
       real-time frame step keeps game speed */
    default_env("TOTK_UNCAPPED_INTERVAL", "1");
    /* memory pools of 64 KiB and up are exported Vulkan memory (dma-buf over the guest pages; Turnip) */
    default_env("TOTK_VK_ZEROCOPY", "1");
    /* draws read those pools in place instead of snapshots */
    default_env("TOTK_VK_ZEROCOPY_BIND", "1");
    /* uniforms with pending GPU writes are assembled on the GPU, no completion wait */
    default_env("TOTK_VK_UBO_COMPOSE", "1");
    /* the panel is 1920x1080: a 2560x1440 output costs ~5 ms of GPU a frame */
    default_env("TOTK_OUTPUT_RES", "0");
    /* for the translator's .NET runtime */
    snprintf(path, sizeof path, "%s/tmp", activity->internalDataPath); mkdir(path, 0700); default_env("TMPDIR", path);
    /* runtime_front_run: the Android UI thread is the activity's; the game owns its thread */
    default_env("TOTK_HEADLESS", "1");
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 16u << 20);
    pthread_t t; pthread_create(&t, &a, game_thread, NULL); pthread_attr_destroy(&a); pthread_detach(t);
}

/* ---- front end (runtime.h) ---- */
static _Atomic int focused = 1;
void runtime_front_run(void (*callback)(void)) { callback(); }
int runtime_front_focus(void) { return atomic_load(&focused); }
void *runtime_front_override(const char *name) { return android_vi_override(name); }
void runtime_front_request_close(void) { fprintf(stderr, "[android] close requested\n"); exit(0); }
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_boot_status(const char *stage, const char *detail) { fprintf(stderr, "[boot] %s: %s\n", stage, detail); }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static uint64_t last;
    if (done == total || done - last >= total / 20 || done < last) { last = done; fprintf(stderr, "[boot] %s %llu/%llu\n", stage, (unsigned long long)done, (unsigned long long)total); }
}
void runtime_gpu_path_probe(const char *which) { fprintf(stderr, "[android] GPU probe %s needs the GPU backend\n", which); exit(2); }
void runtime_vpool_probe(void) { fprintf(stderr, "[android] vpool probe needs the GPU backend\n"); exit(2); }

/* ---- controller: physical buttons (controller_map.h names) -> Switch buttons, and the field actions on them ----
   The buttons press the Switch buttons of the Switch layout (controller_map_default; which key is which face button
   follows TOTK_FACE_BUTTONS), so menus confirm and go back where the game puts them. What the buttons do in the field
   is chosen by action (TOTK_CONTROLS, one "action = physical button" line per change, game_actions.h names): the game's
   input routine takes its action bits from tables then (game_patches_v143.c patch_actions). */
static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState pad_state;
static uint64_t pressed_edges, physical;
static uint16_t controller_map[CONTROLLER_BUTTONS];
/* the physical button (controller_map.h index) of each action, -1 none */
static int8_t action_buttons[GAME_ACTIONS];
static float axis_lx, axis_ly, axis_rx, axis_ry, axis_lt, axis_rt, hat_x, hat_y;
static int pad_connected, face_by_label;
/* TOTK_BUTTON_STYLE: the letters the prompts show for a face button, by its place on the device: xbox (the printed ones on
   an Xbox-layout device such as the Odin 3: bottom A, right B, left X, top Y), switch (right A, bottom B, top X, left Y) or
   playstation: the Switch's letters, drawn as Circle, Cross, Triangle and Square by the icons the startup screen merges in
   for it (built-in mod _buttons-playstation, IconMod.java). Only the letters change, never which button a
   prompt means. */
static int style_switch, style_playstation;
/* The prompts show each Switch button's letter swapped (A <-> B, X <-> Y): the letter at the place of the key that presses
   it differs from the Switch's own. By position the keys sit at the Switch layout's places; by label the printed A
   (bottom) presses the Switch's A. */
static int faces_swapped(void) { return style_switch ? face_by_label : !face_by_label; }
/* load_controller_map done (dm_android_prompt_places waits for it) */
static _Atomic int controls_loaded;

static int32_t stick_axis(float value) {
    const float deadzone = 0.12f; float magnitude = fabsf(value);
    if (magnitude <= deadzone) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}
/* The physical button that presses Switch button b in the Switch layout (east A, south B, north X, west Y, the rest by
   index), and back */
static int physical_of_switch(int b) { return game_physical_of_switch(b); }
/* the swap is its own inverse */
static int switch_of_physical(int p) { return p < 0 || p >= CONTROLLER_SWITCH_BUTTONS ? -1 : game_physical_of_switch(p); }
static void actions_default(int8_t phys[GAME_ACTIONS]) { for (int i = 0; i < GAME_ACTIONS; i++) phys[i] = (int8_t)physical_of_switch(game_actions[i].button); }
/* The button prompts for these controls: the field guides and text icons show the button each action is on, and the
   face letters of the button style at the keys' places (faces_swapped) */
/* buttons: Switch button per action, NULL = the game's own */
static void prompts_apply(const int8_t *buttons) {
    static uint8_t field[GAME_GLYPHS]; static uint16_t text[64]; int swap = faces_swapped();
    if (buttons) game_actions_glyphs(buttons, field);
    game_text_icon_chars(buttons ? field : NULL, swap, text);
    if (runtime_prompts_set) runtime_prompts_set(buttons ? field : NULL, swap, buttons || swap ? text : NULL);
}
/* Into the game: our tables, or the game's own (its jump swap included) while every action is where it puts it. Called
   from the input thread (a layout change) and the overlay's thread (the in-game Controls page): one at a time, since
   the tables are built in static buffers. */
static void actions_apply_locked(const int8_t phys[GAME_ACTIONS]) {
    int8_t def[GAME_ACTIONS], buttons[GAME_ACTIONS]; actions_default(def);
    if (!runtime_actions_set) { prompts_apply(buttons); return; }
    if (!memcmp(def, phys, sizeof def)) { runtime_actions_set(NULL); prompts_apply(NULL); return; }
    for (int i = 0; i < GAME_ACTIONS; i++) buttons[i] = (int8_t)switch_of_physical(phys[i]);
    static uint64_t table[GAME_ACTION_TABLE]; int bad = game_actions_table(buttons, table);
    if (bad) { fprintf(stderr, "[totk-input] controls: %s cannot go on %s (kept where the game puts it)\n", game_actions[bad - 1].name, controller_physical_names[phys[bad - 1]]);
        int8_t fixed[GAME_ACTIONS]; memcpy(fixed, phys, sizeof fixed); fixed[bad - 1] = def[bad - 1]; actions_apply_locked(fixed); return; }
    runtime_actions_set(table); prompts_apply(buttons);
}
static void actions_apply(const int8_t phys[GAME_ACTIONS]) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock); actions_apply_locked(phys); pthread_mutex_unlock(&lock);
}
/* "action = physical button|none" lines ('#' comments); 0, or the 1-based line of the first one not understood */
static int actions_parse(const char *text, int8_t phys[GAME_ACTIONS]) {
    actions_default(phys); int line = 1;
    for (const char *p = text; *p; line++) {
        const char *end = strchr(p, '\n'); if (!end) end = p + strlen(p);
        const char *hash = memchr(p, '#', (size_t)(end - p)), *stop = hash ? hash : end, *eq = memchr(p, '=', (size_t)(stop - p));
        const char *a0 = p; while (a0 < stop && (*a0 == ' ' || *a0 == '\t' || *a0 == '\r')) a0++;
        if (a0 < stop) {
            if (!eq) return line;
            const char *a1 = eq; while (a1 > a0 && (a1[-1] == ' ' || a1[-1] == '\t')) a1--;
            const char *b0 = eq + 1; while (b0 < stop && (*b0 == ' ' || *b0 == '\t')) b0++;
            const char *b1 = stop; while (b1 > b0 && (b1[-1] == ' ' || b1[-1] == '\t' || b1[-1] == '\r')) b1--;
            /* a Switch button only: the touchpad (the last physical name) carries no action, and the launcher shows none on it */
            int a = game_action_index(a0, (size_t)(a1 - a0)), b = controller_name_index(controller_physical_names, CONTROLLER_SWITCH_BUTTONS, b0, (size_t)(b1 - b0));
            if (a < 0 || (b < 0 && !(b1 - b0 == 4 && !strncasecmp(b0, "none", 4)))) return line;
            phys[a] = (int8_t)b;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}
static void load_controller_map(void) {
    uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(controller_map, modes);
    /* by label unless asked: what is printed on the key */
    const char *e = getenv("TOTK_FACE_BUTTONS"); face_by_label = !e || strcmp(e, "position");
    e = getenv("TOTK_BUTTON_STYLE"); style_playstation = e && !strcmp(e, "playstation"); style_switch = style_playstation || (e && !strcmp(e, "switch"));
    actions_default(action_buttons);
    const char *path = getenv("TOTK_CONTROLS"); FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (f) {
        char text[8192]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
        int line = actions_parse(text, action_buttons);
        if (line) { fprintf(stderr, "[totk-input] %s line %d not understood: the game's own controls\n", path, line); actions_default(action_buttons); }
    }
    actions_apply(action_buttons);
    fprintf(stderr, "[totk-input] face buttons by %s, %s button letters, field controls %s\n", face_by_label ? "label" : "position",
            style_playstation ? "PlayStation" : style_switch ? "Switch" : "Xbox", f ? path : "the game's own");
    atomic_store(&controls_loaded, 1);
}
/* The command bubble and the HUD's ability key (host/prompts_romfs.c): per diamond of the bubble (A talk / grab...,
   B dash, X jump, Y attacks) the place of the key that does its action on this device (an Xbox-style pad: key A bottom,
   B right, X left, Y top; 0 right, 1 bottom, 2 top, 3 left), the button-font character of the abilities' key, and the
   letter the button style shows at each place. 0 (the art as it is) unless the four are on four different face buttons.
   Read when the game opens those files (at its start: a change of the controls shows from the next start). */
int dm_android_prompt_glyphs(void) { return style_playstation; }
int dm_android_prompt_places(int8_t places[4], uint16_t *ability_char, char letters[4]) {
    /* the input thread reads the controls */
    for (int i = 0; i < 500 && !atomic_load(&controls_loaded); i++) usleep(10000);
    /* the diamonds A, B, X, Y */
    static const int8_t group_action[4] = {GA_INTERACT, GA_SPRINT, GA_JUMP, GA_ATTACK};
    /* key A bottom, B right, X left, Y top */
    static const int8_t key_place[4] = {1, 0, 3, 2};
    pthread_mutex_lock(&pad_lock); int8_t phys[GAME_ACTIONS]; memcpy(phys, action_buttons, sizeof phys); int label = face_by_label, swap = faces_swapped(); pthread_mutex_unlock(&pad_lock);
    /* right, bottom, top, left */
    memcpy(letters, style_switch ? "ABXY" : "BAYX", 4);
    int used = 0;
    for (int g = 0; g < 4; g++) {
        int b = switch_of_physical(phys[group_action[g]]);
        if (b < 0 || b > 3) return 0;
        /* by position the keys press the Switch layout's (A <-> B, X <-> Y) */
        int key = label ? b : (b ^ 1);
        if (used & (1 << key)) return 0;
        used |= 1 << key; places[g] = key_place[key];
    }
    int a = switch_of_physical(phys[GA_ABILITY]);
    *ability_char = a >= 0 && a < 16 ? game_glyph_char[game_glyph_printed(game_glyph_of_button[a], swap)] : 0;
    return 1;
}
/* The controls pages (vk/overlay.h controls): the actions' buttons in use, or new ones, saved to TOTK_CONTROLS and
   given to the game at once */
void dm_android_controls(int8_t phys[GAME_ACTIONS], int set) {
    if (!set) { pthread_mutex_lock(&pad_lock); memcpy(phys, action_buttons, sizeof action_buttons); pthread_mutex_unlock(&pad_lock); return; }
    const char *path = getenv("TOTK_CONTROLS"); char tmp[1100];
    int8_t def[GAME_ACTIONS]; actions_default(def);
    if (path && *path && snprintf(tmp, sizeof tmp, "%s.tmp", path) < (int)sizeof tmp) {
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "# field controls: action = physical button (game_actions.h); actions not listed are where the game puts them\n");
            for (int i = 0; i < GAME_ACTIONS; i++) if (phys[i] != def[i] && game_actions[i].label)
                fprintf(f, "%s = %s\n", game_actions[i].name, phys[i] < 0 ? "none" : controller_physical_names[phys[i]]);
            fclose(f); rename(tmp, path);
        }
    }
    pthread_mutex_lock(&pad_lock); memcpy(action_buttons, phys, sizeof action_buttons); pthread_mutex_unlock(&pad_lock);
    actions_apply(phys);
}
static _Atomic int pending_face = -1;
/* focus lost (UI thread): the input thread lets go of every button and stick */
static _Atomic int pending_unfocus;
/* TOTK_FACE_BUTTONS from the in-game menu: which Android key is which physical face button. */
void dm_android_face_buttons(int by_label) { atomic_store(&pending_face, by_label != 0); }
/* input thread */
static void take_pending_layout(void) {
    int face = atomic_exchange(&pending_face, -1);
    if (face >= 0 && face != face_by_label) { face_by_label = face; physical &= ~0xfull; fprintf(stderr, "[totk-input] face buttons by %s\n", face ? "label" : "position");
        pthread_mutex_lock(&pad_lock); int8_t phys[GAME_ACTIONS]; memcpy(phys, action_buttons, sizeof phys); pthread_mutex_unlock(&pad_lock);
        /* the prompts' printed letters */
        actions_apply(phys); }
}
/* ---- the in-game overlay (vk/overlay.h): the system's Back key, or a tap on the top-right corner ----
   Neither is a button the game uses. While the overlay is open the game sees a neutral controller. */
extern void overlay_pad(uint32_t buttons, int32_t lx, int32_t ly) __attribute__((weak));
extern void overlay_touch(float x, float y, int down) __attribute__((weak));
static uint16_t script_buttons(void);
extern void overlay_toggle(void) __attribute__((weak));
extern int overlay_open(void) __attribute__((weak));
/* the game window's width (on_window_created): the overlay's touch corner */
static int window_width = 1920;
/* input thread: after every input event and at least every 20 ms: the overlay's controller */
static void pad_tick(void) {
    pthread_mutex_lock(&pad_lock);
    /* input.now presses reach the menu too */
    uint64_t nav = pad_state.buttons | script_buttons(); int32_t lx = pad_state.lx, ly = pad_state.ly;
    /* the in-game menu follows the system's face buttons (BUTTON_A confirms): with the game's face buttons by position
       the keys press the Switch button at their place, A <-> B and X <-> Y from the printed ones */
    if (!face_by_label) nav = (nav & ~0xfull) | (nav & 1) << 1 | (nav & 2) >> 1 | (nav & 4) << 1 | (nav & 8) >> 1;
    pthread_mutex_unlock(&pad_lock);
    if (overlay_pad) overlay_pad((uint32_t)nav, lx, ly);
}
/* Recompute the Switch state from the physical state (input thread). */
static void refresh_pad(void) {
    take_pending_layout();
    if (atomic_exchange(&pending_unfocus, 0)) { physical = 0; axis_lx = axis_ly = axis_rx = axis_ry = axis_lt = axis_rt = hat_x = hat_y = 0; }
    RuntimeGamepadState next = {0};
    next.connected = pad_connected;
    uint64_t phys = physical;
    if (axis_lt > 0.5f) phys |= 1ULL << 8;
    if (axis_rt > 0.5f) phys |= 1ULL << 9;
    if (hat_x < -0.5f) phys |= 1ULL << 12;
    if (hat_y < -0.5f) phys |= 1ULL << 13;
    if (hat_x > 0.5f) phys |= 1ULL << 14;
    if (hat_y > 0.5f) phys |= 1ULL << 15;
    /* Android: y grows downward */
    next.lx = stick_axis(axis_lx); next.ly = stick_axis(-axis_ly);
    next.rx = stick_axis(axis_rx); next.ry = stick_axis(-axis_ry);
    next.buttons = controller_map_apply(controller_map, phys);
    pthread_mutex_lock(&pad_lock);
    pressed_edges |= next.buttons & ~pad_state.buttons;
    pad_state = next;
    pthread_mutex_unlock(&pad_lock);    pad_tick();
}
/* Scripted input for unattended testing (adb key events never reach the game):
   - <files>/input.script: lines "<seconds> <button> [hold ms]" (controller_switch_names: a, b, x, y, plus, up...), timed
     from the game's first controller read; e.g. "20 a" presses A (Continue) on the title screen.
   - <files>/input.now: buttons pressed right away (one per line, "<button> [hold ms]"), the file is then removed; the line
     "menu" opens or closes the in-game menu, "quit" is the menu's Quit to startup screen, "report [capture]" saves a bug
     report (with a frame capture), "survey ..." goes to runtime/world_survey.c (TOTK_SURVEY=1 sessions: the benchmark
     route, tools/bench.py). */
static struct { double at; uint16_t bit; unsigned hold_ms; } script[64];
static unsigned script_n, script_next;
static uint64_t script_start, script_release[16];
static char script_dir[1024];
static uint16_t script_bit(const char *name) {
    for (int i = 0; i < CONTROLLER_SWITCH_BUTTONS; i++) if (!strcasecmp(name, controller_switch_names[i])) return (uint16_t)(1u << i);
    return 0;
}
static void script_press(uint16_t bit, unsigned hold_ms, uint64_t now) {
    for (int i = 0; i < 16; i++) if (bit & (1u << i)) script_release[i] = now + (uint64_t)(hold_ms ? hold_ms : 150) * 1000000ULL;
}
/* input.now / input.script stick lines: "lx|ly|rx|ry <value -32767..32767> [ms, default 500]" hold that stick axis. */
static struct { int32_t value; uint64_t until; } script_axis[4];
static int script_axis_index(const char *name) { return !strcmp(name, "lx") ? 0 : !strcmp(name, "ly") ? 1 : !strcmp(name, "rx") ? 2 : !strcmp(name, "ry") ? 3 : -1; }
static uint16_t script_buttons(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    if (!script_start) {
        script_start = now; const char *dir = getenv("DM_VK_SHADERS");
        if (dir) { snprintf(script_dir, sizeof script_dir, "%s", dir); char *slash = strrchr(script_dir, '/'); if (slash) *slash = 0; }
        char path[1100]; snprintf(path, sizeof path, "%s/input.script", script_dir); FILE *f = script_dir[0] ? fopen(path, "r") : NULL;
        if (f) { char line[128];
            while (script_n < 64 && fgets(line, sizeof line, f)) { double at; char name[32]; unsigned hold = 0;
                if (sscanf(line, "%lf %31s %u", &at, name, &hold) >= 2 && script_bit(name)) script[script_n++] = (typeof(script[0])){at, script_bit(name), hold}; }
            fclose(f); fprintf(stderr, "[totk-input] input.script: %u presses\n", script_n); }
    }
    while (script_next < script_n && now - script_start >= (uint64_t)(script[script_next].at * 1e9)) {
        script_press(script[script_next].bit, script[script_next].hold_ms, now);
        fprintf(stderr, "[totk-input] scripted press %#x at %.1f s\n", script[script_next].bit, (now - script_start) / 1e9); script_next++; }
    uint16_t held = 0; for (int i = 0; i < 16; i++) if (now < script_release[i]) held |= (uint16_t)(1u << i);
    return held;
}
void dm_android_quit(void);
void dm_android_report(int start, const char *details, char *out, size_t size);
/* input.now, polled on the input thread (the game reads no input while its menu is open, and "menu" must still close
   it). The presses and stick holds it sets are under pad_lock, which the game's read takes. */
void totk_tl(int kind, uint64_t a, uint64_t b) __attribute__((weak));
void runtime_diag_gfx(const char *line) __attribute__((weak));
static void script_poll_now(void) {
    static uint64_t polled; static char path[1100];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    if (now - polled < 250000000ULL) return;
    polled = now;
    if (!path[0]) {
        const char *dir = getenv("DM_VK_SHADERS"); if (!dir) return;
        snprintf(path, sizeof path, "%s", dir); char *slash = strrchr(path, '/'); if (slash) *slash = 0;
        strncat(path, "/input.now", sizeof path - strlen(path) - 1);
    }
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1200]; int toggle = 0, quit = 0, report = 0, kbtest = 0;
    /* "survey ..." lines (runtime/world_survey.c: benchmark route, positions), run after the lock */
    char survey[4][1200]; int nsurvey = 0;
    pthread_mutex_lock(&pad_lock);
    while (fgets(line, sizeof line, f)) { char name[32]; unsigned hold = 0; int value = 0, ms = 500;
        if (sscanf(line, "%31s %d %d", name, &value, &ms) >= 2 && script_axis_index(name) >= 0) {
            int a = script_axis_index(name); script_axis[a].value = value; script_axis[a].until = now + (uint64_t)(ms > 0 ? ms : 500) * 1000000ULL;
            /* scripted: when the stick moved */
            if (totk_tl) totk_tl(TL_INPUT, TL_INPUT_SCRIPTED | (unsigned)a, (uint64_t)(int64_t)value);
            fprintf(stderr, "[totk-input] input.now %s %d for %d ms\n", name, value, ms); continue; }
        if (!strncmp(line, "survey ", 7)) { if (nsurvey < 4) { line[strcspn(line, "\n")] = 0; snprintf(survey[nsurvey++], sizeof survey[0], "%s", line); } continue; }
        /* graphics lever hunt (game_patches_v143.c) */
        if (!strncmp(line, "gfx", 3)) { line[strcspn(line, "\n")] = 0; if (runtime_diag_gfx) runtime_diag_gfx(line); continue; }
        if (sscanf(line, "%31s", name) == 1 && !strcmp(name, "menu")) { toggle ^= 1; continue; }
        if (sscanf(line, "%31s", name) == 1 && !strcmp(name, "quit")) { quit = 1; continue; }
        /* the game's keyboard the way it names a horse (swkbd_host.c) */
        if (sscanf(line, "%31s", name) == 1 && !strcmp(name, "kbtest")) { kbtest = 1; continue; }
        if (sscanf(line, "%31s", name) == 1 && !strcmp(name, "report")) { report = strstr(line, "capture") ? 2 : 1; continue; }
        if (sscanf(line, "%31s %u", name, &hold) >= 1 && script_bit(name)) { script_press(script_bit(name), hold, now); fprintf(stderr, "[totk-input] input.now %s\n", name); } }
    pthread_mutex_unlock(&pad_lock);
    fclose(f); unlink(path);
    if (toggle && overlay_toggle) overlay_toggle();
    if (quit) dm_android_quit();
    if (kbtest) runtime_swkbd_selftest();
    for (int i = 0; i < nsurvey; i++) if (!runtime_survey_command(survey[i])) fprintf(stderr, "[totk-input] input.now: %s not taken (TOTK_SURVEY=1 is needed)\n", survey[i]);
    if (report) { char status[300]; dm_android_report(report, "input.now", status, sizeof status); }
}
void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&pad_lock);
    uint16_t scripted = script_buttons();
    *out = pad_state; out->buttons |= pressed_edges | scripted; pressed_edges = 0;
    /* while the menu is open the game sees a neutral controller, and the presses made then are the menu's: dropped, or
       they would reach the game when it reads again */
    if (overlay_open && overlay_open()) { int c = out->connected; memset(out, 0, sizeof *out); out->connected = c; pressed_edges = 0; pthread_mutex_unlock(&pad_lock); return; }
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
      int32_t *axes[4] = {&out->lx, &out->ly, &out->rx, &out->ry};
      for (int i = 0; i < 4; i++) if (now < script_axis[i].until) *axes[i] = script_axis[i].value; }
    pthread_mutex_unlock(&pad_lock);
    /* input-to-screen latency: the game read the controller */
    if (totk_tl) totk_tl(TL_INPUT, out->buttons, (uint64_t)(int64_t)out->rx);
}
void runtime_gamepad_clear(void) {
    pthread_mutex_lock(&pad_lock); pad_state = (RuntimeGamepadState){0}; pressed_edges = 0; pthread_mutex_unlock(&pad_lock);
}
/* Rumble: the game sets two motors (left, right) with low/high frequency bands; the Odin 3 has one motor, driven with
   the strongest amplitude. A thread attached to the Java VM forwards changes (at most every 16 ms) to
   GameActivity.rumble, which re-arms a 40 ms pulse while the value stays above zero. While nothing rumbles the thread
   sleeps until a change (rumble_wake) or 500 ms, when it reads the screen rotation. */
static _Atomic float rumble_amp[2];
/* Surface.ROTATION_* (0..3), from GameActivity.displayRotation */
static _Atomic int display_rotation;
static pthread_mutex_t rumble_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t rumble_wake = PTHREAD_COND_INITIALIZER;
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)freq_low; (void)freq_high;
    if (side >= 2) return;
    float v = amp_low > amp_high ? amp_low : amp_high;
    if (atomic_exchange(&rumble_amp[side], v) != v) { pthread_mutex_lock(&rumble_lock); pthread_cond_signal(&rumble_wake); pthread_mutex_unlock(&rumble_lock); }
}
#include <jni.h>
static JavaVM *java_vm;
static jclass game_class;
static void *rumble_thread(void *arg) {
    (void)arg; JNIEnv *env;
    if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL)) return NULL;
    jmethodID rumble = (*env)->GetStaticMethodID(env, game_class, "rumble", "(F)V");
    if (!rumble) { (*env)->ExceptionClear(env); (*java_vm)->DetachCurrentThread(java_vm); return NULL; }
    jmethodID rotation = (*env)->GetStaticMethodID(env, game_class, "displayRotation", "()I");
    if (!rotation) (*env)->ExceptionClear(env);
    float sent = 0; unsigned quiet = 0, failures = 0; uint64_t rotation_at = 0;
    for (;;) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        /* the screen's rotation, for the motion sensors' axes (on_sensors) */
        if (rotation && now_ns >= rotation_at) {
            rotation_at = now_ns + 500000000ull;
            int r = (*env)->CallStaticIntMethod(env, game_class, rotation);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            else if (r >= 0 && r <= 3 && r != atomic_load(&display_rotation)) { atomic_store(&display_rotation, r); fprintf(stderr, "[totk-input] screen rotation %d (motion axes follow it)\n", r * 90); }
        }
        float a = atomic_load(&rumble_amp[0]), b = atomic_load(&rumble_amp[1]), v = a > b ? a : b;
        if (v > 0.01f || sent > 0.01f) {
            if (fabsf(v - sent) > 0.02f || (v > 0.01f && ++quiet >= 2)) {
                (*env)->CallStaticVoidMethod(env, game_class, rumble, v); sent = v; quiet = 0;
                /* a vibrator that went away (controller unplugged) throws; a pending exception would abort the next call */
                if ((*env)->ExceptionCheck(env)) { if (!failures++) (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
            }
        }
        /* rumbling: every 16 ms (the pulse is re-armed); else until the game changes a motor or the rotation is due */
        uint64_t wait_ns = v > 0.01f || sent > 0.01f ? 16000000ull : rotation_at > now_ns ? rotation_at - now_ns : 500000000ull;
        uint64_t until = now_ns + wait_ns; struct timespec deadline = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
        pthread_mutex_lock(&rumble_lock);
        if (atomic_load(&rumble_amp[0]) == a && atomic_load(&rumble_amp[1]) == b) pthread_cond_clockwait(&rumble_wake, &rumble_lock, CLOCK_MONOTONIC, &deadline);
        pthread_mutex_unlock(&rumble_lock);
    }
    return NULL;
}
/* GameActivity.deviceInfo(): the device's modes, battery and thermal state for a bug report (report.c); malloc'd. */
char *dm_android_device_info(void) {
    if (!java_vm || !game_class) return NULL;
    JNIEnv *env; int attached = 0; char *out = NULL;
    if ((*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) { if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL)) return NULL; attached = 1; }
    jmethodID m = (*env)->GetStaticMethodID(env, game_class, "deviceInfo", "()Ljava/lang/String;");
    jstring text = m ? (jstring)(*env)->CallStaticObjectMethod(env, game_class, m) : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (text) { const char *c = (*env)->GetStringUTFChars(env, text, NULL); if (c) { out = strdup(c); (*env)->ReleaseStringUTFChars(env, text, c); } (*env)->DeleteLocalRef(env, text); }
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return out;
}
/* nn::swkbd text entry (runtime/swkbd_host.c, naming a horse): GameActivity.textInput shows a dialog with the system's
   on-screen keyboard and returns the text, or null when the player cancels. Text crosses as UTF-8 byte arrays (JNI's
   modified UTF-8 has no 4-byte characters). The call runs on a host thread of its own -- the caller is a guest thread,
   not one to attach to the VM -- and the caller waits for it. */
typedef struct { const RuntimeTextRequest *req; char *out; size_t out_size; int confirmed; } text_call;
static jbyteArray utf8_bytes(JNIEnv *env, const char *s) {
    jsize n = s ? (jsize)strlen(s) : 0; jbyteArray a = (*env)->NewByteArray(env, n);
    if (a && n) (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)s);
    return a;
}
static void *text_input_thread(void *arg) {
    text_call *c = arg; JNIEnv *env;
    if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL)) return NULL;
    jmethodID m = (*env)->GetStaticMethodID(env, game_class, "textInput", "([B[BIIZZI)[B");
    if (m) {
        const RuntimeTextRequest *r = c->req;
        jbyteArray prompt = utf8_bytes(env, r->prompt), initial = utf8_bytes(env, r->initial);
        jbyteArray text = (jbyteArray)(*env)->CallStaticObjectMethod(env, game_class, m, prompt, initial, (jint)r->max_units, (jint)r->min_units,
                                                                     (jboolean)(r->secure != 0), (jboolean)(r->digits_only != 0), (jint)r->disabled);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); text = NULL; }
        jbyte *bytes = text ? (*env)->GetByteArrayElements(env, text, NULL) : NULL;
        if (bytes) {
            size_t n = (size_t)(*env)->GetArrayLength(env, text);
            /* cut at a character boundary */
            if (n >= c->out_size) { n = c->out_size - 1; while (n > 0 && ((unsigned char)bytes[n] & 0xc0) == 0x80) n--; }
            memcpy(c->out, bytes, n); c->out[n] = 0; c->confirmed = 1;
            (*env)->ReleaseByteArrayElements(env, text, bytes, JNI_ABORT);
        }
        if (text) (*env)->DeleteLocalRef(env, text);
        (*env)->DeleteLocalRef(env, prompt); (*env)->DeleteLocalRef(env, initial);
    } else if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*java_vm)->DetachCurrentThread(java_vm);
    return NULL;
}
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    if (!out || !out_size) return 0;
    out[0] = 0;
    if (!java_vm || !game_class) return 0;
    text_call c = {req, out, out_size, 0}; pthread_t t;
    if (pthread_create(&t, NULL, text_input_thread, &c)) return 0;
    pthread_join(t, NULL);
    return c.confirmed;
}
static void start_rumble(ANativeActivity *activity) {
    java_vm = activity->vm; JNIEnv *env = activity->env;
    jclass local = (*env)->GetObjectClass(env, activity->clazz);
    game_class = (*env)->NewGlobalRef(env, local);
    pthread_t t; pthread_create(&t, NULL, rumble_thread, NULL); pthread_detach(t);
}

/* Physical button of an Android key code (controller_map.h order), or -1. By position: Android's BUTTON_A is the bottom
   face button (Xbox layout), as the Odin 3 reports in its default mode; TOTK_FACE_BUTTONS=label reads them as Nintendo. */
static int physical_of_key(int32_t key) {
    switch (key) {
    case AKEYCODE_BUTTON_A: return face_by_label ? 1 : 0;
    case AKEYCODE_BUTTON_B: return face_by_label ? 0 : 1;
    case AKEYCODE_BUTTON_X: return face_by_label ? 3 : 2;
    case AKEYCODE_BUTTON_Y: return face_by_label ? 2 : 3;
    case AKEYCODE_BUTTON_THUMBL: return 4;
    case AKEYCODE_BUTTON_THUMBR: return 5;
    case AKEYCODE_BUTTON_L1: return 6;
    case AKEYCODE_BUTTON_R1: return 7;
    case AKEYCODE_BUTTON_L2: return 8;
    case AKEYCODE_BUTTON_R2: return 9;
    case AKEYCODE_BUTTON_START: return 10;
    case AKEYCODE_BUTTON_SELECT: return 11;
    case AKEYCODE_DPAD_LEFT: return 12;
    case AKEYCODE_DPAD_UP: return 13;
    case AKEYCODE_DPAD_RIGHT: return 14;
    case AKEYCODE_DPAD_DOWN: return 15;
    default: return -1;
    }
}
static int handle_input(AInputEvent *e) {
    int32_t source = AInputEvent_getSource(e), type = AInputEvent_getType(e);
    int from_pad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e);
        /* the in-game menu (the system's Back key: the Odin's own, a controller's, or the back gesture) */
        if (key == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN && !AKeyEvent_getRepeatCount(e) && overlay_toggle) overlay_toggle();
            return 1;
        }
        int p = physical_of_key(key);
        /* the home button: kept from the system */
        if (p < 0) return key == AKEYCODE_BUTTON_MODE;
        if (action == AKEY_EVENT_ACTION_DOWN) physical |= 1ULL << p; else if (action == AKEY_EVENT_ACTION_UP) physical &= ~(1ULL << p);
        if (!pad_connected && from_pad) { pad_connected = 1; fprintf(stderr, "[totk-input] controller connected (device %d)\n", AInputEvent_getDeviceId(e)); }
        refresh_pad();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
        float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
        int down = action != AMOTION_EVENT_ACTION_UP && action != AMOTION_EVENT_ACTION_CANCEL;
        if (overlay_open && overlay_open()) { if (overlay_touch) overlay_touch(x, y, down); return 1; }
        if (action == AMOTION_EVENT_ACTION_DOWN && overlay_toggle && x > window_width - 160 && y < 160) { overlay_toggle(); return 1; }
        return 0;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        axis_lx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        axis_ly = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        axis_rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
        axis_ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), brake = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
        float rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0), gas = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        axis_lt = lt > brake ? lt : brake; axis_rt = rt > gas ? rt : gas;
        hat_x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        hat_y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        if (!pad_connected) { pad_connected = 1; fprintf(stderr, "[totk-input] controller connected (device %d)\n", AInputEvent_getDeviceId(e)); }
        refresh_pad();
        return 1;
    }
    return 0;
}

/* ---- input thread: the activity's input queue and the motion sensors on one looper ---- */
enum { LOOPER_INPUT = 1, LOOPER_SENSORS = 2 };
static ALooper *input_looper;
static AInputQueue *input_queue;
static ASensorEventQueue *sensor_queue;
static const ASensor *gyro, *accel;
static double last_acc[3] = {0, 0, 1};
/* TOTK_MOTION=off: the game sees a still controller (the overlay changes it live) */
static volatile int motion_on = -1;
void dm_android_motion(int on) { motion_on = on != 0; }
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

static int on_input(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    pthread_mutex_lock(&queue_lock);
    AInputEvent *e;
    while (input_queue && AInputQueue_getEvent(input_queue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(input_queue, e)) continue;
        AInputQueue_finishEvent(input_queue, e, handle_input(e));
    }
    pthread_mutex_unlock(&queue_lock);
    return 1;
}
static void screen_axes(const float v[3], double out[3]) {
    switch (atomic_load(&display_rotation)) {
    /* ROTATION_90 = the device turned a quarter counter-clockwise: the screen's top edge is the natural +x, its right
       edge the natural -y (remapCoordinateSystem(AXIS_Y, AXIS_MINUS_X)); ROTATION_270 the other way */
    case 1: out[0] = -v[1]; out[1] = v[0]; break;
    case 2: out[0] = -v[0]; out[1] = -v[1]; break;
    case 3: out[0] = v[1]; out[1] = -v[0]; break;
    default: out[0] = v[0]; out[1] = v[1]; break;
    }
    out[2] = v[2];
}
static int on_sensors(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    ASensorEvent ev[16]; ssize_t n;
    while ((n = ASensorEventQueue_getEvents(sensor_queue, ev, 16)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            /* Android reports both sensors in the device's natural orientation (portrait on the Odin 3) and the
               acceleration as the force holding the device up (+1 G on z lying face up). The game wants the
               controller's frame as seen on screen (x right, y toward the top edge, z out of the face) and the
               acceleration as gravity (-1 G on z lying face up; motion_fusion.c): rotate by the screen's rotation as
               SensorManager.remapCoordinateSystem does for it, and negate the acceleration. */
            if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) {
                double a[3]; screen_axes(ev[i].acceleration.v, a);
                for (int k = 0; k < 3; k++) last_acc[k] = -a[k] / 9.80665;
            } else if (ev[i].type == ASENSOR_TYPE_GYROSCOPE) {
                double g[3]; screen_axes(ev[i].vector.v, g);
                if (motion_on < 0) { const char *m = getenv("TOTK_MOTION"); motion_on = !(m && !strcmp(m, "off")); }
                static const double still_acc[3] = {0, 0, -1}, still_g[3] = {0, 0, 0};
                if (motion_on) runtime_motion_feed(last_acc, g, runtime_now_ns()); else runtime_motion_feed(still_acc, still_g, runtime_now_ns());
            }
        }
    return 1;
}
static void *input_thread(void *arg) {
    (void)arg;
    input_looper = ALooper_prepare(0);
    ALooper_acquire(input_looper);
    load_controller_map();
    /* this app's package, without :game */
    char package[128] = {0}; { FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { if (fgets(package, sizeof package, f)) package[strcspn(package, ":")] = 0; fclose(f); } }
    ASensorManager *sm = ASensorManager_getInstanceForPackage(package);
    gyro = ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE);
    accel = ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER);
    if (gyro && accel) {
        sensor_queue = ASensorManager_createEventQueue(sm, input_looper, LOOPER_SENSORS, on_sensors, NULL);
        ASensorEventQueue_enableSensor(sensor_queue, accel); ASensorEventQueue_setEventRate(sensor_queue, accel, 5000);
        ASensorEventQueue_enableSensor(sensor_queue, gyro); ASensorEventQueue_setEventRate(sensor_queue, gyro, 5000);
        runtime_motion_live(1);
        fprintf(stderr, "[totk-input] motion: %s + %s at 200 Hz\n", ASensor_getName(gyro), ASensor_getName(accel));
    }
    for (;;) { ALooper_pollOnce(20, NULL, NULL, NULL); script_poll_now(); if (atomic_load(&pending_face) >= 0 || atomic_load(&pending_unfocus)) refresh_pad(); else pad_tick(); }
    return NULL;
}

/* ---- activity callbacks ---- */
void mtl_screen_attach(struct ANativeWindow *window);
void mtl_screen_detach(void);
/* The window callbacks suspend graphics (nvn_vulkan.c mtl_screen_attach / mtl_screen_detach) and leave the release to
   us: presenting resumes once the activity is resumed, whichever of onResume and the new window comes last. */
void dm_gfx_suspend(int on) __attribute__((weak));
static atomic_int resumed;
static void graphics_release(void) { if (dm_gfx_suspend && atomic_load(&resumed)) dm_gfx_suspend(0); }
static void on_window_created(ANativeActivity *a, ANativeWindow *w) {
    (void)a; ANativeWindow_acquire(w); android_vi_set_window(w); mtl_screen_attach(w); graphics_release();
    window_width = ANativeWindow_getWidth(w);
    fprintf(stderr, "[android] window %dx%d\n", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
}
/* The window is withdrawn from the game first: a backend initialisation still to come then waits for the next one instead
   of taking this one as it goes away. */
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; android_vi_set_window(NULL); mtl_screen_detach(); ANativeWindow_release(w); }
static void on_queue_created(ANativeActivity *a, AInputQueue *q) {
    (void)a;
    while (!input_looper) usleep(1000);
    pthread_mutex_lock(&queue_lock);
    input_queue = q; AInputQueue_attachLooper(q, input_looper, LOOPER_INPUT, on_input, NULL);
    pthread_mutex_unlock(&queue_lock);
}
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) {
    (void)a; pthread_mutex_lock(&queue_lock); AInputQueue_detachLooper(q); input_queue = NULL; pthread_mutex_unlock(&queue_lock);
}
static void on_focus(ANativeActivity *a, int has_focus) {
    (void)a; atomic_store(&focused, has_focus);
    if (!has_focus) { atomic_store(&pending_unfocus, 1); if (input_looper) ALooper_wake(input_looper); }
}
static void on_pause(ANativeActivity *a) { (void)a; atomic_store(&resumed, 0); fprintf(stderr, "[android] paused\n"); }
static void on_resume(ANativeActivity *a) { (void)a; atomic_store(&resumed, 1); fprintf(stderr, "[android] resumed\n"); graphics_release(); }

/* The overlay's "Quit to startup screen": finish the game activity (the startup screen is still behind it), and with it
   the :game process (GameActivity.onDestroy), so the next Continue starts a fresh game with the settings of that moment.
   A save in progress is waited out first: its slot would be left half written (fs_host.c). On a thread of its own, so
   the menu keeps presenting meanwhile. */
static void *quit_thread(void *arg) {
    (void)arg; uint64_t t0 = runtime_now_ns();
    while (runtime_fs_save_busy() && runtime_now_ns() - t0 < 30000000000ull) usleep(50000);
    if (runtime_fs_save_busy()) fprintf(stderr, "[android] 【红】quit: the save is still being written after 30 s, quitting anyway\n");
    else if (runtime_now_ns() - t0 > 100000000ull) fprintf(stderr, "[android] quit waited %.1f s for the save to finish\n", (runtime_now_ns() - t0) / 1e9);
    if (the_activity) ANativeActivity_finish(the_activity);
    return NULL;
}
void dm_android_quit(void) {
    static atomic_int quitting; if (atomic_exchange(&quitting, 1)) return;
    pthread_t t; if (pthread_create(&t, NULL, quit_thread, NULL)) quit_thread(NULL); else pthread_detach(t);
}
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *saved, size_t saved_size) {
    the_activity = activity;
    (void)saved; (void)saved_size;
    /* a picked file is gone (GameActivity): no game, and the activity finishes */
    if (getenv("TOTK_SETUP_STOP")) return;
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onInputQueueCreated = on_queue_created;
    activity->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    activity->callbacks->onWindowFocusChanged = on_focus;
    activity->callbacks->onPause = on_pause;
    activity->callbacks->onResume = on_resume;
    /* first: the input thread reads the field controls settings from launch.env */
    start_game(activity);
    static int threads; if (!threads) { threads = 1; pthread_t t; pthread_create(&t, NULL, input_thread, NULL); pthread_detach(t); start_rumble(activity); }
}
