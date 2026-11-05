// Texture creation, Tegra deswizzling, uploads and render-target clears.
#include "nvn_vulkan_image_pool.inc"
/* frame profile (TOTK_VK_FRAMEPROF): where a new image goes, on the translation thread */
static uint64_t arrival_alloc_ns,arrival_alloc_bytes,arrival_view_ns,arrival_views,arrival_fb_ns,arrival_fbs;
/* bl_slot: slot + 1 in each global array (nvn_vulkan_bindless.inc) */
typedef struct cached_image_view { VkImageViewType type; VkImageSubresourceRange range; VkComponentMapping components; VkFormat format; VkImageView view; VkFramebuffer clear_fb[2]; struct cached_image_view *next; uint32_t bl_slot[13]; } cached_image_view;
static void bindless_view_forget(cached_image_view *v);
typedef struct image_entry {
    uint64_t serial; tex_t texture; VkImage image; VkImageView view; image_memory memory;
    VkFormat format; VkImageAspectFlags aspect; uint64_t stamp; int populated;
    uint64_t source_stamp,source_hash,source_seen;int source_valid,source_tracked;
    VkPipelineStageFlags last_stage;VkAccessFlags last_access;int gpu_written;
    /* chunk_epoch of its last GPU access (upload census: an upload into an image untouched in the chunk could
       precede the chunk) */
    uint64_t touched_chunk;
    /* game_sync_epoch when the game last wrote it (attachment / storage write): TOTK_VK_SYNCCHECK */
    uint64_t write_epoch;
    /* the writer (census detail) */
    const char *write_shader; VkPipelineStageFlags write_stage;
    /* census: the piece and submission of the last game write */
    int write_piece; uint64_t write_frame;
    /* written as an attachment since its last feedback barrier (draw_graphics_inner, feedback loops) */
    int fb_dirty;
    /* write-back to guest memory (image_writeback_sweep) */
    uint64_t gpu_write_frame; unsigned writebacks; int writeback_due;
    /* frames in a row it was written by the GPU, up to gpu_write_frame (draw_graphics_inner: skippable draws) */
    unsigned write_streak;
    /* a deferred clear (image_clear_defer), its value in that format */
    int pclear; VkClearValue pclear_value; VkImageSubresourceRange pclear_range; VkFormat pclear_format;
    cached_image_view *views;
    /* TOTK: block-sized render target standing in for this compressed image */
    struct image_entry *block_aux;
    struct image_key { VkImageType type; VkFormat format; VkExtent3D extent; uint32_t levels, layers; VkImageCreateFlags flags; VkImageUsageFlags usage; } key;
    /* TOTK_VK_OVERLAP: the pass its range was last marked in; the draw of its last GPU write */
    uint64_t overlap_noted, overlap_written;
    /* TOTK storage sharing: serials holding this image (the creator and its aliases), idle since (frame) when none.
       view_formats: the format list of a mutable image. */
    unsigned holders; uint64_t idle_since; int storage_keyed, mutable_format; VkFormat view_formats[16]; uint32_t view_format_count; struct storage_key { uint64_t mem, size; uint32_t w, h, d, levels, target, format_class; } storage;
    struct image_entry *next,*hash_next,*storage_next;
} image_entry;
typedef struct { int valid; tex_t texture; view_t view; } texture_descriptor;
typedef struct { int valid; samp_par parameters; VkSampler sampler; } sampler_descriptor;
static image_entry *image_list,*image_index[4096];
/* TOTK: several live texture objects describe the same pool memory with the same layout -- the game registers a
   sampling object in the texture pool and re-initialises the render-target object at that storage every frame. On
   hardware both read the same bytes; with one VkImage per serial, what is rendered through one is invisible through
   the other (the deferred lighting reads black G-buffers). Textures are keyed by storage and layout (as the Metal
   backend does): a later serial with the same key and format becomes an alias of the existing image. An image nobody
   holds stays for STORAGE_IDLE_FRAMES so a re-initialised object finds last frame's contents. Block-compressed
   formats are not shared. */
typedef struct image_alias { uint64_t serial; image_entry *e; struct image_alias *next; } image_alias;
static image_alias *image_aliases[4096];
static image_entry *storage_index[4096];
static uint64_t storage_shared, storage_format_mismatch;
#define STORAGE_IDLE_FRAMES 60
static unsigned storage_bucket(const struct storage_key *k) { return (unsigned)((k->mem>>12)^(k->mem>>24))&4095; }
static int storage_key_of(const tex_t *t,struct storage_key *k) {
    if(!t->mem||!t->L.size)return 0;
    int bw=1,bh=1,bpb=nvn_fmt_bpb(t->d.format,&bw,&bh);if(!bpb||bw!=1||bh!=1)return 0;
    /* colour formats of one texel size share (format views on a mutable image); depth/stencil formats only with themselves */
    memset(k,0,sizeof *k);k->mem=(uint64_t)(uintptr_t)t->mem;k->size=t->L.size;k->w=t->d.width;k->h=t->d.height;k->d=t->d.depth;k->levels=t->d.levels;k->target=(uint32_t)t->d.target;
    k->format_class=t->d.format>=0x31&&t->d.format<=0x36?0x1000u+(uint32_t)t->d.format:(uint32_t)bpb;
    return 1;
}
static image_entry *image_lookup(uint64_t serial) {
    for(image_entry *e=image_index[serial&4095];e;e=e->hash_next)if(e->serial==serial)return e;
    for(image_alias *a=image_aliases[serial&4095];a;a=a->next)if(a->serial==serial)return a->e;
    return NULL;
}
/* TOTK re-initialises texture objects for its per-frame render targets; a new VkImage and vkAllocateMemory for each
   would be 27% of the render thread on the title screen. Released images keep their image and memory in a free list
   keyed by every creation parameter and are reused for the next texture that matches. */
typedef struct free_image { struct image_key key; VkImage image; image_memory memory; VkDeviceSize size; struct free_image *next; } free_image;
static free_image *free_images; static VkDeviceSize free_image_bytes; static uint64_t image_reuses, image_creations;
#define FREE_IMAGE_BUDGET (512ull << 20)
static void image_reap_locked(void);
/* TOTK_VK_TRACE_FMT=<hex NVN format>: the life of every image of that format (creation, sharing, clears, uploads, GPU
   writes, release) */
static int trace_fmt(const tex_t *t) { static int f=-2;if(f==-2){const char *x=getenv("TOTK_VK_TRACE_FMT");f=x?(int)strtol(x,NULL,16):-1;}return f>=0&&t->d.format==(unsigned)f; }
#define TRACEIMG(t,fmt,...) do { static unsigned said_;if(trace_fmt(t)&&said_++<4000)LOG("[vulkan-trace] frame %llu draw %llu serial %llu %ux%u mem %p: " fmt,(unsigned long long)presents,(unsigned long long)draw_count,(unsigned long long)(t)->serial,(t)->d.width,(t)->d.height,(t)->mem,##__VA_ARGS__); } while(0)
static texture_descriptor *texture_descriptors;
static sampler_descriptor *sampler_descriptors;
static int texture_capacity, sampler_capacity;
/* The resolved sampler of each id the shaders can bind (12 bits), read without the descriptor lock on every texture
   binding (a lock pair per binding would be the translation thread's most frequent lock). Set with
   descriptor->sampler under the lock, cleared when the id is registered again or the sampler settings change
   (nvn_vulkan_sampler.inc). Samplers are never destroyed, so a lookup racing a re-registration gets the previous,
   still valid sampler, as the lock gave. */
static _Atomic uintptr_t sampler_fast[4096];
static pthread_mutex_t descriptor_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t texture_uploads, render_passes, draw_count;
static uint64_t image_barriers;
static int image_view_usage(const struct image_entry *e,VkFormat format,VkImageViewUsageCreateInfo *out) {
    if(!e->mutable_format)return 0;
    VkFormatProperties p;vkGetPhysicalDeviceFormatProperties(physical,format,&p);VkFormatFeatureFlags f=p.optimalTilingFeatures;
    *out=(VkImageViewUsageCreateInfo){.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,.usage=e->key.usage&(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT
        |(f&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT?VK_IMAGE_USAGE_SAMPLED_BIT:0)|(f&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0)|(f&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT?VK_IMAGE_USAGE_STORAGE_BIT:0))};
    return 1;
}
/* format: the view's format; another format than the image's only on mutable (storage-shared) images */
static cached_image_view *image_view_formatted(image_entry *e,VkImageViewType type,VkImageSubresourceRange range,VkComponentMapping components,VkFormat format) {
    if(!format||!e->mutable_format)format=e->format;
    for(cached_image_view *v=e->views;v;v=v->next)if(v->type==type&&v->format==format&&!memcmp(&v->range,&range,sizeof range)&&!memcmp(&v->components,&components,sizeof components))return v;
    if(format!=e->format){int listed=0;for(uint32_t k=0;k<e->view_format_count;k++)listed|=e->view_formats[k]==format;
        if(!listed){static unsigned said;if(said++<20)LOG("[vulkan-view-format] image format %d viewed as %d, outside its format list (%ux%u)",e->format,format,e->texture.d.width,e->texture.d.height);}
        /* which reinterpretations the game uses */
        static struct { VkFormat i,v; } seen[64];static unsigned n;unsigned k=0;while(k<n&&(seen[k].i!=e->format||seen[k].v!=format))k++;
        if(k==n&&n<64){seen[n].i=e->format;seen[n].v=format;n++;LOG("[vulkan-view-format] image format %d viewed as %d (%ux%u)",e->format,format,e->texture.d.width,e->texture.d.height);}}
    cached_image_view *v=calloc(1,sizeof *v);if(!v)die("Vulkan image view cache allocation");v->type=type;v->range=range;v->components=components;v->format=format;
    VkImageViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=e->image,.viewType=type,.format=format,.components=components,.subresourceRange=range};
    /* a view of a mutable image inherits the image's usages, some of which its format may lack (sRGB storage) */
    VkImageViewUsageCreateInfo view_usage;if(image_view_usage(e,format,&view_usage))ci.pNext=&view_usage;
    uint64_t view_t0=dm_android_clock_ns(CLOCK_MONOTONIC);VK_CHECK(vkCreateImageView(device,&ci,NULL,&v->view));arrival_view_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-view_t0;arrival_views++;v->next=e->views;e->views=v;return v;
}
/* NVN swizzle selectors (0 zero, 1 one, 2..5 R G B A) of the texture, or of the view when it sets them, as a Vulkan
   component mapping; RGBX formats read alpha as one. All-zero selectors (textures the backend made itself) are identity. */
