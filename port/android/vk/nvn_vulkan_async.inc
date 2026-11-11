// Independent commands/queries/fences per batch; publish guest memory in FIFO order after GPU completion, then release resources.
#ifndef DM_VK_BATCH_SLOTS
#define DM_VK_BATCH_SLOTS 8
#endif

enum {ASYNC_FREE,ASYNC_RECORDING,ASYNC_PENDING};
// Submit at natural pass boundaries. TOTK frames are many small passes and the GPU should start early (the frame is
// latency-bound: translation, GPU and the game's fence wait in series): 16 draws and 1 ms per chunk, up to 16 chunks.
enum {ASYNC_CHUNK_CAPACITY=64,ASYNC_CHUNK_MIN_DRAWS=16};
/* Record budget of a mid-batch chunk while earlier work is in flight: 250 us. A chunk the GPU finishes before the
   next is cut leaves it waiting: with 1 ms it is idle 25-58% of the first ~7 ms of the replay. Still scene: 1000 us
   17.5 ms, 500 us 17.2/17.7, 250 us 17.2/17.1, 125 us 17.1/17.1 (~14 early chunks a frame instead of ~3). What idle
   remains at 3-5 ms into the replay is translation throughput, not the hand-off. With translation 2.5x faster than
   the GPU (11.1 ms frames) the budget makes no difference. */
#define ASYNC_CHUNK_RECORD_NS 250000ull
#define ASYNC_CHUNK_IDLE_RECORD_NS 150000ull
/* TOTK_VK_CHUNK_US=<record budget us>[,<idle budget us>] overrides the two for an A/B (defaults 250,150). */
static uint64_t chunk_budget_ns[2];
static uint64_t chunk_budget(int in_flight) {
    if(!chunk_budget_ns[0]){chunk_budget_ns[0]=ASYNC_CHUNK_RECORD_NS;chunk_budget_ns[1]=ASYNC_CHUNK_IDLE_RECORD_NS;const char *e=getenv("TOTK_VK_CHUNK_US");
        if(e&&*e){char *p;unsigned long long a=strtoull(e,&p,10);if(a)chunk_budget_ns[0]=a*1000;if(*p==','){unsigned long long b=strtoull(p+1,NULL,10);if(b)chunk_budget_ns[1]=b*1000;}}}
    return chunk_budget_ns[in_flight?0:1];
}
enum {ASYNC_QUERY_CAPACITY=1024,ASYNC_TIMESTAMP_BYTES=2*sizeof(uint64_t),
      ASYNC_QUERY_READBACK_BYTES=ASYNC_TIMESTAMP_BYTES+ASYNC_QUERY_CAPACITY*sizeof(uint64_t)};
typedef struct async_batch {
    VkCommandBuffer command;
    VkCommandBuffer chunks[ASYNC_CHUNK_CAPACITY];
    /* per chunk, lazily allocated: work that goes ahead of the chunk (prelude_begin) */
    VkCommandBuffer preludes[ASYNC_CHUNK_CAPACITY];
    unsigned chunk_count,chunk_index,chunk_draws;
    uint64_t chunk_started;
    VkFence fence;
    VkQueryPool queries,times,counter_queries;
    /* counter_written: queries the slot's previous batch wrote (reset at the next begin) */
    uint32_t counter_used,counter_written;
    vk_buffer query_readback;
    post_op *posts,*copies;
    dm_pending_ranges pending_ranges;
    uint64_t ticket,temporary_base;
    uintptr_t known_copy_low,known_copy_high;
    uint32_t query_count;
    /* occlusion queries this slot used last (reset before reuse); its first use resets them all */
    uint32_t queries_to_reset;
    int queries_reset_once;
    int host_sync,has_known_cpu_copy,known_copy_unbounded,status;
    /* TOTK: known CPU copies indexed by 4 KB page: scanning every pending copy per uniform read in
       async_read_known_full_copy would be 20% of the translation thread in game. kc_unindexed: a copy too large to
       index; that batch is scanned. */
    struct kc_node { uintptr_t page; const post_op *post; uint32_t next; } *kc_nodes;
    uint32_t kc_count, kc_cap, kc_head[1024]; int kc_unindexed;
    /* copies over 64 pages, checked one by one */
    const post_op **kc_big; uint32_t kc_big_count, kc_big_cap;
    /* every registered write (known copies and GPU outputs alike) by page, for forward_find's latest writer
       (forward_latest_batch); the same layout as the known-copy index */
    struct kc_node *wi_nodes; uint32_t wi_count, wi_cap, wi_head[1024]; int wi_unindexed;
    const post_op **wi_big; uint32_t wi_big_count, wi_big_cap;
    struct async_batch *next;
    /* write-chunk filter: batches claim increasing serials */
    uint64_t serial;
    /* TOTK_VK_INPLACE=2: the uniform blocks this batch bound in place, with the capture's bytes (the block at the submit
       call); compared with the game's memory when the batch completes: a block the game rewrote before the GPU was done */
    struct ipp_entry { uintptr_t address; uint32_t size; const uint8_t *capture; } *ipp; uint32_t ipp_n, ipp_cap;
} async_batch;
static uint64_t inplace_late_checked, inplace_late_differ;
static async_batch async_batches[DM_VK_BATCH_SLOTS];
#include "nvn_vulkan_drawstats.inc"
static uint64_t timestamp_delta(uint64_t begin,uint64_t end,unsigned bits);
#include "nvn_vulkan_passtime.inc"
static async_batch *async_first,*async_last,*async_current;
static pthread_cond_t async_condition=PTHREAD_COND_INITIALIZER;
static pthread_t async_thread;
static unsigned async_next_slot;
/* timeline events (TOTK_TIMELINE, MacNativeGames totk_timeline.h): commit = a command buffer submitted (a = ticket, b = 1
   when it ends a batch), fence = the completion thread saw the batch's fence (b = 0) / published it (b = 1) */
void totk_tl(int kind, uint64_t a, uint64_t b) __attribute__((weak));
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) __attribute__((weak));
/* TL_COMMIT, TL_GPU (t = GPU start on the GPU clock, a = ticket, b = GPU end), TL_FENCE */
#include "totk_timeline.h"
/* batches in flight (all DM_VK_BATCH_SLOTS): beginning a batch waits for a free slot, i.e.
   for the GPU to finish the batch that used it, so few slots hold the translation back to the GPU's pace. Still open-world
   scene, same draws: 3 slots 30.9 ms a frame (replay stalls ~5 ms in mtl_sub_begin), 8 slots 25.6 ms. */
static int async_initialized,async_stopping;
static uint64_t async_ring_stalls;
static uint64_t async_early_submits,async_logical_submits,async_early_submit_ns;
static pthread_t async_recording_thread;
// Whether this thread owns the current recording (set by mtl_sub_begin, cleared by async_commit_locked): only the recorder itself reads and
// writes it, so mtl_present can decide without holding queue_lock.
static _Thread_local int async_recording_owned;
static void async_chunk_report(void) {
    static uint64_t frame,early,logical,submit_ns;
    if(!presents||presents%60||presents==frame)return;
    uint64_t span=presents-frame;
    LOG("[vulkan-chunks] frame=%llu span=%llu early=%.2f/frame batches=%.2f/frame early_submit_wall=%.4f ms/frame max_chunks=%u min_draws=%u record_budget_us=%llu gpu_timestamp=logical_batch_span",
        (unsigned long long)presents,(unsigned long long)span,(async_early_submits-early)/(double)span,
        (async_logical_submits-logical)/(double)span,(async_early_submit_ns-submit_ns)/(span*1e6),
        ASYNC_CHUNK_CAPACITY,ASYNC_CHUNK_MIN_DRAWS,(unsigned long long)(chunk_budget(1)/1000));
    frame=presents;early=async_early_submits;logical=async_logical_submits;submit_ns=async_early_submit_ns;
}
// Called only before a draw at a natural pass boundary; never retire, publish, or reset batch resources/queries.
/* Would async_chunk_boundary_locked cut a chunk now? (Same tests, without its publish yield.) A render pass that has run
   64+ draws since the last chunk is ended when a cut is due, so long geometry
   passes reach the GPU every chunk budget rather than at the 256-draw batch commit; the pass restarts with LOAD (every
   pass renders in sysmem here, so that costs only the per-pass overhead). Still scene: off 17.2/17.2/17.2 ms, 64
   16.8/16.9/16.7/16.8, 32 16.8/16.8, 128 17.0/17.0; GPU idle 3-6 ms into the replay ~50% -> ~20%. */
