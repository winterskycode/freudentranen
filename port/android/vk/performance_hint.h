#ifndef DM_ANDROID_PERFORMANCE_HINT_H
#define DM_ANDROID_PERFORMANCE_HINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADPF performance hints (APerformanceHint, API 33+): one hint session holds the threads of the frame chain, and each
   presented frame reports its duration against a target, so the CPU governor keeps those threads' cores fast through
   the many short waits of a frame. The target follows the frame cap (1 / cap): uncapped (120) it asks for more than
   we reach, which keeps the boost on; under a lower cap it lets the clocks come down.
   TOTK_ADPF=0 turns it off. The game's own threads stay out of the session.
   On a still scene, uncapped, it makes no frame-time difference: the prime cores already average ~4.25 GHz, and the
   device's performance mode 2 most likely gains its ~3 ms from the memory-bus floor it raises (DDR memlat floor
   547 -> 4761 MHz; mode 1, at 2092 MHz, gains nothing), which ADPF cannot raise. Without the game's threads the tails
   are slightly better (p99 26 vs 28 ms). It is on for the frame-cap (efficiency) case, where a 25 ms target lets the
   clocks come down. */
/* the calling thread joins the session */
void dm_android_perf_hint_thread(void);
/* once per presented game frame */
void dm_android_perf_hint_frame(void);

#ifdef __cplusplus
}
#endif

#endif