static VkComponentMapping texture_components(const tex_t *t,const view_t *v) {
    unsigned sel[4]={t->d.swizzle_r,t->d.swizzle_g,t->d.swizzle_b,t->d.swizzle_a};
    if(v&&v->set_swizzle)for(int i=0;i<4;i++)sel[i]=v->swz[i];
    int format=v&&v->set_format?v->format:t->d.format;
    VkComponentSwizzle c[4];
    for(int i=0;i<4;i++){
        if(!sel[0]&&!sel[1]&&!sel[2]&&!sel[3]){c[i]=VK_COMPONENT_SWIZZLE_IDENTITY;continue;}
        /* RGBX: no alpha */
        unsigned s=sel[i];if(s==5&&format>=0x6d&&format<=0x78)s=1;
        c[i]=s==0?VK_COMPONENT_SWIZZLE_ZERO:s==1?VK_COMPONENT_SWIZZLE_ONE:s<=5?(VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R+s-2):VK_COMPONENT_SWIZZLE_IDENTITY;
        if(c[i]==(VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R+i))c[i]=VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    if(!sel[0]&&!sel[1]&&!sel[2]&&!sel[3]&&format>=0x6d&&format<=0x78)c[3]=VK_COMPONENT_SWIZZLE_ONE;
    /* RGB5, RGB5A1, RGB565 keep red in the low bits, Vulkan's packed formats in the high bits; RGB5 has no alpha */
    if(format>=0x3a&&format<=0x3c)for(int i=0;i<4;i++){
        VkComponentSwizzle x=c[i]==VK_COMPONENT_SWIZZLE_IDENTITY?(VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R+i):c[i];
        c[i]=x==VK_COMPONENT_SWIZZLE_R?VK_COMPONENT_SWIZZLE_B:x==VK_COMPONENT_SWIZZLE_B?VK_COMPONENT_SWIZZLE_R:x==VK_COMPONENT_SWIZZLE_A&&format==0x3a?VK_COMPONENT_SWIZZLE_ONE:x;
    }
    return (VkComponentMapping){c[0],c[1],c[2],c[3]};
}

/* TOTK_VK_PASSLOG=<frame>: log every render pass, draw, clear, copy and dispatch of that frame (debug). */
/* Also on demand: touching <files>/passlog.trigger (adb run-as) logs the next frame; the file is removed. */
static long passlog_frame = -2; static uint64_t passlog_checked = UINT64_MAX;
static int passlog_on_slow(void);
/* hot path: off, and the trigger file was checked this frame already (passlog_on_slow looks every 30 frames) */
static inline int passlog_on(void) { return passlog_frame < 0 && presents == passlog_checked ? 0 : passlog_on_slow(); }
static int passlog_on_slow(void) {
    long *framep = &passlog_frame; long frame = *framep; if (frame == -2) { const char *e = getenv("TOTK_VK_PASSLOG"); frame = *framep = e ? atol(e) : -1; }
    uint64_t *checkedp = &passlog_checked; uint64_t checked = *checkedp;
    /* not a trigger-check frame: remembered for the hot path */
    if (frame < 0 && presents % 30) { *checkedp = presents; return 0; }
    if (presents != checked && !(presents % 30)) { checked = presents; *checkedp = checked;
        const char *dir = getenv("DM_VK_SHADERS"); char path[1024];
        if (dir && snprintf(path, sizeof path, "%s/../passlog.trigger", dir) < (int)sizeof path && !access(path, F_OK)) { unlink(path); frame = *framep = (long)presents + 1; LOG("[vulkan-passlog] triggered: logging frames %ld and %ld", frame, frame + 1); } }
    return frame >= 0 && (presents == (uint64_t)frame || presents == (uint64_t)frame + 1);
}
#define PASSLOG(...) do { if (passlog_on()) LOG("[vulkan-passlog] " __VA_ARGS__); } while (0)
static unsigned passlog_draws;
static void image_access(struct image_entry *e,VkPipelineStageFlags stage,VkAccessFlags access);
/* The texel at x,y of each pass's first attachment, copied after the pass into one buffer and printed after the present. */
static vk_buffer passlog_buffer; static unsigned passlog_samples; static struct { uint64_t serial; int format, bpb; } passlog_sampled[2304];
static void passlog_sample(struct image_entry *e,uint32_t x,uint32_t y);
static void passlog_print(void);
static VkFormat vk_format(int format) {
    static const VkFormat formats[] = {
        [0x01]=VK_FORMAT_R8_UNORM, [0x02]=VK_FORMAT_R8_SNORM, [0x03]=VK_FORMAT_R8_UINT, [0x04]=VK_FORMAT_R8_SINT,
        [0x05]=VK_FORMAT_R16_SFLOAT, [0x06]=VK_FORMAT_R16_UNORM, [0x07]=VK_FORMAT_R16_SNORM, [0x08]=VK_FORMAT_R16_UINT, [0x09]=VK_FORMAT_R16_SINT,
        [0x0a]=VK_FORMAT_R32_SFLOAT, [0x0b]=VK_FORMAT_R32_UINT, [0x0c]=VK_FORMAT_R32_SINT,
        [0x0d]=VK_FORMAT_R8G8_UNORM, [0x0e]=VK_FORMAT_R8G8_SNORM, [0x0f]=VK_FORMAT_R8G8_UINT, [0x10]=VK_FORMAT_R8G8_SINT,
        [0x11]=VK_FORMAT_R16G16_SFLOAT, [0x12]=VK_FORMAT_R16G16_UNORM, [0x13]=VK_FORMAT_R16G16_SNORM, [0x14]=VK_FORMAT_R16G16_UINT, [0x15]=VK_FORMAT_R16G16_SINT,
        [0x16]=VK_FORMAT_R32G32_SFLOAT, [0x17]=VK_FORMAT_R32G32_UINT, [0x18]=VK_FORMAT_R32G32_SINT,
        [0x19]=VK_FORMAT_R8G8B8_UNORM, [0x1a]=VK_FORMAT_R8G8B8_SNORM, [0x1b]=VK_FORMAT_R8G8B8_UINT, [0x1c]=VK_FORMAT_R8G8B8_SINT,
        [0x1d]=VK_FORMAT_R16G16B16_SFLOAT, [0x1e]=VK_FORMAT_R16G16B16_UNORM, [0x1f]=VK_FORMAT_R16G16B16_SNORM, [0x20]=VK_FORMAT_R16G16B16_UINT, [0x21]=VK_FORMAT_R16G16B16_SINT,
        [0x22]=VK_FORMAT_R32G32B32_SFLOAT, [0x23]=VK_FORMAT_R32G32B32_UINT, [0x24]=VK_FORMAT_R32G32B32_SINT,
        [0x25]=VK_FORMAT_R8G8B8A8_UNORM, [0x26]=VK_FORMAT_R8G8B8A8_SNORM, [0x27]=VK_FORMAT_R8G8B8A8_UINT, [0x28]=VK_FORMAT_R8G8B8A8_SINT,
        [0x29]=VK_FORMAT_R16G16B16A16_SFLOAT, [0x2a]=VK_FORMAT_R16G16B16A16_UNORM, [0x2b]=VK_FORMAT_R16G16B16A16_SNORM, [0x2c]=VK_FORMAT_R16G16B16A16_UINT, [0x2d]=VK_FORMAT_R16G16B16A16_SINT,
        [0x2e]=VK_FORMAT_R32G32B32A32_SFLOAT, [0x2f]=VK_FORMAT_R32G32B32A32_UINT, [0x30]=VK_FORMAT_R32G32B32A32_SINT,
        [0x31]=VK_FORMAT_S8_UINT, [0x32]=VK_FORMAT_D16_UNORM, [0x33]=VK_FORMAT_X8_D24_UNORM_PACK32, [0x34]=VK_FORMAT_D32_SFLOAT, [0x35]=VK_FORMAT_D24_UNORM_S8_UINT, [0x36]=VK_FORMAT_D32_SFLOAT_S8_UINT,
        [0x37]=VK_FORMAT_R8G8B8A8_SRGB, [0x38]=VK_FORMAT_R8G8B8A8_SRGB,
        /* red in the low bits: texture_components swaps r and b */
        [0x39]=VK_FORMAT_R4G4B4A4_UNORM_PACK16, [0x3a]=VK_FORMAT_A1R5G5B5_UNORM_PACK16, [0x3b]=VK_FORMAT_A1R5G5B5_UNORM_PACK16, [0x3c]=VK_FORMAT_R5G6B5_UNORM_PACK16,
        [0x3d]=VK_FORMAT_A2B10G10R10_UNORM_PACK32, [0x3e]=VK_FORMAT_A2B10G10R10_UINT_PACK32, [0x3f]=VK_FORMAT_B10G11R11_UFLOAT_PACK32, [0x40]=VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [0x41]=VK_FORMAT_BC1_RGB_UNORM_BLOCK, [0x42]=VK_FORMAT_BC1_RGBA_UNORM_BLOCK, [0x43]=VK_FORMAT_BC2_UNORM_BLOCK, [0x44]=VK_FORMAT_BC3_UNORM_BLOCK,
        [0x45]=VK_FORMAT_BC1_RGB_SRGB_BLOCK, [0x46]=VK_FORMAT_BC1_RGBA_SRGB_BLOCK, [0x47]=VK_FORMAT_BC2_SRGB_BLOCK, [0x48]=VK_FORMAT_BC3_SRGB_BLOCK,
        [0x49]=VK_FORMAT_BC4_UNORM_BLOCK, [0x4a]=VK_FORMAT_BC4_SNORM_BLOCK, [0x4b]=VK_FORMAT_BC5_UNORM_BLOCK, [0x4c]=VK_FORMAT_BC5_SNORM_BLOCK,
        [0x4d]=VK_FORMAT_BC7_UNORM_BLOCK, [0x4e]=VK_FORMAT_BC7_SRGB_BLOCK, [0x4f]=VK_FORMAT_BC6H_SFLOAT_BLOCK, [0x50]=VK_FORMAT_BC6H_UFLOAT_BLOCK,
        [0x6d]=VK_FORMAT_R8G8B8A8_UNORM, [0x6e]=VK_FORMAT_R8G8B8A8_SNORM, [0x6f]=VK_FORMAT_R8G8B8A8_UINT, [0x70]=VK_FORMAT_R8G8B8A8_SINT,
        [0x71]=VK_FORMAT_R16G16B16A16_SFLOAT, [0x72]=VK_FORMAT_R16G16B16A16_UNORM, [0x73]=VK_FORMAT_R16G16B16A16_SNORM, [0x74]=VK_FORMAT_R16G16B16A16_UINT, [0x75]=VK_FORMAT_R16G16B16A16_SINT,
        [0x76]=VK_FORMAT_R32G32B32A32_SFLOAT, [0x77]=VK_FORMAT_R32G32B32A32_UINT, [0x78]=VK_FORMAT_R32G32B32A32_SINT,
    };
    /* TOTK vertex formats: *_UI2F / *_I2F (integer data read as float) are fetched as integers and converted by the
       translated shader (the answers say uscaled/sscaled; Vulkan has no 32-bit scaled formats) */
    if (format >= 0x51 && format <= 0x68) {
        static const VkFormat uints[4][3] = {{VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT}, {VK_FORMAT_R8G8_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R32G32_UINT},
            {VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R32G32B32_UINT}, {VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R32G32B32A32_UINT}};
        /* each *_SINT follows its *_UINT */
        int i = format - 0x51; return uints[i / 6][i / 2 % 3] + (i & 1);
    }
    switch (format) {
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6a: case 0x6c: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case 0x6b: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    }
    if (format >= 0x79 && format <= 0x94) return VK_FORMAT_ASTC_4x4_UNORM_BLOCK + (format - 0x79) % 14 * 2 + (format >= 0x87);
    return format > 0 && format < sizeof formats / sizeof *formats ? formats[format] : VK_FORMAT_UNDEFINED;
}
// Synchronize actual read/write hazards; avoid global barriers for consecutive reads or same-pass attachment writes.
static void image_clear_flush(image_entry *e);
/* TOTK_VK_SYNCCHECK=1: whether the game's own barriers (nvnCommandBufferBarrier, game_sync_epoch) cover the image
   hazards we synchronize ourselves: at each read after a game write, was there a game barrier since that write? A
   layer that derives synchronization from the recording alone can only use the game's barriers. */
/* game_sync_epoch (nvn_vulkan.c): advances at every game barrier and submission */
static int synccheck_state = -1;
static inline int synccheck_on(void) { if (synccheck_state < 0) { const char *e = getenv("TOTK_VK_SYNCCHECK"); synccheck_state = e && atoi(e) > 0; } return synccheck_state; }
/* [0] colour attachment, [1] depth attachment, [2] storage image */
static struct { uint64_t synced[3], unsynced[3], fb_synced, fb_unsynced, barriers; } sc;
/* the attachment access being recorded can write (depth writes on / colour mask nonzero): only then is it a game
   write for the census */
static int attachment_write_real = 1;
/* the shaders of the write and of the read (census detail): a draw's fragment shader or a dispatch's compute shader */
static const char *synccheck_writer, *synccheck_reader;
static void synccheck_read(image_entry *e, int feedback) {
    if (!synccheck_on()) return;
    VkAccessFlags w = e->last_access; int kind = w & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ? 0 : w & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT ? 1 : w & VK_ACCESS_SHADER_WRITE_BIT ? 2 : -1;
    /* our own transfer / host writes: not the game's to synchronize */
    if (kind < 0) return;
    int synced = e->write_epoch != game_sync_epoch;
    if (feedback) { if (synced) sc.fb_synced++; else sc.fb_unsynced++; } else if (synced) sc.synced[kind]++; else sc.unsynced[kind]++;
    if (stateleak_state > 0 && !feedback) { if (e->write_frame != sl.submission) sl.img_older++; else if (e->write_piece != sl.piece) sl.img_cross++; else sl.img_same++; }
    if (!synced && !feedback && kind) { static unsigned shown; if (shown++ < 40) LOG("[vulkan-synccheck] unsynced %s read: %ux%u fmt %#x levels %u layers %u, written by %s at stage %#x, read by fs %s (draw %llu, pass open %d)",
        kind == 1 ? "depth" : "storage image", e->texture.d.width, e->texture.d.height, e->texture.d.format, e->texture.d.levels, e->texture.L.layers, e->write_shader ? e->write_shader : "-", e->write_stage, synccheck_reader ? synccheck_reader : "-", (unsigned long long)draw_count, graphics_pass_open()); }
    static uint64_t last; if (presents >= last + 300) { last = presents;
        LOG("[vulkan-synccheck] a frame: game barriers %.1f; reads after a game write: colour %.1f synced / %.1f NOT, depth %.1f / %.1f, storage image %.1f / %.1f; feedback reads in the pass %.1f / %.1f",
            sc.barriers / 300.0, sc.synced[0] / 300.0, sc.unsynced[0] / 300.0, sc.synced[1] / 300.0, sc.unsynced[1] / 300.0, sc.synced[2] / 300.0, sc.unsynced[2] / 300.0, sc.fb_synced / 300.0, sc.fb_unsynced / 300.0);
        memset(&sc, 0, sizeof sc); }
}
static void image_access(image_entry *e,VkPipelineStageFlags stage,VkAccessFlags access) {
    /* any other use first gets the deferred clear done */
    if(e->pclear)image_clear_flush(e);
    e->touched_chunk=chunk_epoch;
    VkAccessFlags writes=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    if(graphics_image_is_attachment(e)&&stage==e->last_stage&&access==e->last_access&&(access&(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)))return;
    /* measurement only: what feedback-loop pass breaks cost (unsynchronized) */
    { static int skip=-1;if(skip<0)skip=exp_env("nofeedbackbarrier");
      if(skip&&!(access&(VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT))&&graphics_image_is_attachment(e)){e->last_stage|=stage;e->last_access|=access;return;} }
    if(e->last_access&&((e->last_access|access)&writes)){
        if(!(access&writes))synccheck_read(e,0);
        /* why image hazards end passes */
        { static int diag=-1;if(diag<0){const char *x=getenv("TOTK_VK_PASSEND");diag=x&&atoi(x);}
          if(diag&&graphics_pass_open()&&!prelude_recording){static uint64_t n[2][2][2],last;int att=graphics_image_is_attachment(e),depth=!!(e->aspect&VK_IMAGE_ASPECT_DEPTH_BIT),
              rd=!(access&writes);n[att][depth][rd]++;
              if(presents>=last+300){last=presents;for(int i=0;i<8;i++)if(n[i>>2][(i>>1)&1][i&1])LOG("[vulkan-passend-image] %s %s, then %s: %llu",i>>2?"attachment of the pass":"other image",(i>>1)&1?"depth":"color",i&1?"read":"written",(unsigned long long)n[i>>2][(i>>1)&1][i&1]);memset(n,0,sizeof n);}} }
        /* the prelude's barrier goes ahead of the chunk, outside its pass */
        if(!prelude_recording)graphics_pass_end();
        VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=e->last_access,.dstAccessMask=access,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=e->image,.subresourceRange={e->aspect,0,e->texture.d.levels,0,e->texture.L.layers}};
        vkCmdPipelineBarrier(command,e->last_stage,stage,0,0,NULL,0,NULL,1,&b);image_barriers++;
        e->last_stage=stage;e->last_access=access;
    }else{e->last_stage|=stage;e->last_access|=access;}
    if((access&VK_ACCESS_SHADER_WRITE_BIT)||(attachment_write_real&&(access&(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)))){e->write_epoch=game_sync_epoch;e->write_stage=stage;e->write_shader=synccheck_writer;e->write_piece=sl.piece;e->write_frame=sl.submission;}
}
static uint32_t dimension(uint32_t v, unsigned level) { v >>= level; return v ? v : 1; }
static image_entry *image_get(const tex_t *t) {
    unsigned bucket=t->serial&4095;{image_entry *found=image_lookup(t->serial);if(found)return found;}
    struct storage_key sk;int keyed=storage_key_of(t,&sk);
    if(keyed){unsigned sb=storage_bucket(&sk);
        for(image_entry *s=storage_index[sb];s;s=s->storage_next)if(!memcmp(&s->storage,&sk,sizeof sk)){
            image_alias *a=malloc(sizeof *a);if(!a)die("Vulkan image alias allocation");*a=(image_alias){t->serial,s,image_aliases[bucket]};image_aliases[bucket]=a;
            s->holders++;s->idle_since=0;storage_shared++;TRACEIMG(t,"shares entry %llu (holders %u populated %d gpu_written %d frame %llu pclear %d)",(unsigned long long)s->serial,s->holders,s->populated,s->gpu_written,(unsigned long long)s->gpu_write_frame,s->pclear);
            static unsigned said;if(said++<6)LOG("[vulkan] texture serial %llu shares storage %p (%ux%u format %#x) with serial %llu",(unsigned long long)t->serial,t->mem,t->d.width,t->d.height,t->d.format,(unsigned long long)s->serial);
            return s;}
        for(image_entry *s=storage_index[sb];s;s=s->storage_next)if(s->storage.mem==sk.mem){
            static unsigned said;if(said++<16)LOG("[vulkan] 【红】storage %p: serial %llu %ux%ux%u lv%u target %u class %#x size %llu differs from serial %llu %ux%ux%u lv%u target %u class %#x size %llu (not shared)",
                t->mem,(unsigned long long)t->serial,sk.w,sk.h,sk.d,sk.levels,sk.target,sk.format_class,(unsigned long long)sk.size,(unsigned long long)s->serial,s->storage.w,s->storage.h,s->storage.d,s->storage.levels,s->storage.target,s->storage.format_class,(unsigned long long)s->storage.size);break;}
    }
    /* live images over the same memory that are not shared */
    { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_ALIAS_LOG");on=x&&atoi(x);}
      if(on&&t->mem&&t->L.size){static unsigned said;uint64_t a0=(uint64_t)(uintptr_t)t->mem,a1=a0+t->L.size;
        for(image_entry *o=image_list;o&&said<4000;o=o->next){uint64_t b0=(uint64_t)(uintptr_t)o->texture.mem,b1=b0+o->texture.L.size;if(!b0||b1<=a0||b0>=a1)continue;said++;
            LOG("[vulkan-alias] new serial %llu fmt %#x %ux%ux%u lv%u mem %#llx+%llu flags %#x overlaps serial %llu fmt %#x %ux%ux%u lv%u mem %#llx+%llu flags %#x holders %u gpu_written %d",
                (unsigned long long)t->serial,t->d.format,t->d.width,t->d.height,t->d.depth,t->d.levels,(unsigned long long)a0,(unsigned long long)t->L.size,t->d.flags,
                (unsigned long long)o->serial,o->texture.d.format,o->texture.d.width,o->texture.d.height,o->texture.d.depth,o->texture.d.levels,(unsigned long long)b0,(unsigned long long)o->texture.L.size,o->texture.d.flags,o->holders,o->gpu_written);}}}
    if (t->d.target==NVN_TEXTURE_TARGET_BUFFER || t->d.samples>1) die("Vulkan texture target/samples pending: %d/%d",t->d.target,t->d.samples);
    image_entry *e=calloc(1,sizeof *e); if(!e) die("Vulkan texture allocation"); e->serial=t->serial;e->texture=*t;e->format=vk_format(t->d.format);graphics_pass_end();
    if(e->format==VK_FORMAT_UNDEFINED) die("Unknown Vulkan NVN format %#x",t->d.format);
    e->aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    if(t->d.format>=0x31 && t->d.format<=0x36) {e->aspect=0;if(t->d.format!=0x31)e->aspect|=VK_IMAGE_ASPECT_DEPTH_BIT;if(t->d.format==0x31||t->d.format==0x35||t->d.format==0x36)e->aspect|=VK_IMAGE_ASPECT_STENCIL_BIT;}
    VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(physical,e->format,&fp);
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    int is3d=t->d.target==NVN_TEXTURE_TARGET_3D,is1d=t->d.target==NVN_TEXTURE_TARGET_1D||t->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=is3d?VK_IMAGE_TYPE_3D:is1d?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D,.format=e->format,.extent={t->d.width,is1d?1:t->d.height,is3d?t->d.depth:1},.mipLevels=t->d.levels,.arrayLayers=t->L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(t->d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)ci.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    /* TOTK renders 3D textures one depth slice at a time (2D views of a slice) */
    if(is3d)ci.flags|=VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    /* storage-keyed colour images may be viewed as any format of their texel size (another texture object at the same
       storage): mutable, with the usages any format of that size supports */
    /* the formats a mutable image may be viewed as: without this list Turnip turns UBWC off (leaving all colour
       render targets uncompressed; compressing them saves 1.8 ms of GPU time a frame in the open world). Every
       listed format can be compressed; a view outside the list is logged ([vulkan-view-format]). */
    VkFormat view_formats[16];VkImageFormatListCreateInfo format_list={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,.pViewFormats=view_formats};
    if(keyed&&e->aspect==VK_IMAGE_ASPECT_COLOR_BIT){
        static const VkFormat class4[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_A2B10G10R10_UNORM_PACK32,0},
            class8[]={VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R16G16B16A16_UINT,0},class2[]={VK_FORMAT_R16_SFLOAT,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R16_UNORM,0},
            class1[]={VK_FORMAT_R8_UNORM,VK_FORMAT_R8_UINT,0},class16[]={VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32B32A32_UINT,0};
        int bw,bh;unsigned bpb=nvn_fmt_bpb(t->d.format,&bw,&bh);const VkFormat *cls=bpb==4?class4:bpb==8?class8:bpb==2?class2:bpb==1?class1:bpb==16?class16:NULL;
        if(cls){ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;e->mutable_format=1;
            uint32_t n=0;view_formats[n++]=ci.format;
            for(const VkFormat *f=cls;*f&&n<15;f++){if(*f!=ci.format)view_formats[n++]=*f;if(*f==VK_FORMAT_R8G8B8A8_UNORM&&ci.format!=VK_FORMAT_R8G8B8A8_SRGB)view_formats[n++]=VK_FORMAT_R8G8B8A8_SRGB;}
            format_list.viewFormatCount=n;format_list.pNext=ci.pNext;ci.pNext=&format_list;memcpy(e->view_formats,view_formats,n*sizeof *view_formats);e->view_format_count=n;
            for(const VkFormat *f=cls;*f;f++){VkFormatProperties p;vkGetPhysicalDeviceFormatProperties(physical,*f,&p);
                if(p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)ci.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if(p.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)ci.usage|=VK_IMAGE_USAGE_STORAGE_BIT;}}
    }
    e->key=(struct image_key){ci.imageType,ci.format,ci.extent,ci.mipLevels,ci.arrayLayers,ci.flags,ci.usage};
    image_reap_locked();
    free_image **reuse=&free_images;while(*reuse&&memcmp(&(*reuse)->key,&e->key,sizeof e->key))reuse=&(*reuse)->next;
    if(*reuse){free_image *f=*reuse;*reuse=f->next;e->image=f->image;e->memory=f->memory;free_image_bytes-=f->size;free(f);image_reuses++;}
    else{image_creations++;uint64_t alloc_t0=dm_android_clock_ns(CLOCK_MONOTONIC);
    VK_CHECK(vkCreateImage(device,&ci,NULL,&e->image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,e->image,&req);
    uint32_t type=UINT32_MAX;for(uint32_t i=0;i<memory_properties.memoryTypeCount;i++)if((req.memoryTypeBits & (1u<<i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
    if(type==UINT32_MAX)die("No Vulkan image memory");
    image_memory_take(&req,type,&e->memory);VK_CHECK(vkBindImageMemory(device,e->image,e->memory.memory,e->memory.offset));arrival_alloc_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-alloc_t0;arrival_alloc_bytes+=req.size;}
    VkImageViewType view= is3d?VK_IMAGE_VIEW_TYPE_3D:is1d?(t->L.layers>1?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):(t->L.layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D);
    if(ci.flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) view=t->d.target==NVN_TEXTURE_TARGET_CUBEMAP?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=e->image,.viewType=view,.format=e->format,.subresourceRange={e->aspect,0,t->d.levels,0,t->L.layers}};
    VkImageViewUsageCreateInfo view_usage;if(image_view_usage(e,e->format,&view_usage))vi.pNext=&view_usage;
    VK_CHECK(vkCreateImageView(device,&vi,NULL,&e->view));
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=e->image,.subresourceRange=vi.subresourceRange};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
    e->next=image_list;image_list=e;e->hash_next=image_index[bucket];image_index[bucket]=e;e->holders=1;
    if(keyed){e->storage_keyed=1;e->storage=sk;unsigned sb=storage_bucket(&sk);e->storage_next=storage_index[sb];storage_index[sb]=e;}
    { Dl_info info;void *ret=__builtin_return_address(0);TRACEIMG(t,"new entry %p keyed %d thread %d site libtotk+%#lx",(void*)e,keyed,(int)gettid(),dladdr(ret,&info)&&info.dli_fbase?(unsigned long)((uintptr_t)ret-(uintptr_t)info.dli_fbase):0ul); }
    return e;
}
/* TOTK renders into block-compressed textures through an uncompressed view whose texel is one block (BC4 through
   RGBA16UI). Vulkan cannot render to a compressed format, so the pass renders into an auxiliary image of the view
   format sized in blocks. vkCmdCopyImage copies between a compressed format and an uncompressed one of the same block
   size: the blocks are copied out first when the pass loads the old contents, and copied back when the pass ends. */
static void image_gpu_written(image_entry *e);
typedef struct { image_entry *aux,*dst; uint32_t level,layer,bx,by; } block_writeback;
static block_writeback block_writebacks[8];
static unsigned block_writeback_count;
static image_entry *block_aux_get(image_entry *e,const view_t *v) {
    int bw,bh,vbw=0,vbh=0;unsigned bpb=nvn_fmt_bpb(e->texture.d.format,&bw,&bh);
    if(bw==1&&bh==1)return NULL;
    unsigned vbpb=v&&v->set_format?nvn_fmt_bpb(v->format,&vbw,&vbh):0;
    if(!vbpb||vbpb!=bpb||vbw!=1||vbh!=1||vk_format(v->format)==VK_FORMAT_UNDEFINED)
        die("Vulkan render target on compressed format %#x through view format %#x: no block-sized uncompressed view",e->texture.d.format,v&&v->set_format?v->format:0);
    if(e->block_aux){if(e->block_aux->texture.d.format!=v->format)die("Vulkan compressed render target %#x viewed as %#x and %#x",e->texture.d.format,e->block_aux->texture.d.format,v->format);return e->block_aux;}
    if(e->texture.d.target==NVN_TEXTURE_TARGET_CUBEMAP||e->texture.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)die("Vulkan compressed cube render target");
    tex_t t=e->texture;t.serial=e->serial|1ull<<63;t.has_layout=0;t.mem=NULL;t.d.format=v->format;
    t.d.flags&=~(NVN_TEXTURE_FLAGS_LINEAR|NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
    t.d.width=(e->texture.d.width+bw-1)/bw;t.d.height=(e->texture.d.height+bh-1)/bh;
    e->block_aux=image_get(&t);
    LOG("[vulkan] rendering into compressed format %#x (serial %llu) through view format %#x via a %ux%u block image",e->texture.d.format,(unsigned long long)e->serial,v->format,t.d.width,t.d.height);
    return e->block_aux;
}
/* The region of e that range's level and layer (3D: depth slice) cover, as blocks of its auxiliary image. */
static block_writeback block_region(image_entry *e,image_entry *aux,VkImageSubresourceRange range) {
    int bw,bh;nvn_fmt_bpb(e->texture.d.format,&bw,&bh);
    uint32_t lw=dimension(e->texture.d.width,range.baseMipLevel),lh=dimension(e->texture.d.height,range.baseMipLevel);
    block_writeback w={aux,e,range.baseMipLevel,range.baseArrayLayer,(lw+bw-1)/bw,(lh+bh-1)/bh};
    if(w.bx>dimension(aux->texture.d.width,w.level)||w.by>dimension(aux->texture.d.height,w.level))die("Vulkan compressed render target level %u block size mismatch",w.level);
    return w;
}
static void block_copy(const block_writeback *w,int to_compressed) {
    int is3d=w->dst->texture.d.target==NVN_TEXTURE_TARGET_3D;image_entry *src=to_compressed?w->aux:w->dst,*dst=to_compressed?w->dst:w->aux;
    image_access(src,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);image_access(dst,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceLayers layers={VK_IMAGE_ASPECT_COLOR_BIT,w->level,is3d?0:w->layer,1};VkOffset3D at={0,0,is3d?(int32_t)w->layer:0};
    /* the extent is in texels of the source: blocks of the auxiliary image, or texels of the compressed one */
    VkExtent3D extent=to_compressed?(VkExtent3D){w->bx,w->by,1}:(VkExtent3D){dimension(w->dst->texture.d.width,w->level),dimension(w->dst->texture.d.height,w->level),1};
    VkImageCopy region={.srcSubresource=layers,.srcOffset=at,.dstSubresource=layers,.dstOffset=at,.extent=extent};
    vkCmdCopyImage(command,src->image,VK_IMAGE_LAYOUT_GENERAL,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    if(to_compressed)image_gpu_written(w->dst);else w->aux->populated=1;
}
/* Called by graphics_pass_end once the render pass has ended. */
static void block_writeback_flush(void) {
    unsigned n=block_writeback_count;block_writeback_count=0;
    for(unsigned i=0;i<n;i++)block_copy(&block_writebacks[i],1);
}
static void block_writeback_add(block_writeback w) {
    for(unsigned i=0;i<block_writeback_count;i++)if(!memcmp(&block_writebacks[i],&w,sizeof w))return;
    if(block_writeback_count>=8)die("Vulkan compressed render target writebacks");block_writebacks[block_writeback_count++]=w;
}
static void passlog_sample(image_entry *e,uint32_t x,uint32_t y) {
    int bw,bh,bpb=nvn_fmt_bpb(e->texture.d.format,&bw,&bh);
    if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT||bw!=1||bh!=1||bpb<=0||bpb>16||passlog_samples+9>2304)return;
    if(!passlog_buffer.buffer)passlog_buffer=buffer_create(2304*16);
    image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    /* a 3x3 grid at 1/4, 1/2, 3/4 of the pass area (x, y are its centre) */
    for(int gy=1;gy<=3;gy++)for(int gx=1;gx<=3;gx++){
        VkBufferImageCopy c={.bufferOffset=passlog_samples*16,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageOffset={(int32_t)(x*gx/2),(int32_t)(y*gy/2),0},.imageExtent={1,1,1}};
        vkCmdCopyImageToBuffer(command,e->image,VK_IMAGE_LAYOUT_GENERAL,passlog_buffer.buffer,1,&c);
        passlog_sampled[passlog_samples].serial=e->serial;passlog_sampled[passlog_samples].format=e->texture.d.format;passlog_sampled[passlog_samples++].bpb=bpb;}
}
static void passlog_pictures_write(void);
static unsigned passlog_pass;
static void passlog_print(void) {
    passlog_pictures_write();passlog_pass=0;
    for(unsigned i=0;i+9<=passlog_samples;i+=9){char hex[9*34+1]={0};size_t at=0;
        for(int k=0;k<9;k++){const uint8_t *p=(const uint8_t*)passlog_buffer.mapped+(i+k)*16;for(int b=0;b<passlog_sampled[i].bpb&&b<8;b++)at+=sprintf(hex+at,"%02x",p[b]);hex[at++]=' ';}
        LOG("[vulkan-passlog] sample %u: serial %llu fmt %#x centre texel %s",i/9,(unsigned long long)passlog_sampled[i].serial,passlog_sampled[i].format,hex);}
    passlog_samples=0;
}
// Track GPU contents separately from packaged CPU sources; GPU writes must not suppress subsequent real CPU changes.
static uint64_t image_source_hash(const uint8_t *source,uint64_t size) {
    uint64_t hash=14695981039346656037ull;for(uint64_t i=0;i<size;i++)hash=(hash^source[i])*1099511628211ull;return hash;
}
static void image_source_baseline(image_entry *e,const uint8_t *source,uint64_t stamp) {
    const tex_t *t=&e->texture;if(!t->has_layout||!t->mem)return;
    e->source_stamp=stamp;e->source_seen=stamp;e->source_tracked=nvn_dirty_tracked(t->mem,t->L.size);e->source_valid=1;
    // Untracked sources have no reliable generation; compare their bytes while retaining GPU copies if the source is unchanged.
    if(!e->source_tracked)e->source_hash=image_source_hash(source,t->L.size);
}
/* TOTK_VK_WATCH_MEM=<hex address>: every GPU write to an image whose storage holds it is logged, with the writer (the
   draw's shaders, or the calling site as libtotk.so offset for addr2line). */
/* set by the draw path around its image_gpu_written calls */
static const char *gpu_write_shaders;
static uintptr_t gpu_watch_address(void) { static int read;static uintptr_t a;if(!read){read=1;const char *x=getenv("TOTK_VK_WATCH_MEM");if(x)a=(uintptr_t)strtoull(x,NULL,16);}return a; }
/* The watched window: the 0x200 bytes from the watched address rounded down to 0x100 (a record and its neighbours:
   the Ultrahand pick ring alternates two 0x40-byte records). Whether [a, a + size) touches it. */
static int gpu_watch_hit(uintptr_t a, uint64_t size) {
    uintptr_t w=gpu_watch_address();if(!w)return 0;
    uintptr_t w0=w&~(uintptr_t)0xff;return a<w0+0x200&&a+size>w0;
}
/* TOTK_VK_OVERLAP=1 (diagnostic): images over the same memory with another layout are separate images. Which 64 KiB
   blocks each GPU-written image covers is recorded (the last writer per block and the draw it wrote at, once per
   render pass per image); a sampled image whose range a different image wrote later than its own last GPU write, in
   this or the last frame, is counted by writer and reader, the most frequent logged every 600 frames: how often a
   sample would miss what another image rendered there. A block shared by neighbouring images keeps only the last
   writer, so it undercounts. */
typedef struct { uintptr_t block, a, b; uint64_t serial, frame, draw; uint32_t fmt, w, h; } overlap_mark;
#define OVERLAP_SLOTS (1u << 18)
static overlap_mark *overlap_marks;
static int overlap_enabled(void) {
    static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_OVERLAP");on=x&&atoi(x)>0;if(on&&!(overlap_marks=calloc(OVERLAP_SLOTS,sizeof *overlap_marks)))on=0;}
    return on;
}
static overlap_mark *overlap_slot(uintptr_t block) {
    unsigned h=(unsigned)((block*0x9E3779B97F4A7C15ull)>>40)&(OVERLAP_SLOTS-1);
    for(unsigned i=0;i<32;i++){overlap_mark *m=&overlap_marks[(h+i)&(OVERLAP_SLOTS-1)];if(!m->block||m->block==block)return m;}
    return &overlap_marks[h];
}
static void overlap_note_write(image_entry *e) {
    if(!overlap_enabled()||!e->texture.mem||!e->texture.L.size)return;
    e->overlap_written=draw_count+1;if(e->overlap_noted==render_passes+1)return;
    e->overlap_noted=render_passes+1;uintptr_t a=(uintptr_t)e->texture.mem,b=a+e->texture.L.size;
    for(uintptr_t k=a>>16;k<=(b-1)>>16;k++)*overlap_slot(k)=(overlap_mark){k,a,b,e->serial,presents,draw_count+1,e->texture.d.format,e->texture.d.width,e->texture.d.height};
}
/* Returns 1 for such a sample. TOTK_VK_OVERLAP=2 (experiment): the caller then clears a colour image to magenta before
   it is sampled (overlap_poison): whether what these samples read shows on screen at all. */
static int overlap_check_read(image_entry *e) {
    if(!overlap_enabled()||!e->texture.mem||!e->texture.L.size)return 0;
    int hit=0;
    static struct { uint32_t wf,ww,wh,rf,rw,rh; uint64_t n; } top[64];static unsigned ntop;static uint64_t events,last;
    uintptr_t a=(uintptr_t)e->texture.mem,b=a+e->texture.L.size;
    for(uintptr_t k=a>>16;k<=(b-1)>>16;k++){overlap_mark *m=overlap_slot(k);
        if(m->block!=k||m->serial==e->serial||m->b<=a||m->a>=b||m->frame+1<presents)continue;
        /* its own render is as new or newer */
        if(e->overlap_written>=m->draw)continue;
        events++;unsigned i=0;while(i<ntop&&(top[i].wf!=m->fmt||top[i].ww!=m->w||top[i].wh!=m->h||top[i].rf!=e->texture.d.format||top[i].rw!=e->texture.d.width||top[i].rh!=e->texture.d.height))i++;
        if(i==ntop&&ntop<64){top[ntop++]=(typeof(top[0])){m->fmt,m->w,m->h,e->texture.d.format,e->texture.d.width,e->texture.d.height,0};
            static unsigned said;if(said++<40)LOG("[vulkan-overlap] new pair: written fmt %#x %ux%u at %#llx+%#llx (serial %llu, draw %llu), sampled fmt %#x %ux%u lv%u at %#llx+%#llx (serial %llu, own write draw %llu, gpu_written %d)",
                m->fmt,m->w,m->h,(unsigned long long)m->a,(unsigned long long)(m->b-m->a),(unsigned long long)m->serial,(unsigned long long)m->draw,e->texture.d.format,e->texture.d.width,e->texture.d.height,e->texture.d.levels,
                (unsigned long long)a,(unsigned long long)(b-a),(unsigned long long)e->serial,(unsigned long long)e->overlap_written,e->gpu_written);}
        if(i<ntop)top[i].n++;
        hit=1;break;}
    if(presents>=last+600){last=presents;
        LOG("[vulkan-overlap] last 600 frames: %llu samples of an image another image (other layout) rendered over since its own write",(unsigned long long)events);
        for(unsigned j=0;j<ntop;j++)if(top[j].n)LOG("[vulkan-overlap]   %llu x: written as fmt %#x %ux%u, sampled as fmt %#x %ux%u",(unsigned long long)top[j].n,top[j].wf,top[j].ww,top[j].wh,top[j].rf,top[j].rw,top[j].rh);
        events=0;ntop=0;}
    return hit;
}
static void overlap_poison(image_entry *e) {
    static int mode=-1;if(mode<0){const char *x=getenv("TOTK_VK_OVERLAP");mode=x?atoi(x):0;}
    if(mode!=2||e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT||e->texture.d.target==NVN_TEXTURE_TARGET_3D)return;
    graphics_pass_end();image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue magenta={.float32={1,0,1,1}};VkImageSubresourceRange r={VK_IMAGE_ASPECT_COLOR_BIT,0,e->texture.d.levels,0,e->texture.L.layers};
    vkCmdClearColorImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&magenta,1,&r);
}
static void image_gpu_written(image_entry *e) {
    const tex_t *t=&e->texture;
    overlap_note_write(e);
    { if(t->mem&&gpu_watch_hit((uintptr_t)t->mem,t->L.size)){static unsigned said;Dl_info info;void *ret=__builtin_return_address(0);
        if(said++<2000)LOG("[vulkan-watch] frame %llu GPU write serial %llu fmt %#x %ux%u lv%u by %s (site libtotk+%#lx)",(unsigned long long)presents,(unsigned long long)t->serial,t->d.format,t->d.width,t->d.height,t->d.levels,
            gpu_write_shaders?gpu_write_shaders:"-",dladdr(ret,&info)&&info.dli_fbase?(unsigned long)((uintptr_t)ret-(uintptr_t)info.dli_fbase):0ul);
        /* TOTK_VK_WATCH_FREEZE=1 (or =<shader key>): the first draw into it (by that shader) requests a freeze-frame (frz.sh) */
        static int froze;const char *fz=getenv("TOTK_VK_WATCH_FREEZE"),*dir=getenv("TOTK_FILES_DIR");
        if(!froze&&gpu_write_shaders&&fz&&*fz&&strcmp(fz,"0")&&(!strcmp(fz,"1")||strstr(gpu_write_shaders,fz))&&dir){froze=1;char path[600];snprintf(path,sizeof path,"%s/freeze",dir);FILE *f=fopen(path,"w");if(f)fclose(f);LOG("[vulkan-watch] freeze requested");}} }
    // Existing baselines represent CPU versions actually read; repeated GPU marking cannot consume unread CPU changes.
    if(t->has_layout&&t->mem&&!e->source_valid){nvn_dirty_protect(t->mem,t->L.size);image_source_baseline(e,t->mem,nvn_dirty_now());}
    TRACEIMG(&e->texture,"GPU write (entry %llu) by %s",(unsigned long long)e->serial,gpu_write_shaders?gpu_write_shaders:"-");
    if(!e->gpu_written||e->gpu_write_frame!=presents)e->write_streak=e->gpu_written&&e->gpu_write_frame+1==presents?e->write_streak+1:1;
    e->populated=1;e->stamp=0;e->gpu_written=1;e->gpu_write_frame=presents;e->writeback_due=1;
}
/* TOTK: an image the GPU rendered and then left unchanged goes back to guest memory, in the texture's own layout, as
   on the console, where GPU and CPU read the same bytes. The game moves its terrain tiles (the normal maps it
   renders once per tile, 260x260 RG8 with 9 levels) with the CPU when it compacts the tile pool; without this a
   moved tile reads zeros from our guest memory, and the grass placement that reads it drops every blade some 20 s
   after loading. An image goes back once it has gone two frames without a GPU write; one rewritten after two
   write-backs (a periodic target) is left alone, and so are per-frame targets, which never go two frames unwritten.
   If the CPU wrote the range between the copy and its completion (the game reused the memory) nothing is written. */
typedef struct image_writeback { nvn_texdesc d; uint8_t *mem; uint64_t size, stamp; unsigned layers; int dirty; } image_writeback;
#define WRITEBACK_MAX_IMAGE (4u << 20)
#define WRITEBACK_MAX_FRAME (8u << 20)
static uint64_t writeback_images, writeback_bytes, writeback_dropped;
static int image_writeback_wanted(const image_entry *e) {
    const tex_t *t=&e->texture;int bw,bh;
    return e->writeback_due&&e->gpu_written&&presents>=e->gpu_write_frame+2&&e->writebacks<2&&e->storage_keyed&&e->holders&&
        e->aspect==VK_IMAGE_ASPECT_COLOR_BIT&&!e->block_aux&&t->mem&&t->L.size&&t->L.size<=WRITEBACK_MAX_IMAGE&&t->d.target!=NVN_TEXTURE_TARGET_3D&&
        nvn_fmt_bpb(t->d.format,&bw,&bh)&&bw==1&&bh==1&&!(t->d.flags&(NVN_TEXTURE_FLAGS_LINEAR|NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
static void image_writeback_record(image_entry *e) {
    const tex_t *t=&e->texture;int bw,bh;
    /* which images the CPU reads back, and who wrote them */
    { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_WRITEBACKLOG");on=x&&atoi(x);}
      if(on){static struct { uint64_t serial; unsigned n; } seen[32];static unsigned ns;unsigned i=0;while(i<ns&&seen[i].serial!=e->serial)i++;
        if(i==ns&&ns<32)seen[ns++]=(typeof(seen[0])){e->serial,0};if(i<32){seen[i].n++;if(seen[i].n<=2||seen[i].n%300==0)
          LOG("[vulkan-writebacklog] frame %llu serial %llu %ux%u lv%u fmt %#x %llu KiB written back (%u times): writer %s (stage %#x, draw frame %llu)",(unsigned long long)presents,(unsigned long long)e->serial,t->d.width,t->d.height,t->d.levels,t->d.format,(unsigned long long)(t->L.size>>10),seen[i].n,e->write_shader?e->write_shader:"?",(unsigned)e->write_stage,(unsigned long long)e->write_frame);}} }unsigned bpb=nvn_fmt_bpb(t->d.format,&bw,&bh),n=t->d.levels*t->L.layers;
    VkBufferImageCopy *regions=calloc(n,sizeof *regions);if(!regions)die("Vulkan write-back regions");uint64_t total=0;unsigned k=0;
    for(unsigned layer=0;layer<t->L.layers;layer++)for(unsigned level=0;level<t->d.levels;level++){uint32_t w=dimension(t->d.width,level),h=dimension(t->d.height,level);
        regions[k++]=(VkBufferImageCopy){.bufferOffset=total,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageExtent={w,h,1}};total+=(uint64_t)w*bpb*h;}
    graphics_pass_end();image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    post_op *p=post_new(6);p->output=buffer_create(total);
    vkCmdCopyImageToBuffer(command,e->image,VK_IMAGE_LAYOUT_GENERAL,p->output.buffer,n,regions);free(regions);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->output.buffer,.size=total};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    image_writeback *w=malloc(sizeof *w);if(!w)die("Vulkan write-back allocation");
    /* like image_upload: protect before taking the generation, so a CPU write after it is seen */
    nvn_dirty_protect(t->mem,t->L.size);*w=(image_writeback){t->d,t->mem,t->L.size,nvn_dirty_now(),t->L.layers};p->writeback=w;
    e->writeback_due=0;e->writebacks++;writeback_images++;writeback_bytes+=total;
}
/* Once a frame, in its first batch (mtl_sub_begin), with the queue lock held. */
static void image_writeback_sweep(void) {
    uint64_t bytes=0;
    for(image_entry *e=image_list;e&&bytes<WRITEBACK_MAX_FRAME;e=e->next)if(image_writeback_wanted(e)){bytes+=e->texture.L.size;image_writeback_record(e);}
    if(presents&&!(presents%600))LOG("[vulkan-writeback] frame %llu: %llu images (%.1f MiB) written back, %llu dropped (the CPU wrote the range first)",
        (unsigned long long)presents,(unsigned long long)writeback_images,writeback_bytes/1048576.,(unsigned long long)writeback_dropped);
}
/* At completion, for every write-back of the batch before any of its posts publishes: was the range written by the CPU
   since the copy? Publications mark the dirty tracker's blocks (64 KiB in a zero-copy pool), and a block can hold the
   edges of two images: checked before this batch publishes, and on the blocks wholly inside the image. */
static void image_writeback_check(post_op *p) {
    image_writeback *w=p->writeback;if(!w)return;
    /* only the blocks wholly inside the image when there are any: an edge block is shared with the neighbour's storage */
    uintptr_t lo=((uintptr_t)w->mem+65535)&~(uintptr_t)65535,hi=((uintptr_t)w->mem+w->size)&~(uintptr_t)65535;
    w->dirty=hi>lo?nvn_dirty_since((void*)lo,hi-lo,w->stamp):nvn_dirty_since(w->mem,w->size,w->stamp);
}
/* Then, in order: the copied levels into guest memory, in the texture's layout. */
static void image_writeback_publish(post_op *p) {
    image_writeback *w=p->writeback;p->writeback=NULL;if(!w)return;
    if(w->dirty){writeback_dropped++;free(w);return;}
    int bw,bh;unsigned bpb=nvn_fmt_bpb(w->d.format,&bw,&bh);const uint8_t *linear=p->output.mapped;
    /* the GPU's bytes landing in guest memory, not a CPU write: counted as one, the image's next use would find its
       pages written since its baseline and upload these (older) bytes over what the GPU rendered after the copy (the
       Metal backend's write-back does the same) */
    void *tok=nvn_dirty_tracked(w->mem,w->size)?nvn_dirty_gpu_write_begin(w->mem,w->size):NULL;
    if(!tok)dm_guest_write_prepare(w->mem,w->size);
    for(unsigned layer=0;layer<w->layers;layer++)for(unsigned level=0;level<w->d.levels;level++){uint32_t width=dimension(w->d.width,level),h=dimension(w->d.height,level);
        if(nvn_swizzle(&w->d,level,layer,linear,(size_t)width*bpb,w->mem))die("Vulkan write-back swizzle failed (format %#x %ux%u level %u)",w->d.format,w->d.width,w->d.height,level);
        linear+=(uint64_t)width*bpb*h;}
    nvn_dirty_gpu_write_end(tok);
    gpu_watch_changed("image-writeback",(uintptr_t)w->mem);
    free(w);
}
// Read-only counter: an image the GPU rendered is uploaded whole from guest memory again, overwriting freshly
// rendered content with older bytes.
static uint64_t image_gpu_overwrites;
// How often the game bound a handle whose sampled texture the port cannot resolve (falls back to the 1x1 stand-in).
static uint64_t gpu_unregistered_textures;
/* frame profile: bytes copied in and the translation thread's time in uploads (deswizzle, staging) */
static uint64_t texture_upload_bytes,texture_upload_ns;
/* upload census (TOTK_VK_UPLOADLOG=1): which images are uploaded from guest memory, how often inside a render pass (the
   upload ends it and the pass restarts), and whether the GPU had touched the image in the current command buffer
   (if not, the copy could go into a prelude command buffer ahead of the chunk instead of ending the pass). */
static void graphics_pass_dims(uint32_t *w,uint32_t *h,unsigned *att);
static void upload_census(image_entry *e) {
    static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_UPLOADLOG");on=x&&atoi(x);}if(!on)return;
    static struct { uint64_t serial; uint32_t w,h,fmt,levels; uint64_t bytes,n,inpass,touched,pw,ph; unsigned patt; } rows[64]; static unsigned nrows; static uint64_t last;
    if(presents>=last+300){
        if(last)LOG("[vulkan-uploadlog] %.2f uploads a frame went into the chunk prelude",prelude_uploads/300.);prelude_uploads=0;
        if(last)for(unsigned k=0;k<nrows;k++)if(rows[k].n>=3)LOG("[vulkan-uploadlog] serial %llu %ux%u lv%u fmt %#x %llu KiB: %.2f uploads a frame, %.2f inside a pass (last %ux%u, %u attachments), %.2f after a GPU touch in the chunk",
            (unsigned long long)rows[k].serial,rows[k].w,rows[k].h,rows[k].levels,rows[k].fmt,(unsigned long long)(rows[k].bytes>>10),rows[k].n/300.,rows[k].inpass/300.,(unsigned)rows[k].pw,(unsigned)rows[k].ph,rows[k].patt,rows[k].touched/300.);
        last=presents;nrows=0;}
    const tex_t *t=&e->texture;unsigned i=0;while(i<nrows&&rows[i].serial!=e->serial)i++;
    if(i==nrows){if(nrows==64)return;rows[nrows++]=(typeof(rows[0])){e->serial,t->d.width,t->d.height,t->d.format,t->d.levels,t->L.size};}
    rows[i].n++;if(graphics_pass_open()){rows[i].inpass++;uint32_t w,h;unsigned a;graphics_pass_dims(&w,&h,&a);rows[i].pw=w;rows[i].ph=h;rows[i].patt=a;}
    if(e->touched_chunk==chunk_epoch)rows[i].touched++;
}
static void image_upload(image_entry *e) {
    const tex_t *t=&e->texture;
    /* a pending clear comes first: it defines the contents, as an immediate clear would have */
    if(e->pclear)image_clear_flush(e);
    int pending=t->mem && snapshot_pending((uintptr_t)t->mem,t->L.size);
    if(e->populated&&!pending){
        if(t->has_layout&&t->mem){
            if(e->source_valid){int tracked=nvn_dirty_tracked(t->mem,t->L.size);
                if(tracked&&e->source_tracked){uint64_t now=nvn_dirty_now();int clean=now==e->source_seen||!nvn_dirty_since(t->mem,t->L.size,e->source_stamp);e->source_seen=now;if(clean)return;}
                else if(!tracked&&!e->source_tracked&&image_source_hash(t->mem,t->L.size)==e->source_hash)return;
            }
        }else if(!e->stamp||!nvn_dirty_since(t->mem,t->L.size,e->stamp))return;
    }
    TRACEIMG(t,"upload from guest memory (entry %llu populated %d gpu_written %d pclear %d)",(unsigned long long)e->serial,e->populated,e->gpu_written,e->pclear);
    if(!t->mem)die("Vulkan texture has no source storage");
    if((e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))die("Vulkan combined depth/stencil CPU upload pending");
    // Like the Metal backend, protect before taking the generation/reading to avoid a window for untracked concurrent
    // writes.
    uint64_t up_t0=dm_android_clock_ns(CLOCK_MONOTONIC);upload_census(e);
    /* inside a pass, into an image the GPU has not touched in this chunk: the copy goes into the chunk's prelude, ahead of
       everything recorded so far, and the pass stays open (the barrier from its last use is recorded there too) */
    VkCommandBuffer chunk_command=command;
    if(graphics_pass_open()&&e->touched_chunk!=chunk_epoch&&prelude_on()){command=prelude_begin();prelude_recording=1;prelude_uploads++;}
    else graphics_pass_end();
    nvn_dirty_protect(t->mem,t->L.size);uint64_t stamp=nvn_dirty_now();image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);uint8_t *shadow=NULL;const uint8_t *storage=t->mem;
    // Uploads must see preceding copies in the batch without publishing guest memory early.
    // Freeze untracked packaged sources too; never upload old bytes and then baseline a concurrent newer write.
    if(pending||(t->has_layout&&!nvn_dirty_tracked(t->mem,t->L.size))){shadow=malloc(t->L.size);if(!shadow||snapshot_read((uintptr_t)t->mem,shadow,t->L.size))die("Vulkan texture snapshot read");storage=shadow;}
    /* TOTK_VK_UPLOADSPLIT=1: where an upload's time goes. The first world frame after a load, 80 uploads / 273 MB:
       staging buffers 27 ms (a buffer_create per mip level), deswizzle 48 ms, recording 0.1 ms. A worker pool for
       the rows and a shared staging arena measure no gain. */
    static uint64_t up_alloc_ns,up_move_ns,up_copy_ns,up_n,up_bytes;uint64_t up_t1=up_t0;
    static int upsplit=-1;if(upsplit<0){const char *e=getenv("TOTK_VK_UPLOADSPLIT");upsplit=e&&atoi(e)>0;}
    for(unsigned layer=0;layer<t->L.layers;layer++)for(unsigned level=0;level<t->d.levels;level++){
        int bw,bh;uint32_t bpb=nvn_fmt_bpb(t->d.format,&bw,&bh),w=dimension(t->d.width,level),h=dimension(t->d.height,level),d=t->d.target==NVN_TEXTURE_TARGET_3D?dimension(t->d.depth,level):1;
        uint32_t pitch=(w+bw-1)/bw*bpb,rows=(h+bh-1)/bh;uint64_t bytes=(uint64_t)pitch*rows*d;
        post_op *p=post_new(3);p->source=buffer_create(bytes);uint8_t *staging=(uint8_t *)p->source.mapped;
        if(upsplit){uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);up_alloc_ns+=now-up_t1;up_t1=now;}
        if(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR|NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)){
            const uint8_t *src=storage+nvn_tex_level_offset(&t->d,level)+layer*t->L.layer_size;size_t stride=t->d.stride?t->d.stride:pitch;
            for(unsigned z=0;z<d;z++)for(unsigned y=0;y<rows;y++)memcpy(staging+(z*rows+y)*pitch,src+(z*rows+y)*stride,pitch);
        }else if(nvn_deswizzle(&t->d,level,layer,storage,staging,pitch))die("Vulkan texture deswizzle failed");
        if(upsplit){uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);up_move_ns+=now-up_t1;up_t1=now;}
        VkBufferImageCopy r={.imageSubresource={e->aspect,level,layer,1},.imageExtent={w,h,d}};
        vkCmdCopyBufferToImage(command,p->source.buffer,e->image,VK_IMAGE_LAYOUT_GENERAL,1,&r);
        if(upsplit){uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);up_copy_ns+=now-up_t1;up_t1=now;}
    }
    if(upsplit){up_n++;up_bytes+=t->L.size;if(up_n%40==0)LOG("[vulkan-uploadsplit] %llu uploads %.1f MB: staging alloc %.1f ms, deswizzle/memcpy %.1f ms, copy record %.1f ms (cumulative)",(unsigned long long)up_n,up_bytes/1048576.0,up_alloc_ns/1e6,up_move_ns/1e6,up_copy_ns/1e6);}
    if(e->gpu_written){image_gpu_overwrites++;if(image_gpu_overwrites<=6||!(image_gpu_overwrites%64))LOG("[vulkan-upload] 覆盖 GPU 已渲染内容 serial=%llu %ux%u format=%#x bytes=%llu 第 %llu 次（客体内存里没有 GPU 那次渲染的结果）",(unsigned long long)t->serial,t->d.width,t->d.height,t->d.format,(unsigned long long)t->L.size,(unsigned long long)image_gpu_overwrites);}
    /* the read-after-upload barrier goes into the prelude as well: the draw that binds the image then finds a read
       after a read and keeps its pass (otherwise that barrier would end the pass just as the upload would) */
    if(prelude_recording){image_access(e,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);command=chunk_command;prelude_recording=0;}
    image_source_baseline(e,storage,stamp);free(shadow);e->populated=1;e->stamp=stamp;texture_uploads++;texture_upload_bytes+=t->L.size;texture_upload_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-up_t0;
}
static VkImageSubresourceRange texture_range(image_entry *e,const view_t *v) {
    VkImageSubresourceRange r={e->aspect,v&&v->set_levels?v->base_level:0,1,v&&v->set_layers?v->min_layer:0,1};
    /* 3D: depth slices */
    uint32_t layers=e->texture.d.target==NVN_TEXTURE_TARGET_3D?dimension(e->texture.d.depth,r.baseMipLevel):e->texture.L.layers;
    if(r.baseMipLevel>=e->texture.d.levels||r.baseArrayLayer>=layers)
        die("Vulkan texture view bounds: base level %u of %u, layer %u of %u (texture %ux%ux%u target %u format %#x serial %llu; view set_levels %u set_layers %u)",
            r.baseMipLevel,e->texture.d.levels,r.baseArrayLayer,layers,e->texture.d.width,e->texture.d.height,e->texture.d.depth,e->texture.d.target,e->texture.d.format,
            (unsigned long long)e->texture.serial,v?v->set_levels:0,v?v->set_layers:0);
    return r;
}
/* A colour clear through a one-attachment render pass, for what vkCmdClearColorImage cannot do: one depth slice of a 3D
   image (it clears every slice of the mip; the slice is a 2D view, the image is 2D_ARRAY_COMPATIBLE) and a scissored
   clear (the pass loads the image and vkCmdClearAttachments clears the rectangle). Render passes are kept per format and
   load op, framebuffers with the view they draw into. format: the view's (the format the game renders with, which on
   a mutable image may differ from the image's own: the clear value is encoded in it). */