#define ASYNC_CHUNK_SPLIT_DRAWS 64u
static int async_chunk_due(void) {
    async_batch *batch=async_current;unsigned split=ASYNC_CHUNK_SPLIT_DRAWS;
    if(!recording||!batch||batch->status!=ASYNC_RECORDING||screen_submission||batch->chunk_index+1>=ASYNC_CHUNK_CAPACITY)return 0;
    if(batch_draws-batch->chunk_draws<(split>ASYNC_CHUNK_MIN_DRAWS?split:ASYNC_CHUNK_MIN_DRAWS))return 0;
    uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC),budget=chunk_budget(async_first!=NULL);
    return now&&batch->chunk_started&&now>=batch->chunk_started&&now-batch->chunk_started>=budget;
}
/* The chunk's prelude: a command buffer submitted just ahead of the chunk's own, for work that may precede
   everything the chunk recorded so far -- an upload into an image the GPU has not touched in the chunk
   (image_upload). Recording it there instead of in the chunk keeps the open render pass open: in the chunk, each
   such upload ends the pass and restarts it with a load (nine 96-byte textures a frame in the cave). Opened on first
   use, closed and submitted with the chunk at both submit sites. TOTK_VK_UPLOAD_PRELUDE=0: off. */
static VkCommandBuffer prelude_command;static int prelude_open,prelude_recording;static uint64_t prelude_uploads;
static int prelude_on(void) { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_UPLOAD_PRELUDE");on=x?atoi(x):1;}return on; }
static VkCommandBuffer prelude_begin(void) {
    async_batch *batch=async_current;
    if(!prelude_open){
        if(!batch->preludes[batch->chunk_index]){VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            VK_CHECK(vkAllocateCommandBuffers(device,&allocation,&batch->preludes[batch->chunk_index]));}
        prelude_command=batch->preludes[batch->chunk_index];VK_CHECK(vkResetCommandBuffer(prelude_command,0));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_CHECK(vkBeginCommandBuffer(prelude_command,&begin));prelude_open=1;}
    return prelude_command;
}
/* fills the submit's command buffer list: the prelude (if open, closed here) then the chunk's buffer */
static uint32_t prelude_submit_list(VkCommandBuffer list[2],VkCommandBuffer chunk) {
    uint32_t n=0;
    if(prelude_open){VK_CHECK(vkEndCommandBuffer(prelude_command));list[n++]=prelude_command;prelude_open=0;}
    list[n++]=chunk;return n;
}
static void async_chunk_boundary_locked(void) {
    async_batch *batch=async_current;
    if(!recording||!batch||batch->status!=ASYNC_RECORDING)die("Vulkan chunk without recording batch");
    if(screen_submission||batch->chunk_index+1>=ASYNC_CHUNK_CAPACITY||batch_draws-batch->chunk_draws<ASYNC_CHUNK_MIN_DRAWS)return;
    uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);
    /* nothing in flight: the GPU is idle waiting for this frame's first work (the game submits only after the
       previous frame's fence), so it goes at once rather than after the 1 ms record budget */
    uint64_t budget=chunk_budget(async_first!=NULL);
    if(!now||!batch->chunk_started||now<batch->chunk_started||now-batch->chunk_started<budget)return;
    VK_CHECK(vkEndCommandBuffer(command));
    VkCommandBuffer list[2];VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=prelude_submit_list(list,command),.pCommandBuffers=list};
    // The final same-queue fence covers prior chunks; guest tickets and WSI signals remain exclusive to final submission.
    /* the capture's bytes are in before the GPU may read them */
    ucap_wait_current();
    VK_CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));if(totk_tl)totk_tl(TL_COMMIT,0,0);
    uint64_t submitted_at=dm_android_clock_ns(CLOCK_MONOTONIC);
    if(submitted_at>=now)async_early_submit_ns+=submitted_at-now;
    async_early_submits++;batch->chunk_index++;
    if(batch->chunk_index==batch->chunk_count){
        VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VK_CHECK(vkAllocateCommandBuffers(device,&allocation,&batch->chunks[batch->chunk_count]));batch->chunk_count++;
    }
    command=batch->command=batch->chunks[batch->chunk_index];
    VK_CHECK(vkResetCommandBuffer(command,0));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(command,&begin));graphics_command_reset();
    batch->chunk_draws=batch_draws;batch->chunk_started=dm_android_clock_ns(CLOCK_MONOTONIC);
}
static uint64_t post0_calls,post0_bytes,post0_copy_calls,post0_copy_bytes;
/* write-backs not copied: a later one in the batch covers them */
static uint64_t post0_superseded,post0_superseded_bytes;
/* published: bytes actually copied */
static uint64_t post0_site_calls[POST0_SITES],post0_site_bytes[POST0_SITES],post0_site_published[POST0_SITES];
static uint64_t known_copy_filter_calls,known_copy_filter_negative;
static void post0_publish_report(void) {
    static uint64_t frame,calls,bytes,copy_calls,copy_bytes;
    static uint64_t filter_calls,filter_negative;
    if(!presents||presents%60||presents==frame)return;
    int saved_errno=errno;uint64_t span=presents-frame;
    LOG("[vulkan-post0-publish] frame=%llu span=%llu calls=%.2f/frame bytes=%.4f MiB/frame copied_calls=%.2f/frame copied_bytes=%.4f MiB/frame",
        (unsigned long long)presents,(unsigned long long)span,(post0_calls-calls)/(double)span,(post0_bytes-bytes)/(span*1048576.),
        (post0_copy_calls-copy_calls)/(double)span,(post0_copy_bytes-copy_bytes)/(span*1048576.));
    { static uint64_t c0[POST0_SITES],b0[POST0_SITES],w0[POST0_SITES];char line[512];int n=0;static const char *names[POST0_SITES]={"other","ssbo","texel","copy-gpu","copy-cpu","tex-to-buf"};
      for(int i=0;i<POST0_SITES;i++){if(post0_site_calls[i]-c0[i])n+=snprintf(line+n,sizeof line-(size_t)n," %s=%.1f/%.2fMiB/%.2fMiB",names[i],(post0_site_calls[i]-c0[i])/(double)span,(post0_site_bytes[i]-b0[i])/(span*1048576.),(post0_site_published[i]-w0[i])/(span*1048576.));
          c0[i]=post0_site_calls[i];b0[i]=post0_site_bytes[i];w0[i]=post0_site_published[i];}
      static uint64_t sc,sb,ip,wr;extern uint64_t ssbo_in_place_writes_total(void),ssbo_war_barriers_total(void),ssbo_pass_barriers_total(void);uint64_t inplace=ssbo_in_place_writes_total(),war=ssbo_war_barriers_total(),pb=ssbo_pass_barriers_total();static uint64_t pb0;
      LOG("[vulkan-post0-sites] frame=%llu per frame: calls/bytes/published%s; superseded %.1f / %.2f MiB; storage writes in place %.1f (%.1f waited for earlier reads, %.1f for writes outside their pass)",(unsigned long long)presents,n?line:" none",
          (post0_superseded-sc)/(double)span,(post0_superseded_bytes-sb)/(span*1048576.),(inplace-ip)/(double)span,(war-wr)/(double)span,(pb-pb0)/(double)span);pb0=pb;sc=post0_superseded;sb=post0_superseded_bytes;ip=inplace;wr=war; }
    uint64_t checked=known_copy_filter_calls-filter_calls,negative=known_copy_filter_negative-filter_negative;
    LOG("[vulkan-known-copy-filter] frame=%llu span=%llu calls=%.2f/frame negative=%.2f/frame original_scan=%.2f/frame negative_pct=%.2f (empty or disjoint bounds only)",
        (unsigned long long)presents,(unsigned long long)span,checked/(double)span,negative/(double)span,(checked-negative)/(double)span,checked?negative*100.0/checked:0);
    filter_calls=known_copy_filter_calls;filter_negative=known_copy_filter_negative;
    frame=presents;calls=post0_calls;bytes=post0_bytes;copy_calls=post0_copy_calls;copy_bytes=post0_copy_bytes;errno=saved_errno;
}
static uint64_t snapshot_known_copy_hits,snapshot_known_copy_read_saved,snapshot_known_copy_overlay_saved,snapshot_known_copy_final_bytes;
static uint64_t pending_index_queries[2],pending_index_nodes[2],pending_index_empty[2],pending_index_registered[2],pending_index_peak_records;

