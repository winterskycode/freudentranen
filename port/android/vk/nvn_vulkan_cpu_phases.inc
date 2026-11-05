#include <sched.h>
// Read-only phase timing: accumulate the recording thread's and present path's wall clock per stage
// each frame, so the CPU budget can be attributed from measurement instead of estimate.
// Only counters and nanoseconds accumulate; no behaviour, ordering or visible result changes.
enum {
    DM_CPU_PHASE_DRAW, DM_CPU_PHASE_BIND, DM_CPU_PHASE_UBO, DM_CPU_PHASE_DESCRIPTOR,
    DM_CPU_PHASE_LAYOUT, DM_CPU_PHASE_DEPENDENCY, DM_CPU_PHASE_RESOLVE, DM_CPU_PHASE_DESCSET,
    DM_CPU_PHASE_RECORD_COUNT
};
enum {
    DM_CPU_PHASE_SUBMIT = DM_CPU_PHASE_RECORD_COUNT, DM_CPU_PHASE_ACQUIRE, DM_CPU_PHASE_BLIT,
    DM_CPU_PHASE_PRESENT, DM_CPU_PHASE_WAIT_PRIOR, DM_CPU_PHASE_PRESENT_TOTAL, DM_CPU_PHASE_COUNT
};
static const char *const dm_cpu_phase_names[DM_CPU_PHASE_COUNT] = {
    "draw", "bind", "ubo", "desc", "layout", "dep", "resolve", "descset",
    "submit", "acquire", "blit", "present", "wait_prior", "present_total"
};
// Phases may nest: draw encloses bind/dep and present_total encloses the present path; each item's own call count separates inclusive from self cost.
static uint64_t dm_cpu_phase_ns[DM_CPU_PHASE_COUNT], dm_cpu_phase_calls[DM_CPU_PHASE_COUNT];
static uint64_t dm_cpu_phase_frames;
// The record phases (draw/bind/ubo/desc/layout/dep/resolve/descset) are ~16k begin/end pairs and 30k+ clock reads per
// frame (~1000 draws in camp); on the S24U that is ~20% of the game thread's user samples (__kernel_clock_gettime),
// the largest single item. Record-phase timing is therefore off by default and only on with
// DM_VK_CPU_PHASES=1 (cpu_phases=1 in launch-settings.conf); present-path phases (a few per frame) are always timed.
// When off, begin returns 0 and end returns on 0, and the log says record_phases=off so zeros are not read as data.
static int dm_cpu_record_phases = -1;
static int dm_cpu_record_phases_enabled(void) {
    if (dm_cpu_record_phases < 0) { const char *e = getenv("DM_VK_CPU_PHASES"); dm_cpu_record_phases = e && !strcmp(e, "1"); }
    return dm_cpu_record_phases;
}
static inline uint64_t dm_cpu_phase_begin(void) { return dm_cpu_record_phases > 0 ? dm_android_clock_ns(CLOCK_MONOTONIC) : 0; }
static inline uint64_t dm_cpu_phase_begin_always(void) { return dm_android_clock_ns(CLOCK_MONOTONIC); }
static inline void dm_cpu_phase_end(unsigned phase, uint64_t started) {
    if (!started) return;
    dm_cpu_phase_ns[phase] += dm_android_clock_ns(CLOCK_MONOTONIC) - started;
    dm_cpu_phase_calls[phase]++;
}
static void dm_cpu_phase_report(uint64_t frames) {
    if (!frames) return;
    dm_cpu_phase_frames += frames;
    static uint64_t previous_ns[DM_CPU_PHASE_COUNT], previous_calls[DM_CPU_PHASE_COUNT];
    char line[768]; size_t used = 0;
    for (unsigned phase = 0; phase < DM_CPU_PHASE_COUNT; phase++) {
        double ms = (double)(dm_cpu_phase_ns[phase] - previous_ns[phase]) / (frames * 1e6);
        double calls = (double)(dm_cpu_phase_calls[phase] - previous_calls[phase]) / frames;
        int written = snprintf(line + used, sizeof line - used, "%s%s=%.3fms/%.1fcalls",
                               phase ? " " : "", dm_cpu_phase_names[phase], ms, calls);
        if (written < 0 || (size_t)written >= sizeof line - used) break;
        used += (size_t)written;
        previous_ns[phase] = dm_cpu_phase_ns[phase]; previous_calls[phase] = dm_cpu_phase_calls[phase];
    }
    LOG("[vulkan-cpu-phases] frames=%llu record_phases=%s %s", (unsigned long long)dm_cpu_phase_frames, dm_cpu_record_phases > 0 ? "on" : "off", line);
}
// Dedicated abnormal-frame log: only frames over the thresholds plus their phase breakdown go into a separate
// file, so a reproduction can be read straight from it instead of searching the whole game log.
// Each frame costs a handful of subtractions and two comparisons; the file is written only on abnormal frames.
enum { DM_ABNORMAL_INTERVAL_NS = 20000000 };
static FILE *dm_abnormal_file;
static int dm_abnormal_failed;
static uint64_t dm_abnormal_previous_ns[DM_CPU_PHASE_COUNT],dm_abnormal_previous_calls[DM_CPU_PHASE_COUNT];
static uint64_t dm_abnormal_frame_ns[DM_CPU_PHASE_COUNT],dm_abnormal_frame_calls[DM_CPU_PHASE_COUNT];
static uint64_t dm_abnormal_frames, dm_abnormal_records;
static void dm_abnormal_open(void) {
    if (dm_abnormal_file || dm_abnormal_failed) return;
    const char *directory = getenv("DM_DATA");
    if (!directory || !*directory) directory = getenv("DM_TEMP");
    if (!directory || !*directory) { dm_abnormal_failed = 1; return; }
    char path[1024];
    int written = snprintf(path, sizeof path, "%s/abnormal-frames.log", directory);
    if (written < 0 || (size_t)written >= sizeof path) { dm_abnormal_failed = 1; return; }
    dm_abnormal_file = fopen(path, "a");
    if (!dm_abnormal_file) { dm_abnormal_failed = 1; return; }
    fprintf(dm_abnormal_file, "# 异常帧日志：帧间隔或线程 CPU 达到 %d ms 才记一行；分段是该帧自身的耗时\n",
            DM_ABNORMAL_INTERVAL_NS / 1000000);
    fprintf(dm_abnormal_file, "# abnormal-frame log: a line per frame whose interval or thread CPU reaches %d ms; phases are that frame's own cost\n",
            DM_ABNORMAL_INTERVAL_NS / 1000000);
    fflush(dm_abnormal_file);
    LOG("[vulkan-abnormal] 异常帧日志 %s", path);
}
// Frequency separates "the same work got slower" from "the device downclocked": phases scaling together while the
// frequency drops is the latter.
// Sampling only the current thread's core would mislead: the scheduler may move the frame elsewhere, and the
// reading would describe a different core. So read this thread's core and the fastest core together.
static long dm_abnormal_cpu_khz(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char text[32];
    ssize_t count = read(fd, text, sizeof text - 1);
    close(fd);
    if (count <= 0) return -1;
    text[count] = 0;
    return strtol(text, NULL, 10);
}
static long dm_abnormal_this_core_khz(void) {
    int core = sched_getcpu();
    if (core < 0) return -1;
    char path[128];
    int written = snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    if (written < 0 || (size_t)written >= sizeof path) return -1;
    return dm_abnormal_cpu_khz(path);
}
static long dm_abnormal_fastest_core_khz(void) {
    long fastest = -1;
    for (int core = 0; core < 8; core++) {
        char path[128];
        if (snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core) <= 0) continue;
        long khz = dm_abnormal_cpu_khz(path);
        if (khz > fastest) fastest = khz;
    }
    return fastest;
}
// Take this frame's phase deltas at every present boundary; only abnormal frames reach the disk, normal frames cost only these subtractions.
static void dm_abnormal_frame(uint64_t frame, uint64_t interval_ns, uint64_t cpu_ns, uint64_t gpu_ns,
                              uint64_t draws, uint64_t passes, uint64_t uploads,
                              uint64_t ubo_bytes, uint64_t ubo_misses, uint64_t descriptor_misses) {
    for (unsigned phase = 0; phase < DM_CPU_PHASE_COUNT; phase++) {
        dm_abnormal_frame_ns[phase] = dm_cpu_phase_ns[phase] - dm_abnormal_previous_ns[phase];
        dm_abnormal_previous_ns[phase] = dm_cpu_phase_ns[phase];
        dm_abnormal_frame_calls[phase] = dm_cpu_phase_calls[phase] - dm_abnormal_previous_calls[phase];
        dm_abnormal_previous_calls[phase] = dm_cpu_phase_calls[phase];
    }
    dm_abnormal_frames++;
    // The three counters are cumulative in the game; the log wants this frame's own amount, so take the difference.
    static uint64_t previous_draws, previous_passes, previous_uploads;
    uint64_t frame_draws = draws - previous_draws, frame_passes = passes - previous_passes, frame_uploads = uploads - previous_uploads;
    previous_draws = draws; previous_passes = passes; previous_uploads = uploads;
    // Byte counts and miss counts go in together: when a phase slows down, that separates "more bytes per call"
    // from "the same work running slower".
    static uint64_t previous_ubo_bytes, previous_ubo_misses, previous_descriptor_misses;
    uint64_t frame_ubo_bytes = ubo_bytes - previous_ubo_bytes, frame_ubo_misses = ubo_misses - previous_ubo_misses, frame_descriptor_misses = descriptor_misses - previous_descriptor_misses;
    previous_ubo_bytes = ubo_bytes; previous_ubo_misses = ubo_misses; previous_descriptor_misses = descriptor_misses;
    if (interval_ns < DM_ABNORMAL_INTERVAL_NS && cpu_ns < DM_ABNORMAL_INTERVAL_NS) return;
    dm_abnormal_open();
    if (!dm_abnormal_file) return;
    dm_abnormal_records++;
    fprintf(dm_abnormal_file, "帧 %llu 间隔 %.2f ms CPU %.2f ms GPU %.2f ms 绘制 %llu 通道 %llu 贴图上传 %llu UBO读取 %.0f KiB/变更 %llu 描述符未命中 %llu 本核 %ld kHz 最快核 %ld kHz\n",
            (unsigned long long)frame, interval_ns / 1e6, cpu_ns / 1e6, gpu_ns / 1e6,
            (unsigned long long)frame_draws, (unsigned long long)frame_passes, (unsigned long long)frame_uploads,
            frame_ubo_bytes / 1024., (unsigned long long)frame_ubo_misses, (unsigned long long)frame_descriptor_misses,
            dm_abnormal_this_core_khz(), dm_abnormal_fastest_core_khz());
    // Every entry carries this frame's call count: when a cost rises, that tells "more work" from "slower per call" at a glance.
    char line[1024]; size_t used = 0;
    for (unsigned phase = 0; phase < DM_CPU_PHASE_COUNT; phase++) {
        int written = snprintf(line + used, sizeof line - used, "%s%s=%.3f/%llu", phase ? " " : "",
                               dm_cpu_phase_names[phase], dm_abnormal_frame_ns[phase] / 1e6,
                               (unsigned long long)dm_abnormal_frame_calls[phase]);
        if (written < 0 || (size_t)written >= sizeof line - used) break;
        used += (size_t)written;
    }
    fprintf(dm_abnormal_file, "  record_phases=%s %s\n", dm_cpu_record_phases > 0 ? "on" : "off", line);
    fflush(dm_abnormal_file);
}
