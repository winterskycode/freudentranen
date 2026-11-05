/* Prepare mode (TOTK_PREPARE=1: the startup screen's "Prepare shaders", host/app.c sets it for one start). The work a new
   player's first sessions otherwise do in the background of play -- every shader of the game translated (the shader
   setup, nvn_vulkan_shader_setup.inc) and built once for the GPU as a pipeline library (nvn_vulkan_gpl.inc) -- done in
   one go before play: the game stays paused at its first frame (overlay_prepare), a progress panel shows, the
   pipeline cache is saved and the app returns to the startup screen. During play the device then stays cool: the library
   build in the background of play can take the Odin to 100 C. <files>/prepared.txt records it for
   the startup screen (dropped when the pipeline cache it went into is not the one in use, prepare_check). */
void dm_android_quit(void) __attribute__((weak));
void overlay_prepare(int active, const char *stage, const char *detail, float fraction);
static int prepare_mode(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_PREPARE"); v = e && atoi(e) > 0; } return v; }
static _Atomic int prepare_stage, prepare_finished;
static uint64_t prepare_begin;
/* The libraries' shape: a change that alters what the prepare step builds (layouts, shader patches) bumps it, so the
   startup screen offers the step again. */
enum { PREPARE_FORMAT = 2 };
static void prepared_path(char *path, size_t n) { const char *f = getenv("TOTK_FILES_DIR"); snprintf(path, n, "%s/prepared.txt", f ? f : "."); }
static void *prepare_progress_main(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-prepare-ui");
    while (!atomic_load(&prepare_finished)) {
        char detail[240] = ""; const char *stage = ""; float f = -1; int st = atomic_load(&prepare_stage);
        if (st == 1 && setup.list) st = 2;
        if (st == 1) { stage = "Finding the game's shaders"; snprintf(detail, sizeof detail, "%zu found so far", setup.count); }
        else if (st == 2) { size_t done = (size_t)(setup.translated + setup.present + setup.failed); stage = "Translating shaders (step 1 of 2)";
            if (setup.count) f = (float)done / (float)setup.count; snprintf(detail, sizeof detail, "%zu of %zu", done, setup.count); }
        else if (st == 3) { size_t done = atomic_load(&gpl_warm_done); stage = "Building them for the GPU (step 2 of 2)";
            if (gpl_warm_count) f = (float)done / (float)gpl_warm_count; snprintf(detail, sizeof detail, "%zu of %zu", done, gpl_warm_count); }
        else if (st == 5) { stage = "Rebuilding the pipelines you used before"; f = -1; snprintf(detail, sizeof detail, "%s", "almost done"); }
        else if (st == 4) { stage = "Saving"; f = 1; }
        if (st) { uint64_t s = (dm_android_clock_ns(CLOCK_MONOTONIC) - prepare_begin) / 1000000000ull; size_t n = strlen(detail);
            snprintf(detail + n, sizeof detail - n, "%s%llu:%02llu", n ? "   ·   " : "", (unsigned long long)(s / 60), (unsigned long long)(s % 60)); }
        overlay_prepare(1, stage, detail, f);
        struct timespec ts = {0, 250000000}; nanosleep(&ts, NULL);
    }
    return NULL;
}
static void *prepare_main(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "totk-prepare");
    prepare_begin = dm_android_clock_ns(CLOCK_MONOTONIC);
    overlay_prepare(1, "Starting", "", -1);
    { pthread_t t; if (!pthread_create(&t, NULL, prepare_progress_main, NULL)) pthread_detach(t); }
    /* nothing else runs: more translators than in play */
    if (!getenv("TOTK_SHADER_SETUP_JOBS")) setenv("TOTK_SHADER_SETUP_JOBS", "4", 1);
    atomic_store(&prepare_stage, 1); setup_thread(NULL);
    LOG("[prepare] shaders translated (%.0f s)", (dm_android_clock_ns(CLOCK_MONOTONIC) - prepare_begin) / 1e9);
    atomic_store(&prepare_stage, 3);
    if (gpl_warm_start(1)) while (atomic_load(&gpl_warm_left) > 0) { struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL); }
    /* the warm-up of the pipelines this device used before (nvn_vulkan_warm.inc) runs alongside from the start: after an
       update that changed the shaders it rebuilds them all, which would otherwise be the first session's background work */
    while (atomic_load_explicit(&pso_warm_running, memory_order_acquire)) { atomic_store(&prepare_stage, 5); struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL); }
    atomic_store(&prepare_stage, 4);
    pipeline_disk_save();
    /* Mesa writes its disk cache from a queue of its own: let it drain */
    { struct timespec ts = {3, 0}; nanosleep(&ts, NULL); }
    char path[600]; prepared_path(path, sizeof path); FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "cache %s\nformat %d\nshaders %zu\nlibraries %zu\nseconds %.0f\n", pipeline_disk_name, PREPARE_FORMAT, gpl_warm_count, (size_t)atomic_load(&gpl_warm_built), (dm_android_clock_ns(CLOCK_MONOTONIC) - prepare_begin) / 1e9); fclose(f); }
    LOG("[prepare] done in %.0f s: %zu shaders, %zu libraries; back to the startup screen", (dm_android_clock_ns(CLOCK_MONOTONIC) - prepare_begin) / 1e9, gpl_warm_count, (size_t)atomic_load(&gpl_warm_built));
    atomic_store(&prepare_finished, 1); overlay_prepare(0, NULL, NULL, 0);
    if (dm_android_quit) dm_android_quit(); else exit(0);
    return NULL;
}
static void prepare_start(void) { pthread_t t; if (!pthread_create(&t, NULL, prepare_main, NULL)) pthread_detach(t); }
/* Outside prepare mode: a prepared.txt for another pipeline cache (another driver, a cleared cache) is dropped. */
static void prepare_check(void) {
    if (prepare_mode()) return;
    char path[600]; prepared_path(path, sizeof path); char *have = totk_read_text(path); if (!have) return;
    char want[160]; snprintf(want, sizeof want, "cache %s\nformat %d\n", pipeline_disk_name, PREPARE_FORMAT);
    if (strncmp(have, want, strlen(want))) { unlink(path); LOG("[prepare] prepared.txt was for another pipeline cache or library format: removed"); }
    else { gpl_prepared = 1; LOG("[prepare] shaders prepared: new pipelines are linked and drawn at once"); }
    free(have);
}