static void clear_color_pass(image_entry *e,VkImageSubresourceRange r,const float rgba[4],VkRect2D rect,VkFormat format) {
    uint32_t w=dimension(e->texture.d.width,r.baseMipLevel),h=dimension(e->texture.d.height,r.baseMipLevel);
    int whole=!rect.offset.x&&!rect.offset.y&&rect.extent.width>=w&&rect.extent.height>=h;
    VkAttachmentLoadOp load=whole?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD;
    static struct { VkFormat format; VkAttachmentLoadOp load; VkRenderPass pass; } passes[32]; static unsigned npasses;
    VkRenderPass pass=VK_NULL_HANDLE;
    if(!format||!e->mutable_format)format=e->format;
    for(unsigned i=0;i<npasses;i++)if(passes[i].format==format&&passes[i].load==load)pass=passes[i].pass;
    if(!pass){
        VkAttachmentDescription a={.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=load,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
        VkRenderPassCreateInfo ci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&a,.subpassCount=1,.pSubpasses=&sub};
        VK_CHECK(vkCreateRenderPass(device,&ci,NULL,&pass));if(npasses<32)passes[npasses++]=(typeof(passes[0])){format,load,pass};else die("Vulkan clear render passes");
    }
    /* destroyed with the view */
    r.layerCount=1;r.levelCount=1;cached_image_view *cv=image_view_formatted(e,VK_IMAGE_VIEW_TYPE_2D,r,(VkComponentMapping){0},format);VkFramebuffer fb=cv->clear_fb[whole];
    if(!fb){VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,.pAttachments=&cv->view,.width=w,.height=h,.layers=1};
        VK_CHECK(vkCreateFramebuffer(device,&fi,NULL,&fb));cv->clear_fb[whole]=fb;}
    graphics_pass_end();image_access(e,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkClearValue value;memcpy(value.color.float32,rgba,4*sizeof(float));
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,.renderArea={{0,0},{w,h}},.clearValueCount=whole,.pClearValues=&value};
    vkCmdBeginRenderPass(command,&bi,VK_SUBPASS_CONTENTS_INLINE);
    if(!whole){VkClearAttachment ca={VK_IMAGE_ASPECT_COLOR_BIT,0,value};VkClearRect cr={rect,0,1};vkCmdClearAttachments(command,1,&ca,1,&cr);}
    vkCmdEndRenderPass(command);image_gpu_written(e);
}
/* Deferred clears (Eden's DeferColorClear, DXVK's deferred clears): a whole-image clear of a render target that the
   open pass does not use is remembered on the image instead of ending the pass for a transfer clear (~26 a frame:
   each a pass end, a clear, a barrier and a pass started with LOAD). The next pass that renders to the same
   subresource loads it with VK_ATTACHMENT_LOAD_OP_CLEAR (draw_graphics_inner); any other use (image_access) or the
   batch commit does the clear first. */
