/* Flight recorder for bug reports (recorder.h). The Vulkan backend hands over each presented picture as NV12
   (nvn_vulkan_record.inc, at most RECORDER_FPS a second); the device's H.264 encoder (AMediaCodec, a key frame every
   second) turns them into packets, written to <files>/flight/seg-<n>.rec: append-only records, so a segment stays
   readable up to its last whole record when the game crashes, unlike an MP4 (its index is written at the end).
   A segment ends at the first key frame after SEGMENT_US, and starts with the encoder's configuration (SPS, PPS), so
   each one decodes on its own; the newest KEEP segments are kept, across runs too (the evidence of a crash is still
   there at the next start). The startup screen turns segments into MP4 files (LauncherActivity, MediaMuxer).

   Segment format: "TOTKREC1", width and height (u32 each), then records: u32 size, u32 flags (1 key frame, 2 codec
   configuration), i64 presentation time in microseconds, size bytes of Annex-B H.264. */
#include "files.h"
#include "recorder.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SEGMENT_US 5000000
#define KEEP 9
#define BIT_RATE 8000000
#define FLAG_KEY 1
#define FLAG_CONFIG 2

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AMediaCodec *codec;
static int running, wanted = -1, stride = RECORDER_W, slice = RECORDER_H;
static pthread_t drain;
static char dir[600];
/* the encoder's SPS and PPS */
static uint8_t *config; static size_t config_size;
static FILE *segment; static int segment_number; static int64_t segment_start;
static uint64_t frames, dropped, packets;

int recorder_enabled(void) {
    if (wanted < 0) { const char *e = getenv("TOTK_RECORD"); wanted = e && !strcmp(e, "on"); if (wanted) recorder_set(1); }
    return running;
}

static int segment_numbers(int *lowest, int *highest) {
    *lowest = *highest = 0; DIR *d = opendir(dir); if (!d) return 0;
    int n = 0, lo = 0, hi = 0; struct dirent *e;
    while ((e = readdir(d))) { int k; if (sscanf(e->d_name, "seg-%d.rec", &k) == 1) { if (!n || k < lo) lo = k; if (!n || k > hi) hi = k; n++; } }
    closedir(d); *lowest = lo; *highest = hi; return n;
}

static void record(FILE *f, const uint8_t *data, uint32_t size, uint32_t flags, int64_t pts) {
    fwrite(&size, 4, 1, f); fwrite(&flags, 4, 1, f); fwrite(&pts, 8, 1, f); fwrite(data, 1, size, f);
}

/* segments from this number on are being copied into a report (recorder_snapshot): kept */
static int pinned = -1;
/* The next segment, from a key frame at pts (lock held) */
static void segment_next(int64_t pts) {
    if (segment) fclose(segment);
    char path[700]; int lo, hi;
    if (segment_numbers(&lo, &hi) >= KEEP) for (int k = lo; k <= hi - KEEP + 1 && (pinned < 0 || k < pinned); k++) { snprintf(path, sizeof path, "%s/seg-%06d.rec", dir, k); unlink(path); }
    snprintf(path, sizeof path, "%s/seg-%06d.rec", dir, ++segment_number);
    segment = fopen(path, "wb"); segment_start = pts;
    if (!segment) { fprintf(stderr, "[recorder] %s: %s\n", path, strerror(errno)); return; }
    uint32_t wh[2] = {RECORDER_W, RECORDER_H};
    fwrite("TOTKREC1", 1, 8, segment); fwrite(wh, 4, 2, segment);
    if (config) record(segment, config, (uint32_t)config_size, FLAG_CONFIG, pts);
}

static void *drain_main(void *arg) {
    (void)arg;
    while (running) {
        AMediaCodecBufferInfo info; ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, 20000);
        if (o < 0) continue;
        size_t capacity; uint8_t *b = AMediaCodec_getOutputBuffer(codec, (size_t)o, &capacity);
        pthread_mutex_lock(&lock);
        if (b && info.size > 0 && (size_t)info.offset + (size_t)info.size <= capacity) {
            const uint8_t *data = b + info.offset;
            if (info.flags & FLAG_CONFIG) { free(config); config = malloc((size_t)info.size); if (config) { memcpy(config, data, (size_t)info.size); config_size = (size_t)info.size; } }
            else {
                int key = (info.flags & FLAG_KEY) != 0;
                if (key && (!segment || info.presentationTimeUs - segment_start >= SEGMENT_US)) segment_next(info.presentationTimeUs);
                if (segment) { record(segment, data, (uint32_t)info.size, key ? FLAG_KEY : 0, info.presentationTimeUs); fflush(segment); packets++; }
            }
        }
        pthread_mutex_unlock(&lock);
        AMediaCodec_releaseOutputBuffer(codec, (size_t)o, false);
    }
    return NULL;
}

