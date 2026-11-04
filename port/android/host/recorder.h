/* Flight recorder for bug reports (recorder.c): the presented pictures, encoded to H.264 by the device's encoder into
   rolling segment files that a crash cannot spoil; a bug report copies the last 30 seconds of them. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { RECORDER_W = 1280, RECORDER_H = 720, RECORDER_FPS = 60 };

/* TOTK_RECORD=on at start; the in-game menu turns it on and off (recorder_set). */
int recorder_enabled(void);
void recorder_set(int on);
/* One picture, NV12 at RECORDER_W x RECORDER_H (luma rows, then interleaved chroma rows), shown at pts_ns
   (CLOCK_MONOTONIC). Dropped when the encoder has no free input. Any thread. */
void recorder_frame(const uint8_t *nv12, uint64_t pts_ns);
/* Copies the segments that hold the last `seconds` into dir (created); the number copied, 0 when nothing is recorded. */
int recorder_snapshot(const char *dir, int seconds);

#ifdef __cplusplus
}
#endif