static image_entry *pclear_list[64]; static unsigned pclear_count; static uint64_t pclear_deferred, pclear_loadops, pclear_flushed, pclear_viewformat;
/* format: the one the clear value is encoded in -- the format the game renders the image with, which on a mutable
   image may be another than the image's own (clear_color) */
static int image_clear_defer(image_entry *e,VkImageSubresourceRange r,VkClearValue v,VkFormat format) {
    if(graphics_image_is_attachment(e)||r.levelCount!=1||r.layerCount!=1)return 0;
    /* another subresource pending: that one first */
    if(e->pclear&&memcmp(&e->pclear_range,&r,sizeof r))image_clear_flush(e);
    if(!e->pclear){if(pclear_count==64)return 0;pclear_list[pclear_count++]=e;}
    e->pclear=1;e->pclear_value=v;e->pclear_range=r;e->pclear_format=format;pclear_deferred++;pclear_viewformat+=format!=e->format;return 1;
}
static void pclear_forget(image_entry *e) {
    e->pclear=0;for(unsigned i=0;i<pclear_count;i++)if(pclear_list[i]==e){pclear_list[i]=pclear_list[--pclear_count];break;}
}
/* a deferred clear at once, as a transfer outside any pass */
static void image_clear_flush(image_entry *e) {
    if(!e->pclear)return;pclear_forget(e);pclear_flushed++;TRACEIMG(&e->texture,"deferred clear done");
    /* encoded in a view format: cleared through a view of it (clear_color_pass) */
    if(e->pclear_format!=e->format){
        uint32_t w=dimension(e->texture.d.width,e->pclear_range.baseMipLevel),h=dimension(e->texture.d.height,e->pclear_range.baseMipLevel);
        clear_color_pass(e,e->pclear_range,e->pclear_value.color.float32,(VkRect2D){{0,0},{w,h}},e->pclear_format);return;
    }
    graphics_pass_end();image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    if(e->pclear_range.aspectMask&VK_IMAGE_ASPECT_COLOR_BIT)vkCmdClearColorImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&e->pclear_value.color,1,&e->pclear_range);
    else vkCmdClearDepthStencilImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&e->pclear_value.depthStencil,1,&e->pclear_range);
    image_gpu_written(e);
}
static void pclear_flush_all(void) { while(pclear_count)image_clear_flush(pclear_list[pclear_count-1]); }
/* draw_graphics_inner, building a pass: an attachment with a deferred clear of the same subresource loads with
   CLEAR (its value goes to graphics_clear_values); another subresource gets its clear done first. */