// Register only completed fields; unknown outputs are visible before encoding, while known CPU copies retain the original publication chain.
/* Pending-write page tables: every registered write (copies, GPU outputs, events, reports) counts +1 on each 4 KiB
   guest page it touches while its batch is unpublished, -1 when the batch publishes (async_publish_locked); each 16
   MiB leaf keeps the total of its pages. A read over pages that all count 0 has nothing pending, so forward_find /
   async_copy_pending / async_unknown / known-copy reads / overlays answer "none" without walking the pending chains.
   One table holds all writes, one only unknown GPU writes and host effects (what async_unknown asks about). Exact
   counts, no hashing: a hash filter over 64 KiB chunks would probe a slot per chunk of every texture of every draw
   (a 4 MiB texture: 64 probes), ~9% of the replay. */
#define PP_PAGE_SHIFT 12
#define PP_LEAF_SHIFT 24
/* guest addresses below 2^40 */
#define PP_TOP (1u << (40 - PP_LEAF_SHIFT))
/* used: one bit per page with a nonzero count, so a query over a texture's hundreds of pages tests 64 at a time */
/* gen: changes of this 16 MiB leaf */
typedef struct { uint32_t total, gen; uint32_t count[1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)]; uint64_t used[(1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) / 64]; } pp_leaf;
/* outside: registrations above 2^40 (then "maybe") */
typedef struct { pp_leaf *leaf[PP_TOP]; uint64_t outside; } pp_table;
static pp_table pp_all, pp_unknown;
static uint64_t write_chunk_serial;
static uint64_t write_chunk_skips, write_chunk_checks;
static void ucap_mark_dirty(uintptr_t address, uint64_t bytes);
/* advance with every change to pp_unknown / pp_all: cached "nothing pending" answers are valid while they stand */
static uint64_t pp_unknown_gen, pp_all_gen;
static void pp_add(pp_table *t, uintptr_t address, uint64_t bytes, int delta) {
    if (!bytes) return;
    if (t == &pp_unknown) pp_unknown_gen++; else pp_all_gen++;
    uintptr_t last_top = UINTPTR_MAX;
    if (delta > 0) ucap_mark_dirty(address, bytes);
    if (bytes > UINTPTR_MAX - address || ((address + bytes - 1) >> 40)) { t->outside += delta; return; }
    for (uintptr_t page = address >> PP_PAGE_SHIFT, last = (address + bytes - 1) >> PP_PAGE_SHIFT; page <= last; page++) {
        uintptr_t top = page >> (PP_LEAF_SHIFT - PP_PAGE_SHIFT); pp_leaf *l = t->leaf[top];
        if (!l) { if (delta < 0) die("Vulkan pending page table underflow"); l = t->leaf[top] = calloc(1, sizeof *l); if (!l) die("Vulkan pending page table"); }
        if (top != last_top) { l->gen++; last_top = top; }
        uint32_t i = page & ((1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) - 1), *c = &l->count[i];
        if (delta < 0 && (!*c || !l->total)) die("Vulkan pending page table underflow");
        *c += delta; l->total += delta;
        if (*c) l->used[i >> 6] |= 1ull << (i & 63); else l->used[i >> 6] &= ~(1ull << (i & 63));
    }
}
/* A signature of the table's state over [address, address+size): equal signatures mean no registration or publication
   touched its leaves since, so a cached answer about the range stands (per-binding memos). Ranges over more than two
   leaves, or outside the table, take the whole table's generation. */
