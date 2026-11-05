// The parts of the D2R host's common header that this backend uses.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

uint64_t now_ns(void);
int dm_detclock_enabled(void);
// Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
void dm_guest_frame_tick(void);
// front end: just returned to the foreground. Slow frames in the first seconds back are recovery (surface rebuild,
// caches gone cold), not GPU pressure: stop judging for 3 seconds
void dm_3d_scale_resumed(void);
// hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_signal_system_event(void *g);
// On a guest thread, bind a guest SystemEvent to a kernel event
void hle_bind_system_event(void *g, int autoclear);
void dm_guest_write_prepare(void *addr, size_t len);
void dm_gfx_suspend(int on);
void dm_gfx_shot_at(long k, const char *path);
// The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_shot_written(const char *path, int ok);