static VkClearValue graphics_clear_values[9]; static image_entry *pclear_attach_e[9]; static unsigned pclear_attach_i[9], pclear_attach_n;
static void pclear_attach(image_entry *e,VkImageSubresourceRange range,VkAttachmentDescription *a,unsigned index) {
    if(!e->pclear||index>=9)return;const VkImageSubresourceRange *p=&e->pclear_range;
    /* the clear value is encoded in the format it was given in (clear_color): a pass viewing the image as another one does it first */
    if(a->format!=e->pclear_format){image_clear_flush(e);return;}
    if(range.baseMipLevel!=p->baseMipLevel||range.baseArrayLayer!=p->baseArrayLayer||range.levelCount!=1||range.layerCount!=1){image_clear_flush(e);return;}
    if(p->aspectMask&VK_IMAGE_ASPECT_COLOR_BIT)a->loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    else{if(p->aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)a->loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;if(p->aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)a->stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;}
    graphics_clear_values[index]=e->pclear_value;pclear_attach_i[pclear_attach_n]=index;pclear_attach_e[pclear_attach_n++]=e;
}
static VkFormat attachment_format(image_entry *e,const tex_t *t,const view_t *v);
static void clear_color(const p_clear_color *c) {
    PASSLOG("clear colour %d of %d: serial %llu rgba %g,%g,%g,%g mask %#x",c->index,state.rt.n,c->index>=0&&c->index<state.rt.n&&state.rt.color[c->index]?(unsigned long long)state.rt.color[c->index]->serial:0ull,c->rgba[0],c->rgba[1],c->rgba[2],c->rgba[3],c->mask);
    if(c->index<0||c->index>=state.rt.n||!state.rt.color[c->index])return;
    if((c->mask & 15)!=15)die("Vulkan masked color clear pending");
    const view_t *view=state.rt.has_cview[c->index]?&state.rt.cview[c->index]:NULL;
    image_entry *e=image_get(state.rt.color[c->index]);VkImageSubresourceRange r=texture_range(e,view);
    /* the clear value is encoded in the format the game renders with: a mutable image first made in another format
       (an R32F image cleared as an RGBA8 target to 1,1,1,1 must read ff ff ff ff, not the float 1.0, or the shrine
       doors' portal mask goes off and a green square is drawn over them) clears through a view of that format */
    VkFormat af=attachment_format(e,state.rt.color[c->index],view);
    if(e->format>=VK_FORMAT_BC1_RGB_UNORM_BLOCK&&e->format<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK)die("Vulkan clear of compressed render target %#x",e->texture.d.format);
    // A partial scissor must preserve uncovered pixels; never substitute a whole-image clear.
    int w=dimension(e->texture.d.width,r.baseMipLevel),h=dimension(e->texture.d.height,r.baseMipLevel);
    if(state.scissor.w>0&&(state.scissor.x||state.scissor.y||state.scissor.w<w||state.scissor.h<h)){
        int x=state.scissor.x>0?state.scissor.x:0,y=state.scissor.y>0?state.scissor.y:0,x1=state.scissor.x+state.scissor.w,y1=state.scissor.y+state.scissor.h;
        if(x1>w)x1=w;if(y1>h)y1=h;if(x1<=x||y1<=y)return;
        clear_color_pass(e,r,c->rgba,(VkRect2D){{x,y},{(uint32_t)(x1-x),(uint32_t)(y1-y)}},af);return;
    }
    if(e->texture.d.target==NVN_TEXTURE_TARGET_3D){clear_color_pass(e,r,c->rgba,(VkRect2D){{0,0},{(uint32_t)w,(uint32_t)h}},af);return;}
    TRACEIMG(&e->texture,"clear colour %g,%g,%g,%g (entry %llu) via %s",c->rgba[0],c->rgba[1],c->rgba[2],c->rgba[3],(unsigned long long)e->serial,state.rt.color[c->index]->serial==e->serial?"own":"alias");
    /* deferred in the view's format */
    { VkClearValue v;memcpy(v.color.float32,c->rgba,sizeof c->rgba);if(image_clear_defer(e,r,v,af))return; }
    if(af!=e->format){clear_color_pass(e,r,c->rgba,(VkRect2D){{0,0},{(uint32_t)w,(uint32_t)h}},af);return;}
    graphics_pass_end();VkClearColorValue color;memcpy(color.float32,c->rgba,sizeof c->rgba);image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);vkCmdClearColorImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&r);image_gpu_written(e);
}
static void clear_depth(const p_clear_ds *c) {
    if(!state.rt.depth)return;image_entry *e=image_get(state.rt.depth);VkImageSubresourceRange r=texture_range(e,state.rt.has_dview?&state.rt.dview:NULL);r.aspectMask=0;
    if(c->depth_mask)r.aspectMask|=VK_IMAGE_ASPECT_DEPTH_BIT;if(c->stencil_mask){if(c->stencil_mask!=255)die("Vulkan partial stencil clear pending");r.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;}
    r.aspectMask&=e->aspect;if(!r.aspectMask)return;
    { VkClearValue v={.depthStencil={c->depth,c->stencil}};if(image_clear_defer(e,r,v,e->format))return; }
    graphics_pass_end();VkClearDepthStencilValue value={c->depth,c->stencil};image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);vkCmdClearDepthStencilImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&r);image_gpu_written(e);
}
static void copy_buffer_texture(const p_copy_b2t *c) {
    if(c->r.width<=0||c->r.height<=0||c->r.depth<=0)return;
    snapshot_prepare();
    dependencies_prepare_copy_texture(c);
    graphics_pass_end();image_entry *e=image_get(c->tex);image_upload(e);image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkImageSubresourceRange range=texture_range(e,c->has_view?&c->view:NULL);
    int bw,bh;unsigned bpb=nvn_fmt_bpb(c->tex->d.format,&bw,&bh),pitch=(c->r.width+bw-1)/bw*bpb,rows=(c->r.height+bh-1)/bh;uint64_t bytes=(uint64_t)pitch*rows*c->r.depth;
    uint64_t stride=state.copy_stride>0?state.copy_stride:pitch;if(stride<pitch)die("Vulkan texture copy stride");post_op *p=post_new(3);p->source=buffer_create(bytes);
    for(int z=0;z<c->r.depth;z++)for(unsigned y=0;y<rows;y++)if(snapshot_read(c->src+(z*rows+y)*stride,(uint8_t*)p->source.mapped+(z*rows+y)*pitch,pitch))die("Vulkan texture copy source");
    int is3d=c->tex->d.target==NVN_TEXTURE_TARGET_3D;VkBufferImageCopy copy={.imageSubresource={e->aspect,range.baseMipLevel,range.baseArrayLayer+(is3d?0:c->r.zoffset),is3d?1:c->r.depth},.imageOffset={c->r.xoffset,c->r.yoffset,is3d?c->r.zoffset:0},.imageExtent={c->r.width,c->r.height,is3d?c->r.depth:1}};
    vkCmdCopyBufferToImage(command,p->source.buffer,e->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);image_gpu_written(e);
}
/* nvnCommandBufferCopyTextureToBuffer (TOTK): the region into a staging buffer laid out with the copy stride, published to
   guest memory when the GPU completes and forwarded to later GPU readers of that memory in the batch. Bytes between rows
   are seeded from the current guest contents so the publication does not overwrite them. */
