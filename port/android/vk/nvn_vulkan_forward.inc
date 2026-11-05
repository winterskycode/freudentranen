// GPU forwarding across FIFO/current batches: borrow recorded, fully initialized versions while preserving guest publication and backing lifetime.
typedef enum forward_result {
    FORWARD_NONE,FORWARD_FULL,FORWARD_PARTIAL,FORWARD_UNRECORDED,FORWARD_ALIGNMENT,FORWARD_BOUNDS,FORWARD_HOST_EFFECT
} forward_result;
typedef struct forward_slice {
    VkBuffer buffer;
    VkDeviceSize offset,size;
    vk_buffer backing;
    post_op *producer;
    int host_written;
} forward_slice;
typedef struct forward_hold_entry { buffer_owner *owner; struct forward_hold_entry *next; } forward_hold_entry;
typedef struct forward_hold_chunk { forward_hold_entry entries[64]; unsigned used; struct forward_hold_chunk *next; } forward_hold_chunk;
static forward_hold_entry *forward_holds[1024];
static forward_hold_chunk *forward_hold_chunks,*forward_hold_current;
static uint64_t forward_hits,forward_partial_misses,forward_unrecorded_misses,forward_alignment_misses,forward_barriers,forward_references;
static void forward_reset(void) {
    // Reset only current-batch retain deduplication; producer readiness lives in its post across slots until publication completes.
    memset(forward_holds,0,sizeof forward_holds);for(forward_hold_chunk *c=forward_hold_chunks;c;c=c->next)c->used=0;forward_hold_current=forward_hold_chunks;
}
static void forward_mark_gpu_recorded(post_op *post,VkPipelineStageFlags stage,VkAccessFlags access,int full_version) {
    // full_version means all size bytes were seeded from the previous version, preserving bytes outside unknown partial shader stores.
    if(!full_version)return;
    if(!post||post->kind!=0||post->source.buffer||!post->destination||!post->size||!post->output.buffer||post->size>post->output.size||!stage||!(access&(VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT)))die("Vulkan forward producer contract");
    if(post->gpu_stage)return;post->gpu_stage=stage;post->gpu_access=access;memset(post->gpu_synchronized,0,sizeof post->gpu_synchronized);
}
static int forward_overlap(uintptr_t address,uint64_t size,uintptr_t start,uint64_t bytes) {
    if(!size||!bytes)return 0;if(size>UINTPTR_MAX-address||bytes>UINTPTR_MAX-start)die("Vulkan forward range overflow");return address<start+bytes&&start<address+size;
}
static post_op *forward_latest_chain(post_op *copies,uintptr_t address,uint64_t size,const post_op *stop_exclusive,post_op *latest) {
    for(post_op *p=copies;p&&p!=stop_exclusive;p=p->copy_next)if(p->destination&&forward_overlap(address,size,(uintptr_t)p->destination,p->size))latest=p;return latest;
}
/* The batch's latest write over [address, address+size) from its page index (forward_index_add): the same answer as
   the chain walk, which visits every write of every batch in flight for each forwarded read (~4% of the replay
   thread). Orders rise along a batch's chain, so stopping at stop_exclusive is taking orders below its own; a stop
   outside this batch is never met by the walk either. Large reads (over 256 pages) and unindexed batches walk.
   TOTK_VK_EXP=fwdchain: always walk; fwdcheck: both, fatal on a difference. */
static post_op *forward_latest_batch(const async_batch *batch,post_op *copies,uintptr_t address,uint64_t size,const post_op *stop_exclusive,post_op *latest) {
    static int mode=-1;if(mode<0)mode=exp_env("fwdchain")?1:exp_env("fwdcheck")?2:0;
    if(mode==1||!batch||batch->wi_unindexed||!size||size>UINTPTR_MAX-address||((address+size-1)>>12)-(address>>12)>=256)return forward_latest_chain(copies,address,size,stop_exclusive,latest);
    uint64_t before=stop_exclusive&&stop_exclusive->batch_serial==batch->serial?stop_exclusive->order:UINT64_MAX;
    const post_op *found=NULL;
    for(uintptr_t page=address>>12;page<=(address+size-1)>>12;page++){
        uint32_t h=(uint32_t)(page*0x9e3779b1u)>>22;
        for(uint32_t i=batch->wi_head[h];i;i=batch->wi_nodes[i-1].next){const struct kc_node *k=&batch->wi_nodes[i-1];if(k->page!=page)continue;
            const post_op *p=k->post;if(p->order<before&&(!found||p->order>found->order)&&forward_overlap(address,size,(uintptr_t)p->destination,p->size))found=p;}
    }
    for(uint32_t i=0;i<batch->wi_big_count;i++){const post_op *p=batch->wi_big[i];
        if(p->order<before&&(!found||p->order>found->order)&&forward_overlap(address,size,(uintptr_t)p->destination,p->size))found=p;}
    post_op *result=found?(post_op*)found:latest;
    if(mode==2&&result!=forward_latest_chain(copies,address,size,stop_exclusive,latest))
        die("Vulkan forward write index mismatch: range=%#llx+%llu batch=%llu",(unsigned long long)address,(unsigned long long)size,(unsigned long long)batch->serial);
    return result;
}
static int forward_host_effects(const async_batch *batch,uintptr_t address,uint64_t size,const post_op *stop_exclusive) {
    if(!batch)return 0;
    // Preserve the dynamic first-future-output boundary at *copy_tail; capturing an order at marker creation would miss intervening events.
    int result=pending_range_overlap(batch,DM_PENDING_HOST_EFFECT,address,size,stop_exclusive?stop_exclusive->order:DM_PENDING_ORDER_ALL);
    return result;
}
static uint64_t draw_count;
/* TOTK_VK_FWDWATCH=<hex address>: forwarding events of the versions holding it, for three frames from frame
   TOTK_VK_FWDWATCH_FRAME (default 2000): seeds, reads, read barriers, in-place writes. */