static int start(void) {
    const char *files = getenv("TOTK_FILES_DIR");
    if (!files) return 0;
    snprintf(dir, sizeof dir, "%s/flight", files); mkdir(dir, 0700);
    int lo, hi; segment_number = segment_numbers(&lo, &hi) ? hi : 0;
    codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec) { fprintf(stderr, "[recorder] no H.264 encoder\n"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, RECORDER_W); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, RECORDER_H);
    /* COLOR_FormatYUV420SemiPlanar: NV12 */
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, BIT_RATE); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, RECORDER_FPS);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    /* BT.709, limited, SDR */
    AMediaFormat_setInt32(f, "color-standard", 1); AMediaFormat_setInt32(f, "color-range", 2); AMediaFormat_setInt32(f, "color-transfer", 3);
    media_status_t s = AMediaCodec_configure(codec, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (s != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) { fprintf(stderr, "[recorder] encoder did not start (%d)\n", (int)s); AMediaCodec_delete(codec); codec = NULL; return 0; }
    AMediaFormat *in = AMediaCodec_getInputFormat(codec); int32_t v;
    stride = RECORDER_W; slice = RECORDER_H;
    if (in) { if (AMediaFormat_getInt32(in, "stride", &v) && v >= RECORDER_W) stride = v; if (AMediaFormat_getInt32(in, "slice-height", &v) && v >= RECORDER_H) slice = v; AMediaFormat_delete(in); }
    running = 1; frames = dropped = packets = 0;
    pthread_create(&drain, NULL, drain_main, NULL);
    fprintf(stderr, "[recorder] on: %dx%d H.264 %d Mbit/s into %s (stride %d, slice height %d)\n", RECORDER_W, RECORDER_H, BIT_RATE / 1000000, dir, stride, slice);
    return 1;
}

static void stop(void) {
    running = 0; pthread_join(drain, NULL);
    AMediaCodec_stop(codec); AMediaCodec_delete(codec); codec = NULL;
    pthread_mutex_lock(&lock); if (segment) { fclose(segment); segment = NULL; } pthread_mutex_unlock(&lock);
    fprintf(stderr, "[recorder] off: %llu pictures, %llu dropped, %llu packets\n", (unsigned long long)frames, (unsigned long long)dropped, (unsigned long long)packets);
}

/* start / stop against frames */
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
void recorder_set(int on) {
    pthread_mutex_lock(&control);
    wanted = on != 0;
    if (on && !running) start(); else if (!on && running) stop();
    pthread_mutex_unlock(&control);
}

void recorder_frame(const uint8_t *nv12, uint64_t pts_ns) {
    /* being started or stopped */
    if (pthread_mutex_trylock(&control)) return;
    if (!running) { pthread_mutex_unlock(&control); return; }
    ssize_t i = AMediaCodec_dequeueInputBuffer(codec, 0);
    if (i < 0) { dropped++; pthread_mutex_unlock(&control); return; }
    size_t capacity; uint8_t *b = AMediaCodec_getInputBuffer(codec, (size_t)i, &capacity);
    size_t need = (size_t)stride * slice + (size_t)stride * (RECORDER_H / 2);
    if (!b || capacity < need) { AMediaCodec_queueInputBuffer(codec, (size_t)i, 0, 0, pts_ns / 1000, 0); dropped++; pthread_mutex_unlock(&control); return; }
    for (int y = 0; y < RECORDER_H; y++) memcpy(b + (size_t)y * stride, nv12 + (size_t)y * RECORDER_W, RECORDER_W);
    for (int y = 0; y < RECORDER_H / 2; y++) memcpy(b + (size_t)stride * slice + (size_t)y * stride, nv12 + (size_t)RECORDER_W * RECORDER_H + (size_t)y * RECORDER_W, RECORDER_W);
    AMediaCodec_queueInputBuffer(codec, (size_t)i, 0, need, pts_ns / 1000, 0);
    if (!(++frames % 3600)) fprintf(stderr, "[recorder] %llu pictures, %llu dropped, %llu packets\n", (unsigned long long)frames, (unsigned long long)dropped, (unsigned long long)packets);
    pthread_mutex_unlock(&control);
}

int recorder_snapshot(const char *to, int seconds) {
    if (!dir[0]) { const char *files = getenv("TOTK_FILES_DIR"); if (!files) return 0; snprintf(dir, sizeof dir, "%s/flight", files); }
    /* The segments are pinned rather than copied under the lock, so recording goes on meanwhile: holding the lock
       through ~35 MB of copying would stall the encoder's output and drop frames. The segment being written is
       copied as far as it is flushed; a record cut short ends the replay there (ReportStore.mux). */
    pthread_mutex_lock(&lock); if (segment) fflush(segment);
    int lo, hi, n = segment_numbers(&lo, &hi), want = seconds * 1000000 / SEGMENT_US + 1, copied = 0, first = hi - want + 1 > lo ? hi - want + 1 : lo;
    if (n) pinned = first;
    pthread_mutex_unlock(&lock);
    mkdir(to, 0755);
    for (int k = first; n && k <= hi; k++) {
        char a[700], b[900]; snprintf(a, sizeof a, "%s/seg-%06d.rec", dir, k); snprintf(b, sizeof b, "%s/seg-%06d.rec", to, k);
        copied += copy_file(a, b);
    }
    pthread_mutex_lock(&lock); pinned = -1; pthread_mutex_unlock(&lock);
    return copied;
}