static void copy_texture_buffer(const p_copy_b2t *c) {
    if(c->r.width<=0||c->r.height<=0||c->r.depth<=0)return;
    snapshot_prepare();dependencies_prepare_texture(c->tex);
    graphics_pass_end();image_entry *e=image_get(c->tex);image_upload(e);
    if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)die("Vulkan depth/stencil texture-to-buffer copy pending (format %#x)",c->tex->d.format);
    VkImageSubresourceRange range=texture_range(e,c->has_view?&c->view:NULL);
    int bw,bh;unsigned bpb=nvn_fmt_bpb(c->tex->d.format,&bw,&bh),pitch=(c->r.width+bw-1)/bw*bpb,rows=(c->r.height+bh-1)/bh;
    uint64_t stride=state.copy_stride>0?state.copy_stride:pitch;if(stride<pitch||stride%bpb)die("Vulkan texture-to-buffer copy stride %llu (pitch %u)",(unsigned long long)stride,pitch);
    uint64_t bytes=stride*rows*(uint64_t)c->r.depth-(stride-pitch);
    if(!mtl_addr(c->src,bytes,NULL,NULL))die("Vulkan texture-to-buffer destination outside pools: %#llx + %llu",(unsigned long long)c->src,(unsigned long long)bytes);
    post_op *p=post_new(0);p->site=POST0_SITE_TEX_TO_BUF;p->destination=(void*)(uintptr_t)c->src;p->size=bytes;p->output=buffer_create(bytes);post_register_range(p);
    if(stride!=pitch&&snapshot_read(c->src,p->output.mapped,bytes))die("Vulkan texture-to-buffer destination unreadable");
    image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    int is3d=c->tex->d.target==NVN_TEXTURE_TARGET_3D;
    VkBufferImageCopy copy={.bufferRowLength=(uint32_t)(stride/bpb*bw),.bufferImageHeight=rows*bh,.imageSubresource={e->aspect,range.baseMipLevel,range.baseArrayLayer+(is3d?0:c->r.zoffset),is3d?1:c->r.depth},
        .imageOffset={c->r.xoffset,c->r.yoffset,is3d?c->r.zoffset:0},.imageExtent={c->r.width,c->r.height,is3d?c->r.depth:1}};
    vkCmdCopyImageToBuffer(command,e->image,VK_IMAGE_LAYOUT_GENERAL,p->output.buffer,1,&copy);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->output.buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    forward_mark_gpu_recorded(p,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,1);
    static unsigned said;if(said++<4)LOG("[vulkan] texture-to-buffer copy: serial %llu format %#x %dx%dx%d -> %#llx stride %llu",(unsigned long long)c->tex->serial,c->tex->d.format,c->r.width,c->r.height,c->r.depth,(unsigned long long)c->src,(unsigned long long)stride);
    { static unsigned shown;if(gpu_watch_hit((uintptr_t)c->src,bytes)&&shown++<200)
        LOG("[vulkan-watch] frame %llu texture-to-buffer serial %llu fmt %#x %ux%u lv%u mem %#llx+%llu region %d,%d %dx%d -> %#llx: gpu_written %d (frame %llu) populated %d source_valid %d",
            (unsigned long long)presents,(unsigned long long)c->tex->serial,c->tex->d.format,c->tex->d.width,c->tex->d.height,c->tex->d.levels,(unsigned long long)(uintptr_t)c->tex->mem,(unsigned long long)c->tex->L.size,
            c->r.xoffset,c->r.yoffset,c->r.width,c->r.height,(unsigned long long)c->src,e->gpu_written,(unsigned long long)e->gpu_write_frame,e->populated,e->source_valid); }
}
enum { TEXTURE_CLEAR_UNORM, TEXTURE_CLEAR_SNORM, TEXTURE_CLEAR_UINT, TEXTURE_CLEAR_SINT, TEXTURE_CLEAR_FLOAT };
typedef struct { unsigned channels,bits,kind; } texture_clear_format;
static texture_clear_format texture_clear_format_get(int format) {
    if(format>=0x6d&&format<=0x78)format-=0x48;
    if(format>=1&&format<=0x30){unsigned index=(format-1)%12;static const uint8_t kinds[]={TEXTURE_CLEAR_UNORM,TEXTURE_CLEAR_SNORM,TEXTURE_CLEAR_UINT,TEXTURE_CLEAR_SINT,TEXTURE_CLEAR_FLOAT,TEXTURE_CLEAR_UNORM,TEXTURE_CLEAR_SNORM,TEXTURE_CLEAR_UINT,TEXTURE_CLEAR_SINT,TEXTURE_CLEAR_FLOAT,TEXTURE_CLEAR_UINT,TEXTURE_CLEAR_SINT};return (texture_clear_format){(format-1)/12+1,index<4?8:index<9?16:32,kinds[index]};}
    if(format==NVN_FORMAT_RGB10A2UI)return (texture_clear_format){4,0,TEXTURE_CLEAR_UINT};
    // bits 0: packed 10/10/10/2 (BotW clears its RGB10A2 targets by region)
    if(format==NVN_FORMAT_RGB10A2)return (texture_clear_format){4,0,TEXTURE_CLEAR_UNORM};
    return (texture_clear_format){0};
}
// Round float16 bits to nearest even; never substitute a base UNORM numeric clear for a float view's bytes.
static uint16_t texture_clear_half(float value) {
    uint32_t bits;memcpy(&bits,&value,4);unsigned sign=(bits>>16)&0x8000,fraction=bits&0x7fffff,exponent=(bits>>23)&255;
    if(exponent==255)return sign|0x7c00|(fraction?0x200|(fraction>>13):0);
    int half_exponent=(int)exponent-127+15;if(half_exponent>=31)return sign|0x7c00;
    if(half_exponent<=0){if(half_exponent<-10)return sign;unsigned shift=14-half_exponent,mantissa=fraction|0x800000,result=mantissa>>shift,remainder=mantissa&((1u<<shift)-1),half=1u<<(shift-1);return sign|(result+(remainder>half||(remainder==half&&(result&1))));}
    unsigned result=(half_exponent<<10)|(fraction>>13),remainder=fraction&0x1fff;return sign|(result+(remainder>0x1000||(remainder==0x1000&&(result&1))));
}
static uint64_t texture_clear_round(double value) {
    uint64_t result=(uint64_t)value;double remainder=value-(double)result;return result+(remainder>.5||(remainder==.5&&(result&1)));
}
static unsigned texture_clear_pattern(const p_clear_tex *c,int format,uint8_t pattern[16]) {
    texture_clear_format f=texture_clear_format_get(format);if(!f.channels)die("Vulkan typed texture clear format pending: %#x",format);
    uint32_t packed=0;
    for(unsigned i=0;i<f.channels;i++){
        unsigned bits=f.bits?f.bits:i==3?2:10;uint64_t mask=(1ull<<bits)-1,raw=0;double value=c->is_uint?(double)c->c.u[i]:(double)c->c.f[i];
        if(f.kind==TEXTURE_CLEAR_FLOAT){float converted=(float)value;if(bits==16)raw=texture_clear_half(converted);else{uint32_t word;memcpy(&word,&converted,4);raw=word;}}
        else if(f.kind==TEXTURE_CLEAR_UINT){if(c->is_uint)raw=c->c.u[i]&mask;else{if(!(value>=0&&value<=4294967295.0))die("Vulkan float-to-UINT texture clear value");raw=(uint64_t)value&mask;}}
        else if(f.kind==TEXTURE_CLEAR_SINT){int64_t minimum=-(1ll<<(bits-1)),maximum=(1ll<<(bits-1))-1;if(!(value>=(double)minimum&&value<=(double)maximum))die("Vulkan SINT texture clear value outside view range");raw=(uint64_t)(int64_t)value&mask;}
        else if(f.kind==TEXTURE_CLEAR_UNORM){if(value!=value)die("Vulkan NaN UNORM texture clear");value=value<0?0:value>1?1:value;raw=texture_clear_round(value*(double)mask);}
        else {if(value!=value)die("Vulkan NaN SNORM texture clear");value=value<-1?-1:value>1?1:value;uint64_t magnitude=texture_clear_round((value<0?-value:value)*(double)(mask>>1));raw=(value<0?-(int64_t)magnitude:(int64_t)magnitude)&mask;}
        if(!f.bits)packed|=(uint32_t)raw<<(i*10);else for(unsigned byte=0;byte<bits/8;byte++)pattern[i*(bits/8)+byte]=raw>>(byte*8);
    }
    if(!f.bits){memcpy(pattern,&packed,4);return 4;}return f.channels*f.bits/8;
}
static void clear_texture(const p_clear_tex *c) {
    if(c->r.width<=0||c->r.height<=0||c->r.depth<=0||!(c->mask&15))return;
    const view_t *v=c->has_view?&c->view:NULL;int format=v&&v->set_format?v->format:c->tex->d.format;
    // Log only the first five actual initialization clears, without adding per-frame draw logging or synchronization.
    static unsigned diagnostics;if(diagnostics++<5)LOG("[vulkan] typed texture clear serial=%llu base=%#x view=%#x ui=%u raw=%08x,%08x,%08x,%08x mip=%d layer=%d+%d region=%d,%d,%d %dx%dx%d mask=%#x",(unsigned long long)c->tex->serial,c->tex->d.format,format,c->is_uint,c->c.u[0],c->c.u[1],c->c.u[2],c->c.u[3],v&&v->set_levels?v->base_level:0,v&&v->set_layers?v->min_layer:0,v&&v->set_layers?v->nlayers:c->tex->L.layers,c->r.xoffset,c->r.yoffset,c->r.zoffset,c->r.width,c->r.height,c->r.depth,c->mask);
    if(c->tex->has_layout&&c->tex->mem)dependencies_prepare_texture(c->tex);
    graphics_pass_end();image_entry *e=image_get(c->tex);VkImageSubresourceRange range=texture_range(e,v);int is3d=c->tex->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned w=dimension(c->tex->d.width,range.baseMipLevel),h=(c->tex->d.target==NVN_TEXTURE_TARGET_1D||c->tex->d.target==NVN_TEXTURE_TARGET_1D_ARRAY)?1:dimension(c->tex->d.height,range.baseMipLevel),d=is3d?dimension(c->tex->d.depth,range.baseMipLevel):c->tex->L.layers-range.baseArrayLayer;
    if(v&&v->set_levels&&(v->nlevels<=0||(uint32_t)v->nlevels>c->tex->d.levels-range.baseMipLevel))die("Vulkan texture clear mip view bounds");
    if(v&&v->set_layers){if(is3d||v->nlayers<=0||(uint32_t)v->nlayers>d)die("Vulkan texture clear layer view bounds");d=v->nlayers;}
    if(c->r.xoffset<0||c->r.yoffset<0||c->r.zoffset<0||(uint64_t)c->r.xoffset+c->r.width>w||(uint64_t)c->r.yoffset+c->r.height>h||(uint64_t)c->r.zoffset+c->r.depth>d)die("Vulkan texture clear region bounds");
    texture_clear_format f=texture_clear_format_get(format);unsigned channel_mask=f.channels?(1u<<f.channels)-1:15;
    if((c->mask&channel_mask)!=channel_mask)die("Vulkan masked texture clear pending: mask=%#x format=%#x",c->mask,format);
    int bw,bh,vbw,vbh;unsigned bytes=nvn_fmt_bpb(c->tex->d.format,&bw,&bh),view_bytes=nvn_fmt_bpb(format,&vbw,&vbh);
    if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT||!bytes||bytes!=view_bytes||bw!=1||bh!=1||vbw!=1||vbh!=1||vk_format(format)==VK_FORMAT_UNDEFINED||(format>=0x31&&format<=0x36))die("Vulkan incompatible typed texture clear base=%#x view=%#x",c->tex->d.format,format);
    // Like Metal tex_mtl, preserve packaged sources first; the region exterior, other mips and layers must retain their contents.
    if(c->tex->has_layout&&c->tex->mem)image_upload(e);
    image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    int whole_xy=!c->r.xoffset&&!c->r.yoffset&&c->r.width==(int)w&&c->r.height==(int)h;
    if(!is3d&&whole_xy&&vk_format(format)==e->format&&((c->is_uint&&f.channels&&f.kind==TEXTURE_CLEAR_UINT)||(!c->is_uint&&(!f.channels||f.kind==TEXTURE_CLEAR_UNORM||f.kind==TEXTURE_CLEAR_SNORM||f.kind==TEXTURE_CLEAR_FLOAT)))){
        range.baseArrayLayer+=c->r.zoffset;range.layerCount=c->r.depth;VkClearColorValue value;memcpy(&value,&c->c,sizeof value);vkCmdClearColorImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
    }else{
        // VkClearColorImage accepts an image, not a typed view; encode view pixels and copy raw GPU bytes in place, avoiding illegal format aliases.
        uint8_t pattern[16]={0};if(texture_clear_pattern(c,format,pattern)!=bytes)die("Vulkan typed texture clear pixel size");
        uint64_t count=(uint64_t)c->r.width*c->r.height;if(count>((1ull<<32)-3)/bytes/(unsigned)c->r.depth)die("Vulkan texture clear staging size");uint64_t size=count*c->r.depth*bytes,padded=(size+3)&~3ull;
        post_op *p=post_new(3);p->source=buffer_create(padded);
        if(bytes==1||bytes==2||bytes==4){uint8_t repeated[4];for(unsigned i=0;i<4;i++)repeated[i]=pattern[i%bytes];uint32_t word;memcpy(&word,repeated,4);vkCmdFillBuffer(command,p->source.buffer,0,padded,word);
            VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->source.buffer,.offset=0,.size=padded};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&barrier,0,NULL);
        }else for(uint64_t offset=0;offset<size;offset+=bytes)memcpy((uint8_t*)p->source.mapped+offset,pattern,bytes);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,range.baseMipLevel,range.baseArrayLayer+(is3d?0:c->r.zoffset),is3d?1:c->r.depth},.imageOffset={c->r.xoffset,c->r.yoffset,is3d?c->r.zoffset:0},.imageExtent={c->r.width,c->r.height,is3d?c->r.depth:1}};vkCmdCopyBufferToImage(command,p->source.buffer,e->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    }
    image_gpu_written(e);
}
static void copy_texture_texture(const p_copy_t2t *c) {
    PASSLOG("copy texture serial %llu (%ux%u fmt %#x) -> serial %llu (%ux%u fmt %#x) %dx%d flags %#x",(unsigned long long)c->src->serial,c->src->d.width,c->src->d.height,c->src->d.format,(unsigned long long)c->dst->serial,c->dst->d.width,c->dst->d.height,c->dst->d.format,c->dr.width,c->dr.height,c->flags);
    snapshot_prepare();
    dependencies_prepare_texture(c->src);
    if(c->dst->has_layout&&c->dst->mem)dependencies_prepare_texture(c->dst);
    graphics_pass_end();image_entry *src=image_get(c->src),*dst=image_get(c->dst);image_upload(src);
    // Metal also resolves destinations through tex_mtl; preserve untouched blocks/mips of packaged destinations from their real source.
    if(dst->texture.has_layout&&dst->texture.mem)image_upload(dst);
    image_access(src,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);image_access(dst,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange sr=texture_range(src,c->has_sv?&c->sv:NULL),dr=texture_range(dst,c->has_dv?&c->dv:NULL);int s3=c->src->d.target==NVN_TEXTURE_TARGET_3D,d3=c->dst->d.target==NVN_TEXTURE_TARGET_3D;
    VkImageAspectFlags aspects=src->aspect & dst->aspect;if(!aspects)die("Vulkan incompatible copy aspects");
    for(unsigned aspect=1;aspect<=VK_IMAGE_ASPECT_STENCIL_BIT;aspect<<=1)if(aspects & aspect){
        VkImageSubresourceLayers sl={aspect,sr.baseMipLevel,sr.baseArrayLayer+(s3?0:c->sr.zoffset),s3?1:c->sr.depth},dl={aspect,dr.baseMipLevel,dr.baseArrayLayer+(d3?0:c->dr.zoffset),d3?1:c->dr.depth};
        if(src->format==dst->format&&c->sr.width==c->dr.width&&c->sr.height==c->dr.height&&c->sr.depth==c->dr.depth&&!(c->flags & (1|4|8|16))){VkImageCopy region={.srcSubresource=sl,.srcOffset={c->sr.xoffset,c->sr.yoffset,s3?c->sr.zoffset:0},.dstSubresource=dl,.dstOffset={c->dr.xoffset,c->dr.yoffset,d3?c->dr.zoffset:0},.extent={c->sr.width,c->sr.height,s3||d3?c->sr.depth:1}};vkCmdCopyImage(command,src->image,VK_IMAGE_LAYOUT_GENERAL,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);}
        else {VkImageBlit region={.srcSubresource=sl,.srcOffsets={{c->sr.xoffset,c->sr.yoffset,s3?c->sr.zoffset:0},{c->sr.xoffset+c->sr.width,c->sr.yoffset+c->sr.height,s3?c->sr.zoffset+c->sr.depth:1}},.dstSubresource=dl,.dstOffsets={{c->dr.xoffset,c->dr.yoffset,d3?c->dr.zoffset:0},{c->dr.xoffset+c->dr.width,c->dr.yoffset+c->dr.height,d3?c->dr.zoffset+c->dr.depth:1}}};for(int axis=0;axis<3;axis++)if(c->flags & (4<<axis)){int32_t *a=(int32_t*)&region.srcOffsets[0],*b=(int32_t*)&region.srcOffsets[1];int32_t t=a[axis];a[axis]=b[axis];b[axis]=t;}vkCmdBlitImage(command,src->image,VK_IMAGE_LAYOUT_GENERAL,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,&region,(c->flags&1)&&aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST);}
    }
    image_gpu_written(dst);
}
/* The texture descriptor table is reserved once at its largest size (address space only; untouched pages cost
   nothing) so it never moves, and every entry has a sequence number (odd while being written): descriptor_texture()
   reads an entry without the descriptor lock, retrying a copy that raced a write. It runs for every texture binding
   of every draw (~60,000 lock pairs a frame in the open world if it locked). */
static _Atomic uint32_t texture_seq[1u << 20];
void mtl_set_tex_pool_n(int n) {
    if(n<0||n>1048576)die("Vulkan texture descriptor capacity %d",n);pthread_mutex_lock(&descriptor_lock);
    if(!texture_descriptors){void *m=mmap(NULL,(size_t)1048576*sizeof *texture_descriptors,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if(m==MAP_FAILED)die("Vulkan descriptor reservation");texture_descriptors=m;}
    if(n>texture_capacity)texture_capacity=n;pthread_mutex_unlock(&descriptor_lock);
}
/* TOTK: changes whenever a texture id is (re)registered */
static _Atomic uint64_t descriptor_generation;
/* TOTK: per texture id what a shader variant depends on (nvn_vulkan_totk_shader.inc totk_state_sig): 1 | target << 1 |
   format << 8 of the view (else the texture), 0 for none. A fixed array (4 MB of zero pages until used): read without
   the descriptor lock on every draw. */
static _Atomic uint32_t texture_kinds[1u << 20];
void mtl_reg_tex(int id,const tex_t *t,const view_t *v) {pthread_mutex_lock(&descriptor_lock);atomic_fetch_add(&descriptor_generation,1);if(id<0||id>=texture_capacity)die("Vulkan texture descriptor index %d",id);
    atomic_store_explicit(&texture_kinds[id],1u|(uint32_t)((v&&v->set_target?v->target:t->d.target)&127)<<1|(uint32_t)(v&&v->set_format?v->format:t->d.format)<<8,memory_order_relaxed);uint32_t seq=atomic_load_explicit(&texture_seq[id],memory_order_relaxed);atomic_store_explicit(&texture_seq[id],seq+1,memory_order_relaxed);atomic_thread_fence(memory_order_release);
    texture_descriptors[id]=(texture_descriptor){.valid=1,.texture=*t};if(v)texture_descriptors[id].view=*v;atomic_store_explicit(&texture_seq[id],seq+2,memory_order_release);
    if(v){static unsigned view_diagnostics;if(v->set_format&&v->format!=t->d.format&&view_diagnostics++<12)LOG("[vulkan] texture view format texture=%#x view=%#x serial=%llu",t->d.format,v->format,(unsigned long long)t->serial);}pthread_mutex_unlock(&descriptor_lock);}
void mtl_reg_samp(int id,const samp_par *p) {
    if(id<0||id>1048576)die("Vulkan sampler descriptor index %d",id);pthread_mutex_lock(&descriptor_lock);
    if(id>=sampler_capacity){int n=(id+64)&~63;sampler_descriptor *next=realloc(sampler_descriptors,n*sizeof *next);if(!next)die("Vulkan sampler allocation");memset(next+sampler_capacity,0,(n-sampler_capacity)*sizeof *next);sampler_descriptors=next;sampler_capacity=n;}
    sampler_descriptors[id].valid=1;sampler_descriptors[id].parameters=*p;sampler_descriptors[id].sampler=VK_NULL_HANDLE;if(id<4096)atomic_store_explicit(&sampler_fast[id],0,memory_order_release);pthread_mutex_unlock(&descriptor_lock);
}
/* Released textures are unlinked at once (a later texture with the same serial gets a new image) and torn down once
   the GPU has finished the last submission made before the release: framebuffers and views destroyed, image and
   memory into the free list. A full GPU wait per release would be ~26 per frame on the title screen. */
typedef struct retired_image { image_entry *e; uint64_t fence; struct retired_image *next; } retired_image;
static retired_image *retired_images;
/* advances when any image entry is destroyed: cached entry pointers (bind_shader's texture memo) are stale */
static uint64_t image_generation;
static void image_destroy_locked(image_entry *e) {
    image_generation++;
    graphics_cache_forget_image(e);
    while(e->views){cached_image_view *v=e->views;e->views=v->next;bindless_view_forget(v);for(int i=0;i<2;i++)if(v->clear_fb[i])vkDestroyFramebuffer(device,v->clear_fb[i],NULL);vkDestroyImageView(device,v->view,NULL);free(v);}
    vkDestroyImageView(device,e->view,NULL);
    VkMemoryRequirements req;vkGetImageMemoryRequirements(device,e->image,&req);free_image *f=free_image_bytes+req.size<=FREE_IMAGE_BUDGET?malloc(sizeof *f):NULL;
    if(f){*f=(free_image){e->key,e->image,e->memory,req.size,free_images};free_images=f;free_image_bytes+=req.size;}
    else{vkDestroyImage(device,e->image,NULL);image_memory_give(&e->memory);}
    free(e);
}
static void image_reap_locked(void) {
    for(retired_image **link=&retired_images;*link;){retired_image *r=*link;
        if(mtl_wait(r->fence,0)){*link=r->next;image_destroy_locked(r->e);free(r);}else link=&r->next;}
}
static void frz_forget_image(const image_entry *e);
/* Unlink an image entry (from the list, the serial index and the storage index) and retire it. */
static void image_retire_locked(image_entry *e) {
    TRACEIMG(&e->texture,"retired");
    frz_forget_image(e);
    image_entry **link=&image_list;while(*link&&*link!=e)link=&(*link)->next;if(!*link)die("Vulkan image list corruption");*link=e->next;
    image_entry **hash=&image_index[e->serial&4095];while(*hash&&*hash!=e)hash=&(*hash)->hash_next;if(!*hash)die("Vulkan image index corruption");*hash=e->hash_next;
    if(e->storage_keyed){image_entry **st=&storage_index[storage_bucket(&e->storage)];while(*st&&*st!=e)st=&(*st)->storage_next;if(*st)*st=e->storage_next;}
    if(e->block_aux){image_entry *aux=e->block_aux;e->block_aux=NULL;image_retire_locked(aux);}
    retired_image *r=malloc(sizeof *r);if(!r)die("Vulkan retired image allocation");*r=(retired_image){e,mtl_last_fence(),retired_images};retired_images=r;
}
static void image_release_locked(uint64_t serial) {
    image_entry *e=NULL;
    for(image_alias **a=&image_aliases[serial&4095];*a;a=&(*a)->next)if((*a)->serial==serial){image_alias *x=*a;e=x->e;*a=x->next;free(x);break;}
    if(!e)for(image_entry *p=image_index[serial&4095];p;p=p->hash_next)if(p->serial==serial){e=p;break;}
    if(!e||!e->holders)return;
    TRACEIMG(&e->texture,"release serial %llu (holders %u)",(unsigned long long)serial,e->holders);
    if(--e->holders)return;
    /* kept for a re-initialised object at the same storage */
    if(e->storage_keyed){e->idle_since=presents+1;return;}
    image_retire_locked(e);
}
/* Storage-keyed images nobody has held for STORAGE_IDLE_FRAMES frames. */
static void image_idle_sweep_locked(void) {
    static uint64_t swept;if(presents==swept)return;swept=presents;
    for(image_entry *e=image_list,*next;e;e=next){next=e->next;if(!e->holders&&e->idle_since&&presents>e->idle_since+STORAGE_IDLE_FRAMES)image_retire_locked(e);}
}
void mtl_tex_release(tex_t *t) {
    if(!device)return;pthread_mutex_lock(&queue_lock);image_release_locked(t->serial);image_idle_sweep_locked();image_reap_locked();pthread_mutex_unlock(&queue_lock);
}
void mtl_read_texels(const tex_t *t,unsigned level,unsigned layer,const NVNcopyRegion *r,uint8_t *dst,size_t row,size_t img) {
    if(r->width<=0||r->height<=0||r->depth<=0)return;mtl_sub_begin();dependencies_prepare_texture(t);image_entry *e=image_get(t);image_upload(e);
    if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)die("Vulkan depth readback pending");int bw,bh,bpb=nvn_fmt_bpb(t->d.format,&bw,&bh);
    uint32_t pitch=(r->width+bw-1)/bw*bpb,rows=(r->height+bh-1)/bh;uint64_t bytes=(uint64_t)pitch*rows*r->depth;vk_buffer readback=buffer_create(bytes);image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy={.imageSubresource={e->aspect,level,layer,1},.imageOffset={r->xoffset,r->yoffset,r->zoffset},.imageExtent={r->width,r->height,r->depth}};vkCmdCopyImageToBuffer(command,e->image,VK_IMAGE_LAYOUT_GENERAL,readback.buffer,1,&copy);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);uint64_t ticket=mtl_sub_commit();mtl_wait(ticket,UINT64_MAX);
    if(!row)row=pitch;if(!img)img=row*rows;dm_guest_write_prepare(dst,img*r->depth);
    for(unsigned z=0;z<(unsigned)r->depth;z++)for(unsigned y=0;y<rows;y++)memcpy(dst+z*img+y*row,(uint8_t*)readback.mapped+(z*rows+y)*pitch,pitch);
    buffer_destroy(&readback);
}
/* nvnTextureWriteTexels (TOTK's font glyph cache uses it): the region goes straight into the image, and into the guest
   bytes too so later uploads and texture objects created over the same storage see it. The image is brought up to date
   with the guest first, and the guest bytes are re-baselined after, so the write never triggers a re-upload that would
   overwrite GPU-rendered contents. */