static uint64_t pp_range_sig(const pp_table *t, uintptr_t address, uint64_t size) {
    uint64_t all = t == &pp_unknown ? pp_unknown_gen : pp_all_gen;
    if (!size || size > UINTPTR_MAX - address || t->outside || ((address + size - 1) >> 40)) return all;
    uintptr_t a = address >> PP_LEAF_SHIFT, b = (address + size - 1) >> PP_LEAF_SHIFT;
    if (b > a + 1) return all;
    const pp_leaf *la = t->leaf[a], *lb = t->leaf[b];
    /* never equal to a whole-table generation */
    return ((uint64_t)(la ? la->gen : 0) << 32 | (lb ? lb->gen : 0)) ^ ((uint64_t)a << 48) ^ 0x4000000000000000ull;
}
/* 0: no unpublished write overlaps [address, address+size); 1: maybe (some page of it has one). */
static int pp_any_pages(const pp_table *t, uintptr_t address, uint64_t size);
static int pp_any(const pp_table *t, uintptr_t address, uint64_t size) {
    /* the bitmap against the page counts */
    static int check = -1; if (check < 0) check = exp_env("ppcheck");
    int r = pp_any_pages(t, address, size);
    if (check && size && size <= UINTPTR_MAX - address && !t->outside && !((address + size - 1) >> 40)) {
        int slow = 0;
        for (uintptr_t q = address >> PP_PAGE_SHIFT; q <= (address + size - 1) >> PP_PAGE_SHIFT && !slow; q++) { const pp_leaf *l = t->leaf[q >> (PP_LEAF_SHIFT - PP_PAGE_SHIFT)]; slow = l && l->count[q & ((1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) - 1)]; }
        if (slow != r) die("Vulkan pending page bitmap mismatch: %#llx+%llu", (unsigned long long)address, (unsigned long long)size);
    }
    return r;
}
static int pp_any_pages(const pp_table *t, uintptr_t address, uint64_t size) {
    write_chunk_checks++;
    if (!size || size > UINTPTR_MAX - address || t->outside || ((address + size - 1) >> 40)) return 1;
    uintptr_t page = address >> PP_PAGE_SHIFT, last = (address + size - 1) >> PP_PAGE_SHIFT;
    while (page <= last) {
        uintptr_t top = page >> (PP_LEAF_SHIFT - PP_PAGE_SHIFT), leaf_end = ((top + 1) << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) - 1;
        const pp_leaf *l = t->leaf[top]; uintptr_t end = last < leaf_end ? last : leaf_end;
        if (l && l->total) {
            uint32_t lo = page & ((1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) - 1), hi = end & ((1u << (PP_LEAF_SHIFT - PP_PAGE_SHIFT)) - 1);
            for (uint32_t w = lo >> 6; w <= hi >> 6; w++) {
                uint64_t m = l->used[w];
                if (w == lo >> 6) m &= ~0ull << (lo & 63);
                if (w == hi >> 6) m &= ~0ull >> (63 - (hi & 63));
                if (m) return 1;
            }
        }
        page = end + 1;
    }
    write_chunk_skips++; return 0;
}
static int write_chunk_maybe(uintptr_t address, uint64_t size) { return pp_any(&pp_all, address, size); }
static void forward_index_add(async_batch *batch,const post_op *p) {
    if(batch->wi_unindexed)return;uintptr_t start=(uintptr_t)p->destination;
    if(p->size>UINTPTR_MAX-start){batch->wi_unindexed=1;return;}
    uintptr_t first=start>>12,last=(start+p->size-1)>>12;
    if(last-first>=64){if(batch->wi_big_count==batch->wi_big_cap){batch->wi_big_cap=batch->wi_big_cap?batch->wi_big_cap*2:64;batch->wi_big=realloc(batch->wi_big,batch->wi_big_cap*sizeof *batch->wi_big);if(!batch->wi_big)die("forward write index");}
        batch->wi_big[batch->wi_big_count++]=p;if(batch->wi_big_count>4096)batch->wi_unindexed=1;return;}
    for(uintptr_t page=first;page<=last;page++){
        if(batch->wi_count==batch->wi_cap){batch->wi_cap=batch->wi_cap?batch->wi_cap*2:4096;batch->wi_nodes=realloc(batch->wi_nodes,batch->wi_cap*sizeof *batch->wi_nodes);if(!batch->wi_nodes)die("forward write index");}
        uint32_t h=(uint32_t)(page*0x9e3779b1u)>>22;batch->wi_nodes[batch->wi_count]=(struct kc_node){page,p,batch->wi_head[h]};batch->wi_head[h]=++batch->wi_count;}
}
static void post_register_range(post_op *p) {
    if(!recording||!async_current||!p||!p->order||p->range_registered)die("Vulkan pending range registration state");
    p->census_piece=sl.piece;p->census_frame=sl.submission;
    { uintptr_t wa=0;uint64_t wb=0;
      if(p->kind==0&&p->destination&&p->size){wa=(uintptr_t)p->destination;wb=p->size;}
      else if(p->kind==1){wa=(uintptr_t)p->event;wb=4;}
      else if(p->kind==2){wa=(uintptr_t)p->destination;wb=16;}
      if(wa){pp_add(&pp_all,wa,wb,+1);p->pp_all_addr=wa;p->pp_all_size=wb;}
      if(wa&&frz_noting)frz_note(wa,wb); }
    post_write_order++;
    if(p->kind==0&&p->destination&&p->size)forward_index_add(async_current,p);
    dm_pending_kind kind;uintptr_t address;uint64_t bytes;
    if(p->kind==0&&!p->source.buffer&&p->destination&&p->size){kind=DM_PENDING_UNKNOWN;address=(uintptr_t)p->destination;bytes=p->size;}
    else if(p->kind==1){kind=DM_PENDING_HOST_EFFECT;address=(uintptr_t)p->event;bytes=4;}
    else if(p->kind==2){kind=DM_PENDING_HOST_EFFECT;address=(uintptr_t)p->destination;bytes=16;}
    else{if(p->kind!=0)die("Vulkan pending range unexpected post kind");
        if(p->source.buffer&&p->destination&&p->size){
            async_batch *batch=async_current;uintptr_t start=(uintptr_t)p->destination;
            // Unrepresentable ranges force the full scan; summaries never authorize a copy.
            if(p->size>UINTPTR_MAX-start)batch->known_copy_unbounded=1;
            else if(!batch->known_copy_unbounded){uintptr_t end=start+p->size;
                if(!batch->has_known_cpu_copy){batch->known_copy_low=start;batch->known_copy_high=end;}
                else{if(start<batch->known_copy_low)batch->known_copy_low=start;if(end>batch->known_copy_high)batch->known_copy_high=end;}
            }
            batch->has_known_cpu_copy=1;
            if(!batch->kc_unindexed){uintptr_t first=start>>12,last=(start+p->size-1)>>12;
                if(p->size>UINTPTR_MAX-start)batch->kc_unindexed=1;
                else if(last-first>=64){if(batch->kc_big_count==batch->kc_big_cap){batch->kc_big_cap=batch->kc_big_cap?batch->kc_big_cap*2:64;batch->kc_big=realloc(batch->kc_big,batch->kc_big_cap*sizeof *batch->kc_big);if(!batch->kc_big)die("known copy index");}
                    batch->kc_big[batch->kc_big_count++]=p;if(batch->kc_big_count>4096)batch->kc_unindexed=1;}
                else for(uintptr_t page=first;page<=last;page++){
                    if(batch->kc_count==batch->kc_cap){batch->kc_cap=batch->kc_cap?batch->kc_cap*2:4096;batch->kc_nodes=realloc(batch->kc_nodes,batch->kc_cap*sizeof *batch->kc_nodes);if(!batch->kc_nodes)die("known copy index");}
                    uint32_t h=(uint32_t)(page*0x9e3779b1u)>>22;batch->kc_nodes[batch->kc_count]=(struct kc_node){page,p,batch->kc_head[h]};batch->kc_head[h]=++batch->kc_count;}}
        }
        p->range_registered=1;return;}
    pp_add(&pp_unknown,address,bytes,+1);p->pp_unknown_addr=address;p->pp_unknown_size=bytes;
    if(dm_pending_ranges_insert(&async_current->pending_ranges,kind,address,bytes,p->order)!=1)die("Vulkan pending range insert: %s",strerror(errno));
    p->range_registered=1;pending_index_registered[kind]++;
    uint64_t records=async_current->pending_ranges.collections[0].count+async_current->pending_ranges.collections[1].count;
    if(records>pending_index_peak_records)pending_index_peak_records=records;
}
static int pending_range_overlap(const async_batch *batch,dm_pending_kind kind,uintptr_t address,uint64_t size,uint64_t before) {
    if(!batch)return 0;size_t visited=0;
    int result=dm_pending_ranges_overlap(&batch->pending_ranges,kind,address,size,before,&visited);
    if(result<0)die("Vulkan pending range query: %s",strerror(errno));
    pending_index_queries[kind]++;pending_index_nodes[kind]+=visited;pending_index_empty[kind]+=!result;return result;
}
static void pending_index_report(void) {
    static uint64_t frame,queries[2],nodes[2],empty[2],registered[2];
    if(!presents||presents%60||presents==frame)return;uint64_t frames=presents-frame,retained=0;
    for(unsigned i=0;i<DM_VK_BATCH_SLOTS;i++)retained+=async_batches[i].pending_ranges.retained_bytes;
    double q0=pending_index_queries[0]-queries[0],q1=pending_index_queries[1]-queries[1];
    LOG("[vulkan-pending-index] frame=%llu unknown/host_queries=%.1f/%.1f per_frame tree_nodes/query=%.2f/%.2f no_overlap=%.1f/%.1f%% inserted=%.1f/%.1f per_frame peak_batch_records=%llu retained_heap=%.1f KiB",(unsigned long long)presents,
        q0/frames,q1/frames,q0?(pending_index_nodes[0]-nodes[0])/q0:0,q1?(pending_index_nodes[1]-nodes[1])/q1:0,
        q0?(pending_index_empty[0]-empty[0])*100/q0:0,q1?(pending_index_empty[1]-empty[1])*100/q1:0,
        (pending_index_registered[0]-registered[0])/(double)frames,(pending_index_registered[1]-registered[1])/(double)frames,(unsigned long long)pending_index_peak_records,retained/1024.);
    frame=presents;for(unsigned i=0;i<2;i++){queries[i]=pending_index_queries[i];nodes[i]=pending_index_nodes[i];empty[i]=pending_index_empty[i];registered[i]=pending_index_registered[i];}
}

static int async_overlap(uintptr_t address,uint64_t size,uintptr_t start,uint64_t bytes) {
    if(!size||!bytes)return 0;
    if(size>UINTPTR_MAX-address||bytes>UINTPTR_MAX-start)die("Vulkan asynchronous visibility range overflow");
    return address<start+bytes&&start<address+size;
}

// Metadata access and retirement hold queue_lock; in-flight copy snapshots cannot be released during recording.
static void async_overlay_chain(const post_op *copies,uintptr_t address,void *destination,uint64_t size) {
    for(const post_op *p=copies;p;p=p->copy_next)if(p->destination&&p->source.buffer&&async_overlap(address,size,(uintptr_t)p->destination,p->size)){
        uintptr_t start=(uintptr_t)p->destination,low=address>start?address:start;
        uintptr_t high=address+size<start+p->size?address+size:start+p->size;
        memcpy((uint8_t*)destination+low-address,(const uint8_t*)p->source.mapped+low-start,high-low);
    }
}

static int async_known_copy_may_overlap(const async_batch *batch,uintptr_t address,uint64_t size);

static void async_overlay_copies(uintptr_t address,void *destination,uint64_t size) {
    if(!size||size>UINTPTR_MAX-address)return;
    if(!write_chunk_maybe(address,size))return;
    for(const async_batch *b=async_first;b;b=b->next)if(async_known_copy_may_overlap(b,address,size))async_overlay_chain(b->copies,address,destination,size);
    if(async_known_copy_may_overlap(async_current,address,size))async_overlay_chain(post_copy_first,address,destination,size);
}

static const post_op *async_version_chain(const post_op *copies,uintptr_t address,uint64_t size,const post_op *version) {
    for(const post_op *p=copies;p;p=p->copy_next)
        if(p->destination&&p->source.buffer&&async_overlap(address,size,(uintptr_t)p->destination,p->size))version=p;
    return version;
}