/* Experiments that take effect without a restart, so a frozen frame (frz.sh) replays with and without them:
   <files>/exp.live holds words, read every frame (fg_dump_poll; TOTK_VK_LIVE when there is no file; frz.sh live).
   "@A-B" limits a word to the frame's graphics draws A..B, counted from 0 at the previous present, for bisecting.
     restart[@A-B]        a pass end before every draw
     barrier[@A-B]        a full memory barrier after every pass
     inpassbarrier[@A-B]  the game's barriers inside passes kept (a pass end and a full barrier)
     noforward, nofwd=<hex>[,<hex>...]   no GPU-to-GPU forwarding: everything / reads overlapping those addresses
     fwdwatch=<hex>       the forwarding events of that address, logged ([fwdwatch])
     passlog@A-B          the passes ending in that range, logged once with their targets and the caller that ended them
     qpass                a samples report or counter reset inside a pass ends the pass (graphics_query_split)
     keyend[@A-B]         the pass ends right before the draw's pass decision (draw_pass_decide) */
static char live_exp[128];static uint64_t live_frame_base,live_log_frame;
/* hot (forward_find): empty list first */
static int live_exp_has(const char *word) { return live_exp[0] && exp_word(live_exp, word, 0) != NULL; }
static int live_range(const char *word, uint64_t d) {
    const char *w = live_exp[0] ? exp_word(live_exp, word, 0) : NULL; unsigned long long a, b;
    if (!w) return 0;
    if (*w != '@') return 1;
    return sscanf(w + 1, "%llu-%llu", &a, &b) == 2 && d >= a && d <= b;
}
static int fwd_watch(uintptr_t address,uint64_t size) {
    {const char *v=live_exp[0]?exp_word(live_exp,"fwdwatch=",0):NULL;if(v){uint64_t x=strtoull(v,NULL,16);if(x>=address&&x-address<size)return 1;}}
    static uint64_t w=1,first;if(w==1){const char *e=getenv("TOTK_VK_FWDWATCH"),*f=getenv("TOTK_VK_FWDWATCH_FRAME");w=e?strtoull(e,NULL,16):0;first=f?strtoull(f,NULL,10):2000;}
    return w&&w>=address&&w-address<size&&presents>=first&&presents<first+3;
}
static forward_result forward_find(uintptr_t address,uint64_t size,uint64_t alignment,post_op *const *scope_marker,forward_slice *out) {
    if(!out)return FORWARD_BOUNDS;*out=(forward_slice){0};if(!address||!size||size>UINTPTR_MAX-address)return FORWARD_BOUNDS;
    /* experiment TOTK_VK_EXP=noforward: no GPU-to-GPU forwarding; readers of GPU-written memory wait for completion and
       read the published bytes (dependency flushes) */
    { static int off=-1;if(off<0)off=exp_env("noforward");if(off||live_exp_has("noforward"))return FORWARD_NONE; }
    {const char *x=live_exp[0]?exp_word(live_exp,"nofwd=",0):NULL;if(x)
        while(*x&&*x!=' '){char *e;uint64_t v=strtoull(x,&e,16);if(e==x)break;if(v>=address&&v<address+size)return FORWARD_NONE;x=*e==','?e+1:e;}}
    // Capture post_copy_tail's address before binding; *marker starts this NVN command's new outputs and may still be null.
    if(!write_chunk_maybe(address,size))return FORWARD_NONE;
    const post_op *stop_exclusive=scope_marker?*scope_marker:NULL;post_op *latest=NULL;
    for(async_batch *batch=async_first;batch;batch=batch->next){
        if(batch->host_sync&&forward_host_effects(batch,address,size,NULL))return FORWARD_HOST_EFFECT;
        latest=forward_latest_batch(batch,batch->copies,address,size,NULL,latest);
    }
    if(pending_host_sync&&forward_host_effects(async_current,address,size,stop_exclusive))return FORWARD_HOST_EFFECT;
    latest=forward_latest_batch(async_current,post_copy_first,address,size,stop_exclusive,latest);if(!latest)return FORWARD_NONE;
    uintptr_t start=(uintptr_t)latest->destination;
    // Never fall back to an older full version when the latest write covers only part of the requested range.
    if(address<start||size>latest->size||address-start>latest->size-size){forward_partial_misses++;return FORWARD_PARTIAL;}
    const vk_buffer *buffer;
    if(latest->source.buffer)buffer=&latest->source;
    else{if(!latest->gpu_stage){forward_unrecorded_misses++;return FORWARD_UNRECORDED;}buffer=&latest->output;}
    uint64_t offset=address-start;if(!buffer->buffer||!buffer->owner||offset>buffer->size||size>buffer->size-offset)return FORWARD_BOUNDS;
    if(alignment&&offset%alignment){forward_alignment_misses++;return FORWARD_ALIGNMENT;}
    *out=(forward_slice){.buffer=buffer->buffer,.offset=offset,.size=size,.backing=*buffer,.producer=latest,.host_written=!!latest->source.buffer};forward_hits++;
    if(stateleak_state>0){if(latest->census_frame!=sl.submission)sl.fwd_older++;else if(latest->census_piece!=sl.piece)sl.fwd_cross++;else sl.fwd_same++;}
    return FORWARD_FULL;
}
static void forward_read_barrier(const forward_slice *slice,VkPipelineStageFlags stage,VkAccessFlags access) {
    VkAccessFlags writes=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if(!slice||!slice->producer||!stage||!access||(access&writes))die("Vulkan forward read-only contract");
    // Coherent host input is written before submission, whose host-write ordering needs no additional pass break.
    if(slice->host_written)return;post_op *producer=slice->producer;if(!producer->gpu_stage)die("Vulkan forward recorded version missing");
    /* this read, for a later in-place write of the version to wait on (write after read) */
    if(producer->read_draw!=draw_count){producer->read_stages|=producer->read_stages_draw;producer->read_stages_draw=0;producer->read_draw=draw_count;}
    producer->read_stages_draw|=stage;
    int needed=0;for(unsigned bit=0;bit<32;bit++)if((stage&(1u<<bit))&&(access&~producer->gpu_synchronized[bit]))needed=1;
    if(fwd_watch((uintptr_t)producer->destination,producer->size))
        LOG("[fwdwatch] frame %llu draw %llu read barrier stage %#x producer order %llu batch %llu gpu_stage %#x: %s",(unsigned long long)presents,(unsigned long long)draw_count,stage,(unsigned long long)producer->order,(unsigned long long)producer->batch_serial,producer->gpu_stage,needed?"barrier":"already synchronized");
    if(!needed)return;
    // Synchronize the whole producer range before memoizing; a small-slice barrier cannot cover later slices.
    graphics_pass_end();VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=producer->gpu_access,.dstAccessMask=access,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=slice->buffer,.offset=0,.size=producer->size};
    vkCmdPipelineBarrier(command,producer->gpu_stage,stage,0,0,NULL,1,&barrier,0,NULL);forward_barriers++;
    for(unsigned bit=0;bit<32;bit++)if(stage&(1u<<bit))producer->gpu_synchronized[bit]|=access;
}
static void forward_hold(const forward_slice *slice) {
    if(!slice||!slice->backing.buffer||!slice->backing.owner)die("Vulkan forward backing missing");
    uint64_t key=(uintptr_t)slice->backing.owner>>4;key^=key>>33;key*=0xff51afd7ed558ccdull;key^=key>>33;unsigned bucket=key&1023;
    for(forward_hold_entry *entry=forward_holds[bucket];entry;entry=entry->next)if(entry->owner==slice->backing.owner)return;
    if(!forward_hold_current){forward_hold_chunks=calloc(1,sizeof *forward_hold_chunks);if(!forward_hold_chunks)die("Vulkan forward hold allocation");forward_hold_current=forward_hold_chunks;}
    while(forward_hold_current->used==64){if(!forward_hold_current->next){forward_hold_current->next=calloc(1,sizeof *forward_hold_current);if(!forward_hold_current->next)die("Vulkan forward hold allocation");}forward_hold_current=forward_hold_current->next;}
    forward_hold_entry *entry=&forward_hold_current->entries[forward_hold_current->used++];entry->owner=slice->backing.owner;entry->next=forward_holds[bucket];forward_holds[bucket]=entry;
    // post3 owns an independent backing reference; the producer post can publish/retire while the buffer stays alive until the consumer fence.
    post_new(3)->source=buffer_borrow(slice->backing);forward_references++;
}