/* The textures whose pool bytes are their contents (nvn_mtl.m tex_mirrored): a packaged layout, or TOTK's default layout in a
   CPU-visible pool. WriteTexels has to reach those bytes: the Depths terrain writes its tile maps once, then makes new textures
   over them, and those read the pool. */
static int tex_mirrored(const tex_t *t) {
    if(!t||!t->mem)return 0;
    if(t->has_layout)return 1;
    const pool_t *p=t->pool;
    if(!p||p->host||!(p->flags&(NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_CPU_CACHED)))return 0;
    if(t->d.flags&(NVN_TEXTURE_FLAGS_DISPLAY|NVN_TEXTURE_FLAGS_COMPRESSIBLE|NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL|NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR|NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))return 0;
    if(t->d.samples>1||t->d.target==10)return 0;
    return !(t->d.format>=0x31&&t->d.format<=0x36);
}
void mtl_write_texels(const tex_t *t,unsigned level,unsigned layer0,const NVNcopyRegion *r,const uint8_t *src,size_t row,size_t img) {
    int nz=r->depth?r->depth:1,is3d=t->d.target==NVN_TEXTURE_TARGET_3D;
    if(r->width<=0||r->height<=0||nz<=0)return;
    int bw,bh;unsigned bpb=nvn_fmt_bpb(t->d.format,&bw,&bh);if(!bpb)die("WriteTexels: unknown format %#x",t->d.format);
    uint32_t lw=dimension(t->d.width,level),lh=dimension(t->d.height,level),lz=is3d?dimension(t->d.depth,level):1;
    if(r->xoffset<0||r->yoffset<0||r->zoffset<0||r->xoffset%bw||r->yoffset%bh||(uint32_t)(r->xoffset+r->width)>lw||(uint32_t)(r->yoffset+r->height)>lh||(is3d&&(uint32_t)(r->zoffset+nz)>lz))
        die("WriteTexels: region %d,%d,%d %dx%dx%d outside level %u (%ux%ux%u, format %#x)",r->xoffset,r->yoffset,r->zoffset,r->width,r->height,nz,level,lw,lh,lz,t->d.format);
    size_t wb=(size_t)((r->width+bw-1)/bw)*bpb,hb=(r->height+bh-1)/bh;
    int guest=tex_mirrored(t);
    mtl_sub_begin();if(guest)dependencies_prepare_texture(t);
    image_entry *e=image_get(t);if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)die("WriteTexels: depth/stencil texture");
    if(guest)image_upload(e);
    image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    post_op *p=post_new(3);p->source=buffer_create(wb*hb*nz);
    for(int z=0;z<nz;z++)for(size_t y=0;y<hb;y++)memcpy((uint8_t*)p->source.mapped+(z*hb+y)*wb,src+z*img+y*row,wb);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,is3d?0:layer0+r->zoffset,is3d?1:(uint32_t)nz},.imageOffset={r->xoffset,r->yoffset,is3d?r->zoffset:0},
        .imageExtent={(uint32_t)r->width,(uint32_t)r->height,is3d?(uint32_t)nz:1}};
    vkCmdCopyBufferToImage(command,p->source.buffer,e->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    e->populated=1;
    if(guest){
        size_t lp=(size_t)((lw+bw-1)/bw)*bpb,lr=(lh+bh-1)/bh;uint8_t *lin=malloc(lp*lr*lz);if(!lin)die("WriteTexels: out of memory");
        dm_guest_write_prepare(t->mem,t->L.size);int ok=1;
        for(int z=0;z<nz&&ok;z++){
            unsigned layer=is3d?0:layer0+r->zoffset+z;
            if((!is3d||z==0)&&nvn_deswizzle(&t->d,level,layer,t->mem,lin,lp))ok=0;
            uint8_t *dst=lin+(is3d?(size_t)(r->zoffset+z)*lp*lr:0);
            if(ok)for(size_t y=0;y<hb;y++)memcpy(dst+(r->yoffset/bh+y)*lp+(size_t)(r->xoffset/bw)*bpb,src+z*img+y*row,wb);
            if(ok&&(!is3d||z==nz-1)&&nvn_swizzle(&t->d,level,layer,lin,lp,t->mem))ok=0;
        }
        free(lin);
        if(!ok){static int said;if(!said++)LOG("[vulkan] WriteTexels: texture layout %#x not swizzlable; guest bytes not updated",t->d.flags);}
        /* our own write is not a change to upload */
        nvn_dirty_protect(t->mem,t->L.size);uint64_t now=nvn_dirty_now();image_source_baseline(e,t->mem,now);if(!t->has_layout)e->stamp=now;
    }
    mtl_sub_commit();
}