static const post_op *async_copy_version(uintptr_t address,uint64_t size) {
    if(!size||size>UINTPTR_MAX-address)return NULL;
    const post_op *version=NULL;
    for(const async_batch *b=async_first;b;b=b->next)if(async_known_copy_may_overlap(b,address,size))version=async_version_chain(b->copies,address,size,version);
    if(async_known_copy_may_overlap(async_current,address,size))version=async_version_chain(post_copy_first,address,size,version);
    return version;
}

static int async_copy_pending(uintptr_t address,uint64_t size) {
    if(!write_chunk_maybe(address,size))return 0;
    return async_copy_version(address,size)!=NULL;
}

static int pending_batch_unknown(const async_batch *batch,uintptr_t address,uint64_t size) {
    if(!batch)return 0;
    int result=pending_range_overlap(batch,DM_PENDING_UNKNOWN,address,size,DM_PENDING_ORDER_ALL)||pending_range_overlap(batch,DM_PENDING_HOST_EFFECT,address,size,DM_PENDING_ORDER_ALL);
    return result;
}

static int async_unknown(uintptr_t address,uint64_t size) {
    if(size>UINTPTR_MAX-address)die("Vulkan asynchronous read range overflow");
    if(!pp_any(&pp_unknown,address,size))return 0;
    for(const async_batch *b=async_first;b;b=b->next)
        if(pending_batch_unknown(b,address,size))return 1;
    return pending_batch_unknown(async_current,address,size);
}

typedef struct known_copy_read {
    const post_op *latest;
    uint64_t overlay_bytes;
} known_copy_read;
static int pending_range_overlap(const async_batch *batch,dm_pending_kind kind,uintptr_t address,uint64_t size,uint64_t before);
/* The batch's latest known copy over [address, address+size) from its page index; same answer as the chain walk
   below (an overlapping unknown write rejects; the latest overlapping copy). -1: not indexed, walk instead. The
   caller only uses the latest copy (and whether it covers the range), so only that is tracked: collecting every
   overlapping copy, de-duplicating them pairwise and summing their overlap is quadratic in the copies to one page,
   and TOTK rewrites one small constant buffer before each of ~80 one-draw cube-map passes: 59% of those passes' CPU
   time, growing from 25 to 43 us per pass across the batch. TOTK_VK_EXP=kcall: the full collection (A/B). */
static int async_known_copy_indexed_all(const async_batch *batch,uintptr_t address,uint64_t size,known_copy_read *read);
static int async_known_copy_indexed(const async_batch *batch,uintptr_t address,uint64_t size,known_copy_read *read) {
    { static int all=-1;if(all<0)all=exp_env("kcall");if(all)return async_known_copy_indexed_all(batch,address,size,read); }
    if(!batch||batch->kc_unindexed)return -1;
    if(pending_range_overlap(batch,DM_PENDING_UNKNOWN,address,size,DM_PENDING_ORDER_ALL))return 0;
    /* orders restart per batch: the latest within this batch; a later batch overrides (callers go oldest first) */
    const post_op *latest=NULL;
    for(uintptr_t page=address>>12;page<=(address+size-1)>>12;page++){
        uint32_t h=(uint32_t)(page*0x9e3779b1u)>>22;
        for(uint32_t i=batch->kc_head[h];i;i=batch->kc_nodes[i-1].next){const struct kc_node *k=&batch->kc_nodes[i-1];if(k->page!=page)continue;
            const post_op *p=k->post;uintptr_t start=(uintptr_t)p->destination;if(address>=start+p->size||start>=address+size)continue;
            if(!latest||p->order>latest->order)latest=p;}
    }
    for(uint32_t i=0;i<batch->kc_big_count;i++){const post_op *p=batch->kc_big[i];uintptr_t start=(uintptr_t)p->destination;
        if(address>=start+p->size||start>=address+size)continue;if(!latest||p->order>latest->order)latest=p;}
    if(latest){uintptr_t start=(uintptr_t)latest->destination;
        uintptr_t low=address>start?address:start,high=address+size<start+latest->size?address+size:start+latest->size;
        /* the latest copy's own overlap: the caller checks it covers the range */
        read->latest=latest;read->overlay_bytes=high-low;}
    return 1;
}
static int async_known_copy_indexed_all(const async_batch *batch,uintptr_t address,uint64_t size,known_copy_read *read) {
    if(!batch||batch->kc_unindexed)return -1;
    if(pending_range_overlap(batch,DM_PENDING_UNKNOWN,address,size,DM_PENDING_ORDER_ALL))return 0;
    const post_op *found[256];unsigned n=0;
    for(uintptr_t page=address>>12;page<=(address+size-1)>>12;page++){
        uint32_t h=(uint32_t)(page*0x9e3779b1u)>>22;
        for(uint32_t i=batch->kc_head[h];i;i=batch->kc_nodes[i-1].next){const struct kc_node *k=&batch->kc_nodes[i-1];if(k->page!=page)continue;
            const post_op *p=k->post;uintptr_t start=(uintptr_t)p->destination;if(address>=start+p->size||start>=address+size)continue;
            unsigned j=0;while(j<n&&found[j]!=p)j++;if(j<n)continue;if(n==256)return -1;found[n++]=p;}
    }
    for(uint32_t i=0;i<batch->kc_big_count;i++){const post_op *p=batch->kc_big[i];uintptr_t start=(uintptr_t)p->destination;
        if(address>=start+p->size||start>=address+size)continue;if(n==256)return -1;found[n++]=p;}
    /* orders restart per batch: the latest within this batch; a later batch overrides (callers go oldest first) */
    const post_op *latest=NULL;
    for(unsigned i=0;i<n;i++){const post_op *p=found[i];uintptr_t start=(uintptr_t)p->destination;
        uintptr_t low=address>start?address:start,high=address+size<start+p->size?address+size:start+p->size;
        if(high-low>UINT64_MAX-read->overlay_bytes)return 0;read->overlay_bytes+=high-low;
        if(!latest||p->order>latest->order)latest=p;}
    if(latest)read->latest=latest;
    return 1;
}
static int async_known_copy_chain(const post_op *copies,const post_op *posts,int host_sync,uintptr_t address,uint64_t size,known_copy_read *read) {
    for(const post_op *p=copies;p;p=p->copy_next){
        if(!p->destination||!p->size)continue;uintptr_t start=(uintptr_t)p->destination;
        if(p->size>UINTPTR_MAX-start)return 0;
        if(address>=start+p->size||start>=address+size)continue;
        // Any overlapping GPU/unknown write retains the original path; a later full CPU copy never replaces real dependency preflight.
        if(!p->source.buffer)return 0;
        uintptr_t low=address>start?address:start,high=address+size<start+p->size?address+size:start+p->size;
        if(high-low>UINT64_MAX-read->overlay_bytes)return 0;
        read->overlay_bytes+=high-low;read->latest=p;
    }
    if(host_sync)for(const post_op *p=posts;p;p=p->next){
        uintptr_t start;uint64_t bytes;
        if(p->kind==1){start=(uintptr_t)p->event;bytes=4;}
        else if(p->kind==2){start=(uintptr_t)p->destination;bytes=16;}
        else continue;
        if(bytes>UINTPTR_MAX-start||(address<start+bytes&&start<address+size))return 0;
    }
    return 1;
}
// The request is already overflow-checked; only reject impossible overlap, never infer coverage, version or FIFO visibility.
static int async_known_copy_may_overlap(const async_batch *batch,uintptr_t address,uint64_t size) {
    return batch&&batch->has_known_cpu_copy&&(batch->known_copy_unbounded||
        (address<batch->known_copy_high&&batch->known_copy_low<address+size));
}
static int async_read_known_full_copy(uintptr_t address,void *destination,uint64_t size) {
    if(address&&size&&!write_chunk_maybe(address,size))return 0;
    // queue_lock keeps the immutable copy source alive; avoid only the base read/old overlays, without changing publication order.
    if(!address||!destination||!size||size>UINTPTR_MAX-address)return 0;
    // Reject only when every known-destination bounding interval is disjoint; otherwise retain the complete original scan.
    known_copy_filter_calls++;
    int may_overlap=async_known_copy_may_overlap(async_current,address,size);
    for(const async_batch *b=async_first;!may_overlap&&b;b=b->next)may_overlap=async_known_copy_may_overlap(b,address,size);
    if(!may_overlap){known_copy_filter_negative++;return 0;}
    known_copy_read read={0};
    for(const async_batch *b=async_first;b;b=b->next){
        /* host_sync batches: their event/report writes are DM_PENDING_HOST_EFFECT ranges (registered before
           submission), so the page index plus that range check equals the full chain scan (20% of the translation
           thread) */
        if(b->host_sync&&pending_range_overlap(b,DM_PENDING_HOST_EFFECT,address,size,DM_PENDING_ORDER_ALL))return 0;
        known_copy_read before=read;int r=async_known_copy_indexed(b,address,size,&read);
        if(r==0)return 0;if(r<0){read=before;if(!async_known_copy_chain(b->copies,b->posts,0,address,size,&read))return 0;}
    }
    /* event/report posts register at creation */
    { const async_batch *b=async_current;if(pending_host_sync&&pending_range_overlap(b,DM_PENDING_HOST_EFFECT,address,size,DM_PENDING_ORDER_ALL))return 0;
      int r=async_known_copy_indexed(b,address,size,&read);
      if(r==0)return 0;if(r<0&&!async_known_copy_chain(post_copy_first,post_first,pending_host_sync,address,size,&read))return 0; }
    if(!read.latest)return 0;
    const post_op *p=read.latest;uintptr_t start=(uintptr_t)p->destination;
    // A latest partial write must never fall back to an old full one; validate nonzero offsets against both post and actual source bounds.
    if(address<start||size>p->size||address-start>p->size-size||!p->source.mapped||!p->source.owner||
        size>p->source.size||address-start>p->source.size-size||read.overlay_bytes<size)return 0;
    memcpy(destination,(const uint8_t*)p->source.mapped+address-start,size);
    snapshot_known_copy_hits++;snapshot_known_copy_read_saved+=size;
    snapshot_known_copy_overlay_saved+=read.overlay_bytes-size;snapshot_known_copy_final_bytes+=size;return 1;
}

