/* nn::vi for Android: one display and one layer, as MacNativeGames' macOS front end has. Vsync events are signalled at
   60 Hz; the native window handed to the game is the app's ANativeWindow when there is one (android_vi_set_window),
   else a placeholder (headless runs). */
#include "runtime.h"
#include <android/native_window.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void hle_bind_system_event(void *ev, int autoclear);
void hle_signal_system_event(void *ev);
static int vi_ready;
static unsigned display_identity, layer_identity, window_identity;
static void *native_window;
/* The app's window as the activity hands it over and takes it back (host/app.c), for the game (android_vi_window) and the
   Vulkan backend's initialisation, which takes it with a reference of its own (dm_android_window_acquire) or, when the
   app went to the background during start-up, waits for the next one (dm_android_window_wait). */
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_changed = PTHREAD_COND_INITIALIZER;
void android_vi_set_window(void *window) {
    pthread_mutex_lock(&window_lock); native_window = window; pthread_cond_broadcast(&window_changed); pthread_mutex_unlock(&window_lock);
}
void *android_vi_window(void) { pthread_mutex_lock(&window_lock); void *w = native_window; pthread_mutex_unlock(&window_lock); return w; }
void *dm_android_window_acquire(void) {
    pthread_mutex_lock(&window_lock); void *w = native_window; if (w) ANativeWindow_acquire(w); pthread_mutex_unlock(&window_lock);
    return w;
}
void *dm_android_window_wait(void) {
    pthread_mutex_lock(&window_lock); while (!native_window) pthread_cond_wait(&window_changed, &window_lock);
    void *w = native_window; pthread_mutex_unlock(&window_lock); return w;
}
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vsync_events[16];
static unsigned vsync_count;
static void *vsync_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vi-vsync");
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 16666667; if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&vsync_lock);
        for (unsigned i = 0; i < vsync_count; i++) hle_signal_system_event(vsync_events[i]);
        pthread_mutex_unlock(&vsync_lock);
    }
    return NULL;
}
static void vi_initialize(void) { vi_ready = 1; fprintf(stderr, "[android] VI display initialized\n"); }
static uint32_t vi_open(void **out) { if (!vi_ready || !out) return 0xf601; *out = &display_identity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &display_identity) return 0xf601; *out = &layer_identity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layer_identity) return 0xf601; *out = native_window ? native_window : (void *)&window_identity; return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &display_identity) return 0xf601;
    hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&vsync_lock);
    if (vsync_count >= 16) runtime_fail("VI display event limit");
    vsync_events[vsync_count++] = ev;
    if (vsync_count == 1) { pthread_t t; pthread_create(&t, NULL, vsync_thread, NULL); pthread_detach(t); }
    pthread_mutex_unlock(&vsync_lock);
    return 0;
}
void *android_vi_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
