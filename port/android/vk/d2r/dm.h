// D2R 宿主公共头中本后端用到的部分。
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
void dm_guest_frame_tick(void);        // 每次出图调（hle_nvn.c）：DM_DETCLOCK 开时把客体时钟推进一个固定步长 | Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
void dm_3d_scale_resumed(void);       // 前端：刚回前台。回来头几秒的慢帧是恢复（表面重建、缓存变冷），不是 GPU 吃紧：停判 3 秒 | front end: just returned to the foreground. Slow frames in the first seconds back are recovery (surface rebuild, caches gone cold), not GPU pressure: stop judging for 3 seconds
void hle_signal_system_event(void *g);   // hle_thread.c：触发服务层 SystemEvent（任何线程） | hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // 客体线程上把客体 SystemEvent 绑到内核事件 | On a guest thread, bind a guest SystemEvent to a kernel event
void dm_guest_write_prepare(void *addr, size_t len);
void dm_gfx_suspend(int on);
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // 图形侧截图写盘完成（diag.c：DM_TICK_HOLD=E:SHOT 据此解冻） | The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