/* Pictures of a pass's colour targets (passes at least 256 wide): blitted to a quarter-size RGBA16F image, written as
   <files>/passlog/NNN-<serial>.ppm after the present (float targets brightened with x/(1+x), then sqrt). */
typedef struct { VkImage image; VkDeviceMemory memory; vk_buffer buffer; uint32_t w, h; uint64_t serial, frame; int format; unsigned pass, index, last; } passlog_picture;
static passlog_picture passlog_pictures[200]; static unsigned passlog_npictures;
static void passlog_picture_take(image_entry *e,unsigned index) {
    if(e->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)return;
    { static uint64_t pictured_frame; if(pictured_frame!=presents){pictured_frame=presents;passlog_pass=0;} }
    /* consecutive passes on the same target: one picture of its latest state */
    for(unsigned i=0;i<passlog_npictures;i++){passlog_picture *q=&passlog_pictures[i];
        if(q->serial==e->serial&&q->frame==presents&&q->last+1>=passlog_pass){q->last=passlog_pass;image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)e->texture.d.width,(int32_t)e->texture.d.height,1}},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)q->w,(int32_t)q->h,1}}};
            VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
            vkCmdBlitImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,q->image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
            VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={q->w,q->h,1}};vkCmdCopyImageToBuffer(command,q->image,VK_IMAGE_LAYOUT_GENERAL,q->buffer.buffer,1,&c);
            return;}}
    if(passlog_npictures>=200)return;
    VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(physical,e->format,&fp);
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return;
    texture_clear_format f=texture_clear_format_get(e->texture.d.format);if(f.channels&&(f.kind==TEXTURE_CLEAR_UINT||f.kind==TEXTURE_CLEAR_SINT))return;
    passlog_picture *p=&passlog_pictures[passlog_npictures];uint32_t w=dimension(e->texture.d.width,0)/4,h=dimension(e->texture.d.height,0)/4;if(!w||!h)return;
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R16G16B16A16_SFLOAT,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VK_CHECK(vkCreateImage(device,&ci,NULL,&p->image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,p->image,&req);
    uint32_t type=0;for(uint32_t i=0;i<memory_properties.memoryTypeCount;i++)if(req.memoryTypeBits&(1u<<i)){type=i;break;}
    VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};VK_CHECK(vkAllocateMemory(device,&mi,NULL,&p->memory));VK_CHECK(vkBindImageMemory(device,p->image,p->memory,0));
    p->buffer=buffer_create((uint64_t)w*h*8);p->w=w;p->h=h;p->serial=e->serial;p->format=e->texture.d.format;p->pass=passlog_pass;p->last=passlog_pass;p->index=index;p->frame=presents;passlog_npictures++;
    image_access(e,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=p->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)e->texture.d.width,(int32_t)e->texture.d.height,1}},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)w,(int32_t)h,1}}};
    vkCmdBlitImage(command,e->image,VK_IMAGE_LAYOUT_GENERAL,p->image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={w,h,1}};vkCmdCopyImageToBuffer(command,p->image,VK_IMAGE_LAYOUT_GENERAL,p->buffer.buffer,1,&c);
}
static float passlog_half(uint16_t h){uint32_t s=(h>>15)&1,e=(h>>10)&31,m=h&1023;float v=e==0?m/16777216.f:e==31?65504.f:(1.f+m/1024.f)*ldexpf(1.f,(int)e-15);return s?-v:v;}
static void passlog_pictures_write(void) {
    const char *dir=getenv("DM_VK_SHADERS");char path[1100];if(!dir)return;snprintf(path,sizeof path,"%s/../passlog",dir);mkdir(path,0700);
    for(unsigned i=0;i<passlog_npictures;i++){passlog_picture *p=&passlog_pictures[i];
        texture_clear_format f=texture_clear_format_get(p->format);int hdr=f.kind==TEXTURE_CLEAR_FLOAT||p->format==0x3f||p->format==0x40;
        snprintf(path,sizeof path,"%s/../passlog/f%llu-%03u-%u-%llu-%#x.ppm",dir,(unsigned long long)p->frame,p->pass,p->index,(unsigned long long)p->serial,p->format);FILE *o=fopen(path,"wb");
        if(o){fprintf(o,"P6\n%u %u\n255\n",p->w,p->h);const uint16_t *px=p->buffer.mapped;
            for(uint32_t k=0;k<p->w*p->h;k++)for(int c=0;c<3;c++){float v=passlog_half(px[k*4+c]);if(v<0)v=0;if(hdr){v=v/(1+v);v=sqrtf(v);}if(v>1)v=1;fputc((int)(v*255+.5f),o);}fclose(o);}
        vkDestroyImage(device,p->image,NULL);vkFreeMemory(device,p->memory,NULL);buffer_destroy(&p->buffer);}
    if(passlog_npictures)LOG("[vulkan-passlog] wrote %u pictures to %s/../passlog",passlog_npictures,dir);
    passlog_npictures=0;
}