static int pending_batch_has_unknown(const async_batch *batch) {
    if(!batch)return 0;int result=batch->pending_ranges.collections[0].count||batch->pending_ranges.collections[1].count;
    return result;
}

static int async_have_unknown(void) {
    for(const async_batch *b=async_first;b;b=b->next)if(pending_batch_has_unknown(b))return 1;
    return pending_batch_has_unknown(async_current);
}

static uint64_t async_batch_temporary_bytes(void) {
    uint64_t temporary=active_buffer_bytes-read_cache_bytes-immutable_buffer_bytes;
    return async_current&&temporary>async_current->temporary_base?temporary-async_current->temporary_base:0;
}

static uint64_t seed_posts,seed_published;
static void async_publish_locked(async_batch *batch) {
    uint64_t started=dm_android_clock_ns(CLOCK_MONOTONIC);
    /* TOTK_VK_INPLACE=2: in-place uniform blocks against the capture, now that the GPU is done with them */
    if(batch->ipp_n){
        for(uint32_t i=0;i<batch->ipp_n;i++){const struct ipp_entry *e=&batch->ipp[i];inplace_late_checked++;
            if(memcmp(e->capture,(const void*)e->address,e->size)){inplace_late_differ++;static unsigned said;
                if(said++<80){size_t k=0;while(k<e->size&&e->capture[k]==((const uint8_t*)e->address)[k])k++;
                    LOG("[vulkan-inplace-late] frame %llu block %#llx+%u was rewritten before the batch completed (from byte %zu; %llu of %llu differ)",(unsigned long long)presents,(unsigned long long)e->address,e->size,k,(unsigned long long)inplace_late_differ,(unsigned long long)inplace_late_checked);}}}
        batch->ipp_n=0;}
    if(batch->times){
        uint64_t times[2];memcpy(times,batch->query_readback.mapped,sizeof times);
        gpu_total_ns+=timestamp_delta(times[0],times[1],gpu_timestamp_bits)*gpu_timestamp_period;
        /* GPU clock: offset found offline */
        if(totk_tl_at)totk_tl_at(TL_GPU,(uint64_t)(times[0]*gpu_timestamp_period),batch->ticket,(uint64_t)(times[1]*gpu_timestamp_period));
    }
    if(batch->query_count)memcpy(query_counts,(const uint8_t*)batch->query_readback.mapped+ASYNC_TIMESTAMP_BYTES,batch->query_count*sizeof *query_counts);
    graphics_probe_query_batch(batch->ticket,query_counts,batch->query_count);
    /* A write-back that a later full write-back of the same range in this batch overwrites is not copied: this
       function publishes the whole batch in order, so nobody sees the bytes in between. (One 128.5 KiB storage
       buffer is written by ~260 draws a frame in the open world: copied back each time, 33 MiB a frame under
       queue_lock.) */
    enum { SUPERSEDE_BUCKETS = 256 };
    struct { uintptr_t dst; uint64_t size; const post_op *last; } supersede[SUPERSEDE_BUCKETS];unsigned supersede_used=0;
    memset(supersede,0,sizeof supersede);
    for(const post_op *p=batch->posts;p;p=p->next)if(p->kind==0&&!p->seed&&p->destination&&p->size>=4096){
        unsigned h=(unsigned)((((uintptr_t)p->destination>>6)^p->size)*0x9e3779b1u)>>24;
        for(unsigned probe=0;probe<SUPERSEDE_BUCKETS;probe++,h=(h+1)%SUPERSEDE_BUCKETS){
            if(!supersede[h].last){if(supersede_used*2>=SUPERSEDE_BUCKETS)break;supersede_used++;supersede[h].dst=(uintptr_t)p->destination;supersede[h].size=p->size;supersede[h].last=p;break;}
            if(supersede[h].dst==(uintptr_t)p->destination&&supersede[h].size==p->size){supersede[h].last=p;break;}
        }
    }
    /* before this batch publishes anything */
    for(post_op *p=batch->posts;p;p=p->next)if(p->kind==6)image_writeback_check(p);
    for(post_op *p=batch->posts,*next;p;p=next){
        next=p->next;
        /* this batch's writes stop being pending */
        if(p->pp_all_size)pp_add(&pp_all,p->pp_all_addr,p->pp_all_size,-1);
        if(p->pp_unknown_size)pp_add(&pp_unknown,p->pp_unknown_addr,p->pp_unknown_size,-1);
        if(p->kind==0){
            // After the real fence, borrow only a complete CPU-ready owned source; helper=0 copied nothing, preserving original publication order.
            post0_calls++;post0_bytes+=p->size;const vk_buffer *output=&p->output;int copied=0;uint64_t seeded_before=seed_published;int seeded=p->seed!=NULL,superseded=0;
            unsigned site=p->site<POST0_SITES?p->site:0;post0_site_calls[site]++;post0_site_bytes[site]+=p->size;
            /* destinations of write-backs, one frame in 300 */
            { static int diag=-1;if(diag<0){const char *x=getenv("TOTK_VK_POST0_DIAG");diag=x&&atoi(x);}
              static struct { uintptr_t dst; uint64_t size, n; } d[1024]; static unsigned nd; static uint64_t at;
              if(diag&&site==POST0_SITE_SSBO&&presents%300==10){if(presents!=at){nd=0;at=presents;}
                  unsigned i=0;while(i<nd&&(d[i].dst!=(uintptr_t)p->destination||d[i].size!=p->size))i++;
                  if(i==nd&&nd<1024){d[nd].dst=(uintptr_t)p->destination;d[nd].size=p->size;d[nd].n=0;nd++;}if(i<nd)d[i].n++;}
              if(diag&&presents%300==11&&nd){uint64_t all=0,calls=0;for(unsigned i=0;i<nd;i++){all+=d[i].size*d[i].n;calls+=d[i].n;}
                  LOG("[vulkan-post0-diag] frame %llu: %u destinations, %llu write-backs, %.2f MiB",(unsigned long long)at,nd,(unsigned long long)calls,all/1048576.);
                  for(int shown=0;shown<16;shown++){unsigned best=nd;for(unsigned i=0;i<nd;i++)if(d[i].n&&(best==nd||d[i].size*d[i].n>d[best].size*d[best].n))best=i;if(best==nd)break;
                      LOG("[vulkan-post0-diag]   dst %#llx size %llu: %llu write-backs",(unsigned long long)d[best].dst,(unsigned long long)d[best].size,(unsigned long long)d[best].n);d[best].n=0;}
                  nd=0;}
              if(0){for(unsigned i=0;i<nd;i++)LOG("[vulkan-post0-diag] frame %llu dst %#llx size %llu: %llu write-backs",(unsigned long long)at,(unsigned long long)d[i].dst,(unsigned long long)d[i].size,(unsigned long long)d[i].n);nd=0;} }
            /* TOTK: a shader-written buffer seeded on the CPU publishes only the 64-byte blocks the GPU changed: the
               full range would overwrite CPU writes the game made meanwhile elsewhere in the buffer (stale particle
               parameters). */
            if(p->seed&&output->mapped){const uint8_t *now=output->mapped,*was=p->seed;uint8_t *dst=p->destination;uint64_t n=p->size;
                for(uint64_t off=0;off<n;){uint64_t len=n-off<64?n-off:64;if(!memcmp(now+off,was+off,len)){off+=len;continue;}
                    uint64_t run=off;while(run<n){uint64_t l=n-run<64?n-run:64;if(!memcmp(now+run,was+run,l))break;run+=l;}
                    dm_guest_write_prepare(dst+off,run-off);memcpy(dst+off,now+off,run-off);seed_published+=run-off;off=run;}
                seed_posts++;free(p->seed);p->seed=NULL;copied=1;}
            if(!copied&&p->size>=4096){unsigned h=(unsigned)((((uintptr_t)p->destination>>6)^p->size)*0x9e3779b1u)>>24;
                for(unsigned probe=0;probe<SUPERSEDE_BUCKETS&&supersede[h].last;probe++,h=(h+1)%SUPERSEDE_BUCKETS)
                    if(supersede[h].dst==(uintptr_t)p->destination&&supersede[h].size==p->size){if(supersede[h].last!=p){copied=superseded=1;post0_superseded++;post0_superseded_bytes+=p->size;}break;}}
            if(!copied){post0_copy_calls++;post0_copy_bytes+=p->size;dm_guest_write_prepare(p->destination,p->size);memcpy(p->destination,output->mapped,p->size);}
            post0_site_published[site]+=superseded?0:seeded?seed_published-seeded_before:p->size;
            /* TOTK_VK_WATCH_MEM: GPU write-backs over the watched window, with the watched word when they cover it */
            { uintptr_t d=(uintptr_t)p->destination;static unsigned shown;
              if(gpu_watch_hit(d,p->size)&&shown++<400){uintptr_t w=gpu_watch_address()&~(uintptr_t)7;uint64_t v=0;int has=w>=d&&w+8<=d+p->size;if(has)memcpy(&v,(const void*)w,8);
                LOG("[vulkan-watch] frame %llu write-back site %u dst %#llx+%llu seeded %d superseded %d: watched word %s%#llx",(unsigned long long)presents,site,(unsigned long long)d,(unsigned long long)p->size,seeded,superseded,has?"":"(not in it) ",(unsigned long long)v);} }
            if(p->source.buffer==p->output.buffer)memset(&p->output,0,sizeof p->output);
            buffer_destroy(&p->source);buffer_destroy(&p->output);
        }else if(p->kind==1){
            dm_guest_write_prepare((void*)p->event,4);if(p->mode)*p->event+=p->value;else *p->event=p->value;
        }else if(p->kind==2){
            uint64_t record[2];
            // Supported types share the device clock domain; samples modify only the publication copy and never become GPU-readable early.
            if(p->counter_timestamp_encoded)memcpy(record,p->output.mapped,sizeof record);
            else{uint64_t t=dm_android_clock_ns(CLOCK_MONOTONIC)/625*12;record[0]=record[1]=t;}
            if(p->value==NVN_COUNTER_TYPE_SAMPLES_PASSED)record[0]=samples_passed;
            else if(p->value!=NVN_COUNTER_TYPE_TIMESTAMP&&p->value!=NVN_COUNTER_TYPE_TIMESTAMP_TOP)die("Vulkan counter type %u is not implemented",p->value);
            /* TOTK: timestamp reports write counter 0 */
            else record[0]=0;
            if(p->value==NVN_COUNTER_TYPE_SAMPLES_PASSED){dm_guest_write_prepare(p->destination,sizeof record);memcpy(p->destination,record,sizeof record);cond_pending_done((uintptr_t)p->destination);}
            /* the counter word was written at replay (mtl_sub_counter) */
            else{dm_guest_write_prepare((uint8_t*)p->destination+8,8);memcpy((uint8_t*)p->destination+8,&record[1],8);}
            static unsigned report_diagnostics;
            if(p->counter_timestamp_encoded&&report_diagnostics<6){report_diagnostics++;
                LOG("[vulkan-counter-published] sample=%u type=%#x count=%llu timestamp=%llu domain=device GPU_forward=%u",report_diagnostics,p->value,
                    (unsigned long long)record[0],(unsigned long long)record[1],(unsigned)p->counter_gpu_encoded);}
            buffer_destroy(&p->output);
        }else if(p->kind==6){image_writeback_publish(p);buffer_destroy(&p->output);
        }else if(p->kind==4)samples_passed=0;
        else if(p->kind==5){if(p->value>=batch->query_count)die("Vulkan asynchronous query index");samples_passed+=query_counts[p->value];}
        else{
            if(p->buffer_view)vkDestroyBufferView(device,p->buffer_view,NULL);
            buffer_destroy(&p->source);buffer_destroy(&p->output);
            if(p->frame)vkDestroyFramebuffer(device,p->frame,NULL);
            if(p->pipeline)vkDestroyPipeline(device,p->pipeline,NULL);
            if(p->descriptors)vkDestroyDescriptorPool(device,p->descriptors,NULL);
            if(p->pipeline_layout)vkDestroyPipelineLayout(device,p->pipeline_layout,NULL);
            if(p->pass)vkDestroyRenderPass(device,p->pass,NULL);
            if(p->image_view)vkDestroyImageView(device,p->image_view,NULL);
        }
        gpu_watch_changed("post",p->kind|(uint64_t)p->site<<8);
        free(p);
    }
    batch->posts=batch->copies=NULL;batch->query_count=0;batch->host_sync=0;
    // Publish/retire resources first, then retire only this batch's ranges; other in-flight roots and consumer references remain intact.
    dm_pending_ranges_reset(&batch->pending_ranges);
    uint64_t elapsed=dm_android_clock_ns(CLOCK_MONOTONIC)-started;
    stability_worker_publish_ns+=elapsed;
}

