/* TOTK_LATENCY_PROBE=1 (diagnostic, with TOTK_TIMELINE): input-to-screen latency by cause and effect.
   Each present, generated pictures too (the first motion on screen is often one), copies two rows of the picture (at 1/2 and 1/3 of its height) into a ring of host buffers; once
   the GPU has written them, the change from the previous picture goes to the timeline (TL_PIXELS: a = present id, the
   one TL_SHOWN reports, b = mean absolute byte difference x 1000). A camera turn from a scripted stick moves the whole
   row, far above the still scene's own motion: the first such picture after the game read the stick is the effect. */
#include "totk_timeline.h"
enum { LAT_SLOTS = 8 };
typedef struct { vk_buffer buffer; uint64_t ticket; uint32_t id, bytes, bpb; } lat_slot;
static lat_slot lat_ring[LAT_SLOTS];
/* slots recorded / compared, counted from 0 */
static uint32_t lat_next, lat_done;
static uint8_t *lat_prev; static uint32_t lat_prev_bytes;

static int lat_latency_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_LATENCY_PROBE"); on = e && atoi(e) > 0; }
    /* TL_SHOWN comes with or without pacing */
    return on && totk_tl_at && pace_ext && pace_get_past;
}
/* TOTK_PICTURE_SIG=1 (diagnostic): the same rows give a picture signature every 15 pictures, "[pic] <n> <32 values>":
   the mean of each sixteenth of each row (all channels, 0-255). The graphics lever hunt (gfxsweep) compares it before,
   during and after a field is changed: what a field does to the picture, and whether the change outlives the restore. */
static int lat_picture_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PICTURE_SIG"); on = e && atoi(e) > 0; }
    return on;
}
static int lat_probe_on(void) { return lat_latency_on() || lat_picture_on(); }
/* in the present's command buffer, before the blit; id = the present id this picture will get */
static void lat_probe_record(image_entry *e, const tex_t *t, int sx, int sy, int sw, int sh, uint32_t id) {
    if (!lat_probe_on() || lat_next - lat_done >= LAT_SLOTS) return;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bw != 1 || bh != 1 || bpb <= 0 || bpb > 16 || sw <= 0 || sh < 3) return;
    lat_slot *s = &lat_ring[lat_next % LAT_SLOTS]; uint32_t row = (uint32_t)sw * bpb;
    if (s->buffer.buffer && s->buffer.size < 2ull * row) buffer_destroy(&s->buffer);
    if (!s->buffer.buffer) s->buffer = buffer_create(2ull * row);
    VkBufferImageCopy c[2] = {{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {sx, sy + sh / 2, 0}, .imageExtent = {(uint32_t)sw, 1, 1}},
                              {.bufferOffset = row, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {sx, sy + sh / 3, 0}, .imageExtent = {(uint32_t)sw, 1, 1}}};
    vkCmdCopyImageToBuffer(command, e->image, VK_IMAGE_LAYOUT_GENERAL, s->buffer.buffer, 2, c);
    s->id = id; s->bpb = (uint32_t)bpb; s->bytes = 2 * row; s->ticket = 0; lat_next++;
}
/* after the commit: the ticket of the slot just recorded; then compare every slot the GPU has finished, in order */
static void lat_probe_collect(uint64_t ticket) {
    if (!lat_probe_on()) return;
    if (lat_next != lat_done && !lat_ring[(lat_next - 1) % LAT_SLOTS].ticket) lat_ring[(lat_next - 1) % LAT_SLOTS].ticket = ticket;
    while (lat_done != lat_next) {
        lat_slot *s = &lat_ring[lat_done % LAT_SLOTS];
        if (!s->ticket || !mtl_wait(s->ticket, 0)) break;
        const uint8_t *px = s->buffer.mapped;
        static unsigned pictures;
        if (lat_picture_on() && !(pictures++ % 15)) {
            char b[400]; int k = snprintf(b, sizeof b, "[pic] %u", pictures - 1); uint32_t seg = s->bytes / 32;
            for (unsigned g = 0; g < 32 && seg; g++) { uint64_t sum = 0; for (uint32_t i = 0; i < seg; i++) sum += px[g * seg + i]; k += snprintf(b + k, sizeof b - k, " %u", (unsigned)(sum / seg)); }
            LOG("%s", b);
        }
        /* not alpha: never shown, and FSR's pictures leave it 0 */
        if (lat_latency_on() && lat_prev && lat_prev_bytes == s->bytes) { uint64_t d = 0; for (uint32_t i = 0; i < s->bytes; i++) if (s->bpb != 4 || (i & 3) != 3) d += (uint64_t)abs((int)px[i] - (int)lat_prev[i]);
            totk_tl_at(TL_PIXELS, dm_android_clock_ns(CLOCK_MONOTONIC), s->id, d * 1000 / s->bytes); }
        if (lat_prev_bytes != s->bytes) { free(lat_prev); lat_prev = malloc(s->bytes); lat_prev_bytes = lat_prev ? s->bytes : 0; }
        if (lat_prev) memcpy(lat_prev, px, s->bytes);
        lat_done++;
    }
}