static int uboprobe_on(void);static void uboprobe_batch_done(unsigned slot);
static void *async_completion_main(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(),"D2R GPU done");
    /* A/B knob (totk_affinity.h); every core by default */
    totk_pin_self("TOTK_GPUDONE_CPUS", "all");dm_android_perf_hint_thread();
    pthread_mutex_lock(&queue_lock);
    for(;;){
        while(!async_first&&!async_stopping){int result=pthread_cond_wait(&async_condition,&queue_lock);if(result)die("Vulkan asynchronous condition %d",result);}
        if(!async_first&&async_stopping)break;
        async_batch *batch=async_first;uint64_t ticket=batch->ticket;
        // The worker alone waits/retires; this fence cannot be reset before the slot becomes FREE.
        pthread_mutex_unlock(&queue_lock);
        VK_CHECK(vkWaitForFences(device,1,&batch->fence,VK_TRUE,UINT64_MAX));if(totk_tl)totk_tl(TL_FENCE,ticket,0);pace_fence_passed(ticket,dm_android_clock_ns(CLOCK_MONOTONIC));
        drawstat_batch_done((unsigned)(batch-async_batches));
        passtime_batch_done((unsigned)(batch-async_batches));
        pthread_mutex_lock(&queue_lock);
        if(async_first!=batch||batch->status!=ASYNC_PENDING||batch->ticket!=ticket)die("Vulkan asynchronous FIFO corruption");
        if(uboprobe_on())uboprobe_batch_done((unsigned)(batch-async_batches));
        async_publish_locked(batch);
        async_first=batch->next;if(!async_first)async_last=NULL;batch->next=NULL;batch->status=ASYNC_FREE;
        // Publish completed only after CPU writeback and retirement, preserving SyncWait/Event visibility.
        pthread_mutex_lock(&completion_lock);
        if(ticket!=atomic_load(&completed)+1)die("Vulkan asynchronous completion order");
        atomic_store(&completed,ticket);pthread_cond_broadcast(&completion_condition);if(totk_tl)totk_tl(TL_FENCE,ticket,1);
        pthread_mutex_unlock(&completion_lock);
        pthread_cond_broadcast(&async_condition);
    }
    pthread_mutex_unlock(&queue_lock);return NULL;
}

static void async_initialize_locked(void) {
    if(async_initialized)return;
    for(unsigned i=0;i<DM_VK_BATCH_SLOTS;i++){
        async_batch *batch=&async_batches[i];
        if(!i&&command&&gpu_fence&&draw_queries){batch->command=command;batch->fence=gpu_fence;batch->queries=draw_queries;batch->times=batch_times;}
        else{
            VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            VK_CHECK(vkAllocateCommandBuffers(device,&allocation,&batch->command));
            VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK_CHECK(vkCreateFence(device,&fence,NULL,&batch->fence));
            VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=ASYNC_QUERY_CAPACITY};
            VK_CHECK(vkCreateQueryPool(device,&query,NULL,&batch->queries));
            if(gpu_timestamp_bits&&gpu_timestamp_period>0){query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=2;VK_CHECK(vkCreateQueryPool(device,&query,NULL,&batch->times));}
        }
        // Read each slot's results only after its real fence; exclude persistent capacity from temporary snapshots.
        batch->query_readback=buffer_create(ASYNC_QUERY_READBACK_BYTES);immutable_buffer_bytes+=batch->query_readback.capacity;
        batch->chunks[0]=batch->command;batch->chunk_count=1;
    }
    async_stopping=0;async_initialized=1;
    int result=pthread_create(&async_thread,NULL,async_completion_main,NULL);if(result)die("Vulkan completion worker %d",result);
    LOG("[vulkan] asynchronous submission contexts=%u; completion publishes guest effects in FIFO order",DM_VK_BATCH_SLOTS);
}

// Host GetQueryPoolResults implicitly waits for device idle on LVP, even without WAIT.
// Copy queries at this batch's tail and make them host-visible so publication never waits for later batches.
static void async_record_query_readback_locked(void) {
    async_batch *batch=async_current;
    if(!recording||!batch||batch->status!=ASYNC_RECORDING||used_queries>ASYNC_QUERY_CAPACITY)die("Vulkan asynchronous query readback state");
    VkQueryResultFlags flags=VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT;
    if(batch->times)vkCmdCopyQueryPoolResults(command,batch->times,0,2,batch->query_readback.buffer,0,sizeof(uint64_t),flags);
    if(used_queries)vkCmdCopyQueryPoolResults(command,batch->queries,0,used_queries,batch->query_readback.buffer,ASYNC_TIMESTAMP_BYTES,sizeof(uint64_t),flags);
    if(!batch->times&&!used_queries)return;
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=batch->query_readback.buffer,.offset=0,.size=ASYNC_QUERY_READBACK_BYTES};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
}

static unsigned async_begin_locked(void) {
    async_initialize_locked();
    post0_publish_report();
    async_chunk_report();
    if(async_current||async_stopping)die("Vulkan asynchronous nested/stopping recording");
    for(;;){
        if(async_current||recording)die("Vulkan asynchronous recording claimed during a slot wait");
        for(unsigned i=0;i<DM_VK_BATCH_SLOTS;i++){
            unsigned slot=(async_next_slot+i)%DM_VK_BATCH_SLOTS;async_batch *batch=&async_batches[slot];
            if(batch->status!=ASYNC_FREE)continue;
            if(batch->pending_ranges.collections[0].count||batch->pending_ranges.collections[1].count||batch->pending_ranges.live_nodes)die("Vulkan pending index reused before retirement");
            batch->status=ASYNC_RECORDING;batch->serial=++write_chunk_serial;batch->has_known_cpu_copy=batch->known_copy_unbounded=0;batch->kc_count=0;batch->kc_unindexed=0;batch->kc_big_count=0;memset(batch->kc_head,0,sizeof batch->kc_head);batch->wi_count=0;batch->wi_unindexed=0;batch->wi_big_count=0;memset(batch->wi_head,0,sizeof batch->wi_head);batch->known_copy_low=batch->known_copy_high=0;
            batch->temporary_base=active_buffer_bytes-read_cache_bytes-immutable_buffer_bytes;
            async_current=batch;async_next_slot=(slot+1)%DM_VK_BATCH_SLOTS;
            batch->chunk_index=batch->chunk_draws=0;batch->command=batch->chunks[0];batch->chunk_started=0;
            batch->counter_written=batch->counter_used;batch->counter_used=0;
            command=batch->command;gpu_fence=batch->fence;draw_queries=batch->queries;batch_times=batch->times;
            return slot;
        }
        if(!async_first)die("Vulkan asynchronous slot corruption");
        uint64_t wait_start=dm_android_clock_ns(CLOCK_MONOTONIC);async_ring_stalls++;
        // Waiting releases queue_lock so the completed GPU's worker can publish results and free a slot.
        int result=pthread_cond_wait(&async_condition,&queue_lock);if(result)die("Vulkan asynchronous slot wait %d",result);
        uint64_t elapsed=dm_android_clock_ns(CLOCK_MONOTONIC)-wait_start;
        fence_wait_total_ns+=elapsed;stability_ring_wait_ns+=elapsed;
    }
}

// The caller has ended the render pass, written the end timestamp and ended the command buffer; queue_lock remains held on return.
static uint64_t async_commit_locked(void) {
    async_batch *batch=async_current;
    if(!batch||!recording||batch->status!=ASYNC_RECORDING)die("Vulkan asynchronous commit without begin");
    /* what each batch publishes to the guest, one frame in 300 */
    { static int diag=-1;if(diag<0){const char *x=getenv("TOTK_VK_BATCH_DIAG");diag=x&&atoi(x);}
      if(diag&&presents%300==100){unsigned k[6]={0};uint64_t wb=0;for(const post_op *p=post_first;p;p=p->next){if(p->kind<6)k[p->kind]++;if(p->kind==0)wb+=p->size;}
          LOG("[vulkan-batch-diag] frame %llu draws %llu screen %d: write-backs %u (%llu B) events %u counters %u sample-resets %u query-adds %u releases %u",
              (unsigned long long)presents,(unsigned long long)batch_draws,screen_submission,k[0],(unsigned long long)wb,k[1],k[2],k[4],k[5],k[3]);} }
    VK_CHECK(vkResetFences(device,1,&batch->fence));
    VkCommandBuffer list[2];VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=prelude_submit_list(list,batch->command),.pCommandBuffers=list};
    VkPipelineStageFlags present_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    if(screen_submission){submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&screen_acquired;submit.pWaitDstStageMask=&present_stage;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&screen_rendered;}
    ucap_wait_current();uint64_t ticket=atomic_fetch_add(&submitted,1)+1;VK_CHECK(vkQueueSubmit(queue,1,&submit,batch->fence));if(totk_tl)totk_tl(TL_COMMIT,ticket,1);
    async_logical_submits++;
    batch->posts=post_first;batch->copies=post_copy_first;batch->host_sync=pending_host_sync;batch->query_count=used_queries;batch->queries_to_reset=used_queries;batch->ticket=ticket;batch->status=ASYNC_PENDING;batch->next=NULL;
    if(async_last)async_last->next=batch;else async_first=batch;async_last=batch;
    post_first=post_copy_first=NULL;post_tail=&post_first;post_copy_tail=&post_copy_first;pending_host_sync=0;
    recording=0;screen_submission=0;async_current=NULL;async_recording_owned=0;
    pthread_cond_signal(&async_condition);return ticket;
}

