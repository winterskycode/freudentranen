// Persistent graphics objects and same-attachment passes; attachment reads, copies and writeback boundaries close the pass in the executor.
static uint64_t graphics_hash(const void *data,size_t size) {
    // Select only process-local buckets; load every byte safely and retain callers' full key comparisons to reject collisions.
    const uint8_t *bytes=data;const uint64_t multiplier=0xc6a4a7935bd1e995ull;
    uint64_t hash=14695981039346656037ull^((uint64_t)size*multiplier);
    while(size>=8){uint64_t word;memcpy(&word,bytes,sizeof word);
        word*=multiplier;word^=word>>47;word*=multiplier;
        hash^=word;hash*=multiplier;bytes+=8;size-=8;
    }
    if(size){uint64_t tail=0;memcpy(&tail,bytes,size);hash^=tail;hash*=multiplier;}
    hash^=hash>>47;hash*=multiplier;hash^=hash>>47;
    return hash;
}
typedef struct graphics_layout_entry {
    shader_entry *vs,*fs;VkPipelineLayout layout;struct graphics_layout_entry *next;
} graphics_layout_entry;
static graphics_layout_entry *graphics_layouts[1024];
static uint64_t graphics_layout_creations,graphics_pass_creations,graphics_frame_creations;
static VkPipelineLayout graphics_layout_get_locked(shader_entry *vs,shader_entry *fs,unsigned bucket);
static VkDescriptorSetLayout bl_layout;static VkDescriptorSet bl_set;
/* Per thread, the layouts last found by bucket (layouts and the shaders they are keyed by are never freed), so that a
   draw need not take graphics_object_lock for this lookup. */
static VkPipelineLayout graphics_layout_get(shader_entry *vs,shader_entry *fs) {
    uintptr_t key[2]={(uintptr_t)vs,(uintptr_t)fs};unsigned bucket=graphics_hash(key,sizeof key)&1023;
    static __thread struct { const shader_entry *vs,*fs; VkPipelineLayout layout; } memo[256];
    unsigned m=bucket&255;if(memo[m].vs==vs&&memo[m].fs==fs&&memo[m].layout)return memo[m].layout;
    VkPipelineLayout layout=graphics_layout_get_locked(vs,fs,bucket);memo[m].vs=vs;memo[m].fs=fs;memo[m].layout=layout;return layout;
}
static VkPipelineLayout graphics_layout_get_locked(shader_entry *vs,shader_entry *fs,unsigned bucket) {
    pthread_mutex_lock(&graphics_object_lock);
    for(graphics_layout_entry *entry=graphics_layouts[bucket];entry;entry=entry->next)if(entry->vs==vs&&entry->fs==fs){VkPipelineLayout layout=entry->layout;pthread_mutex_unlock(&graphics_object_lock);return layout;}
    graphics_layout_entry *entry=calloc(1,sizeof *entry);if(!entry)die("Vulkan graphics layout cache allocation");entry->vs=vs;entry->fs=fs;
    /* TOTK_VK_BINDLESS: the global texture arrays are set 0 of every layout (nvn_vulkan_bindless.inc) */
    VkDescriptorSetLayout layouts[3]={bl_layout,vs->layout,fs->layout};int bl=!!bl_set;
    /* the flag: whole and library-linked pipelines bind the same sets (nvn_vulkan_gpl.inc) */
    VkPipelineLayoutCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.flags=gpl_state>0&&!bl&&!(getenv("TOTK_VK_GPL_FULL")&&atoi(getenv("TOTK_VK_GPL_FULL")))?VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT:0,.setLayoutCount=bl?3:2,.pSetLayouts=layouts+!bl};
    VK_CHECK(vkCreatePipelineLayout(device,&info,NULL,&entry->layout));entry->next=graphics_layouts[bucket];graphics_layouts[bucket]=entry;graphics_layout_creations++;VkPipelineLayout layout=entry->layout;pthread_mutex_unlock(&graphics_object_lock);return layout;
}
typedef struct {
    uint32_t attachments,colors,depth,color_indices[8];VkFormat formats[9];uint32_t load[9],stencil_load[9];
} graphics_pass_key;
typedef struct graphics_pass_entry { graphics_pass_key key;VkRenderPass pass;struct graphics_pass_entry *next; } graphics_pass_entry;
static graphics_pass_entry *graphics_passes[256];
typedef struct {
    graphics_pass_entry *pass;VkImageView views[9];uint32_t width,height;
} graphics_frame_key;
typedef struct graphics_frame_entry {
    graphics_frame_key key;image_entry *images[9];VkFramebuffer frame;struct graphics_frame_entry *next;
} graphics_frame_entry;
static graphics_frame_entry *graphics_frames[1024],*graphics_active_frame;
static void fg_pass_begin(graphics_frame_entry *frame);
static uint32_t graphics_active_query;
static graphics_pass_key graphics_pass_key_make(unsigned count,unsigned ncolors,const VkAttachmentDescription *attachments,const VkAttachmentReference *colors,VkAttachmentReference depth) {
    graphics_pass_key key={.attachments=count,.colors=ncolors,.depth=depth.attachment};
    for(unsigned i=0;i<count;i++){key.formats[i]=attachments[i].format;key.load[i]=attachments[i].loadOp;key.stencil_load[i]=attachments[i].stencilLoadOp;}
    for(unsigned i=0;i<ncolors;i++)key.color_indices[i]=colors[i].attachment;return key;
}
static graphics_pass_entry *graphics_pass_get(graphics_pass_key key,const VkAttachmentDescription *attachments,const VkAttachmentReference *colors,VkAttachmentReference depth,int fatal) {
    /* every draw asks (draw_graphics_inner); consecutive draws mostly share the pass, and entries are never freed */
    static __thread graphics_pass_entry *last;if(last&&!memcmp(&last->key,&key,sizeof key))return last;
    unsigned bucket=graphics_hash(&key,sizeof key)&255;
    pthread_mutex_lock(&graphics_object_lock);
    for(graphics_pass_entry *entry=graphics_passes[bucket];entry;entry=entry->next)if(!memcmp(&entry->key,&key,sizeof key)){pthread_mutex_unlock(&graphics_object_lock);last=entry;return entry;}
    graphics_pass_entry *entry=calloc(1,sizeof *entry);if(!entry)die("Vulkan render pass cache allocation");entry->key=key;
    VkSubpassDescription sub={.flags=rorder_ok?VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT:0,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=key.colors,.pColorAttachments=colors,.pDepthStencilAttachment=depth.attachment==VK_ATTACHMENT_UNUSED?NULL:&depth};
    VkRenderPassCreateInfo info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=key.attachments,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub};
    /* the self-dependency an in-pass feedback barrier needs (draw_graphics_inner); not part of pass compatibility */
    VkSubpassDependency self={.srcSubpass=0,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT};
    if(fbloop_ok&&key.colors){info.dependencyCount=1;info.pDependencies=&self;}
    // The driver may reject this pass for an illegal format or combination. At runtime (fatal=1) the pass comes from
    // real textures, so a rejection is a genuine bug and still exits loudly. On the warm path (fatal=0) it comes from
    // a manifest whose records may be damaged or written on another machine, and a rejection only means "this record
    // must not be warmed" -- drop it. Same convention as graphics_pipeline_create, not a second one.
    VkResult created=vkCreateRenderPass(device,&info,NULL,&entry->pass);
    if(created!=VK_SUCCESS){free(entry);pthread_mutex_unlock(&graphics_object_lock);if(!fatal)return NULL;die("Vulkan render pass create failed: %d",(int)created);}
    entry->next=graphics_passes[bucket];graphics_passes[bucket]=entry;graphics_pass_creations++;pthread_mutex_unlock(&graphics_object_lock);return entry;
}
static graphics_frame_entry *graphics_frame_get(graphics_pass_entry *pass,const VkImageView *views,image_entry *const *images,uint32_t width,uint32_t height) {
    graphics_frame_key key={.pass=pass,.width=width,.height=height};memcpy(key.views,views,pass->key.attachments*sizeof *views);unsigned bucket=graphics_hash(&key,sizeof key)&1023;
    for(graphics_frame_entry *entry=graphics_frames[bucket];entry;entry=entry->next)if(!memcmp(&entry->key,&key,sizeof key))return entry;
    graphics_frame_entry *entry=calloc(1,sizeof *entry);if(!entry)die("Vulkan framebuffer cache allocation");entry->key=key;memcpy(entry->images,images,pass->key.attachments*sizeof *images);
    VkFramebufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass->pass,.attachmentCount=pass->key.attachments,.pAttachments=views,.width=width,.height=height,.layers=1};
    uint64_t fb_t0=dm_android_clock_ns(CLOCK_MONOTONIC);VK_CHECK(vkCreateFramebuffer(device,&info,NULL,&entry->frame));arrival_fb_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-fb_t0;arrival_fbs++;entry->next=graphics_frames[bucket];graphics_frames[bucket]=entry;graphics_frame_creations++;return entry;
}
static int graphics_image_is_attachment(const image_entry *image) {
    if(!graphics_active_frame)return 0;
    for(unsigned i=0;i<graphics_active_frame->key.pass->key.attachments;i++)if(graphics_active_frame->images[i]==image)return 1;
    return 0;
}
static int graphics_pass_matches(const graphics_pass_key *key,const VkImageView *views,uint32_t width,uint32_t height) {
    if(!graphics_active_frame)return 0;const graphics_frame_key *frame=&graphics_active_frame->key;const graphics_pass_key *active=&frame->pass->key;
    // LOAD applies only at pass begin; an active pass may retain draws following its first DONT_CARE load.
    return frame->width==width&&frame->height==height&&active->attachments==key->attachments&&active->colors==key->colors&&active->depth==key->depth
        &&!memcmp(active->color_indices,key->color_indices,sizeof key->color_indices)&&!memcmp(active->formats,key->formats,sizeof key->formats)
        &&!memcmp(frame->views,views,key->attachments*sizeof *views);
}
/* pass-end histogram (TOTK_VK_PASSEND=1): who ends render passes, by caller address, logged every 300 frames. "restart"
   counts the ends after which the next pass opens the same framebuffer again: those breaks are ours to remove, while an
   end followed by another framebuffer costs nothing the game's own target switch would not. */
static struct { void *pc; uint64_t n, restarts; } passend_hist[64];
static void *passend_last_caller; static const void *passend_last_frame;
static int passend_on(void) { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_PASSEND");on=x&&atoi(x);}return on; }
static void passend_note(void *caller,const void *frame) {
    if(!passend_on())return;
    static uint64_t last_frame;
    for(int i=0;i<64;i++){if(passend_hist[i].pc==caller){passend_hist[i].n++;break;}if(!passend_hist[i].pc){passend_hist[i].pc=caller;passend_hist[i].n=1;break;}}
    passend_last_caller=caller;passend_last_frame=frame;
    if(presents>=last_frame+300){last_frame=presents;Dl_info info;dladdr((void*)passend_note,&info);
        for(int i=0;i<64&&passend_hist[i].pc;i++)LOG("[vulkan-passend] frame %llu caller +%#lx count %llu restarts %llu",(unsigned long long)presents,(unsigned long)((uintptr_t)passend_hist[i].pc-(uintptr_t)info.dli_fbase),(unsigned long long)passend_hist[i].n,(unsigned long long)passend_hist[i].restarts);
        memset(passend_hist,0,sizeof passend_hist);}
}
static void passend_begin_note(const void *frame) {
    if(!passend_on()||!passend_last_caller)return;
    if(frame==passend_last_frame)for(int i=0;i<64&&passend_hist[i].pc;i++)if(passend_hist[i].pc==passend_last_caller){passend_hist[i].restarts++;break;}
    passend_last_caller=NULL;
}
/* Occlusion queries feed the game's samples-passed counters (19 reports a frame against ~180 passes a frame in the cave).
   On Turnip a query end is a ZPASS_DONE event and a wait-for-idle. TOTK_VK_PASS_QUERIES: 0 none (measurement: counts read
   0; also TOTK_VK_EXP=noquery), 1 every pass, 2 (default) only while a count is live: from a samples
   counter reset to the next report. A report with no reset since the last one ("late": its count would miss the draws in
   between) switches to every pass for the rest of the run, logged once. In the cave: 180 queries a frame -> 19, frame
   11.17 -> 10.92 ms (GPU 9.54 -> 9.32), the same as with no queries at all; no late report there. */
static int pass_queries_mode=-1;static int samples_live=1;static uint64_t samples_late,pass_queries_opened;
/* an occlusion query is begun on graphics_active_query */
static int graphics_query_open;
static int pass_queries_init(void) { if(pass_queries_mode<0){const char *e=getenv("TOTK_VK_PASS_QUERIES");pass_queries_mode=e?atoi(e):2;if(exp_env("noquery"))pass_queries_mode=0;} return pass_queries_mode; }
static int pass_queries_on(void) { int m=pass_queries_init(); return m==1||(m==2&&samples_live); }
static void query_close(void) { if(!graphics_query_open)return;vkCmdEndQuery(command,draw_queries,graphics_active_query);post_new(5)->value=graphics_active_query;graphics_query_open=0; }
/* a query index is taken only when a query is begun: the batch's readback copies [0, used_queries) with a wait, and a
   reserved index never begun would hang the GPU */
static void query_open(void) { if(graphics_query_open||!graphics_active_frame||!pass_queries_on())return;
    if(used_queries>=1024)die("Vulkan occlusion batch capacity");graphics_active_query=used_queries++;
    vkCmdBeginQuery(command,draw_queries,graphics_active_query,gpu_features.occlusionQueryPrecise?VK_QUERY_CONTROL_PRECISE_BIT:0);graphics_query_open=1;pass_queries_opened++; }
static int graphics_pass_open(void) { return graphics_active_frame != NULL; }
static void graphics_pass_dims(uint32_t *w,uint32_t *h,unsigned *att) { const graphics_frame_entry *f=graphics_active_frame;*w=f?f->key.width:0;*h=f?f->key.height:0;*att=f?f->key.pass->key.attachments:0; }
static void mo_flush(graphics_frame_entry *ended);
static void graphics_pass_end(void) {
    if(!graphics_active_frame)return;
    passend_note(__builtin_return_address(0),graphics_active_frame);
    // Consecutive draws share an occlusion query; reset/report boundaries close the pass first to preserve counter order.
    PASSLOG("  pass end: %u draws",passlog_draws);passlog_draws=0;
    query_close();
    graphics_frame_entry *ended=graphics_active_frame;
    vkCmdEndRenderPass(command);graphics_active_frame=NULL;
    /* object motion: this pass's listed draws again, while their sets and buffers are this batch's */
    mo_flush(ended);
    if(passtime_state>0)passtime_pass_end((unsigned)(async_current-async_batches),command,passtime_draws);passtime_draws=0;
    /* timestamp reports recorded inside the pass */
    counter_gpu_flush();
    /* experiment: full barrier after every pass */
    { static int full=-1;if(full<0)full=exp_env("barrier");
      /* live "passlog@A-B": one frame's passes (the first after the word was set) */
      { Dl_info dli;void *ra;
        if(live_range("passlog",draw_count-live_frame_base)&&*exp_word(live_exp,"passlog",0)=='@'&&(!live_log_frame||live_log_frame==live_frame_base)){live_log_frame=live_frame_base;
            const graphics_pass_key *k=&ended->key.pass->key;const image_entry *c0=k->colors&&k->color_indices[0]<k->attachments?ended->images[k->color_indices[0]]:NULL;
            const image_entry *z=k->depth!=VK_ATTACHMENT_UNUSED&&k->depth<k->attachments?ended->images[k->depth]:NULL;ra=__builtin_return_address(0);
            LOG("[exp] pass ends at frame draw %llu: %ux%u c0 %llu depth %llu, first fs %s, ended by +%#lx",(unsigned long long)(draw_count-live_frame_base),ended->key.width,ended->key.height,
                c0?(unsigned long long)c0->serial:0ull,z?(unsigned long long)z->serial:0ull,passtime_fs?passtime_fs:"-",dladdr(ra,&dli)&&dli.dli_fbase?(unsigned long)((uintptr_t)ra-(uintptr_t)dli.dli_fbase):0ul);} }
      if(inplace_pass_writes){inplace_pass_writes=0;inplace_pass_barriers++;VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
          /* in place: storage written by this pass's draws, read in place by what follows */
          vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);}
      if(full||live_range("barrier",draw_count-live_frame_base)){VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
          vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);} }
    block_writeback_flush();
    /* experiment: the pass log's attachment reads, without copies */
    { static int attread=-1;if(attread<0)attread=exp_env("attread");
      if(attread&&!passlog_on())for(unsigned i=0;i<ended->key.pass->key.attachments;i++){image_entry *ae=ended->images[i];if(ae->aspect==VK_IMAGE_ASPECT_COLOR_BIT)image_access(ae,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);} }
    if(passlog_on()&&ended->key.pass->key.attachments){passlog_sample(ended->images[0],ended->key.width/2,ended->key.height/2);
        if(ended->key.width>=768)for(unsigned i=0;i<ended->key.pass->key.attachments;i++)passlog_picture_take(ended->images[i],i);
        passlog_pass++;}
}
/* Inside a pass: close the occlusion query so far (its samples post keeps the counting order) and continue with a new
   one, instead of ending the render pass for a samples report or counter reset. */
static void graphics_query_split(void) {
    if(!graphics_active_frame)return;
    /* experiment: a samples report or counter reset ends the pass */
    if(live_exp_has("qpass")){graphics_pass_end();return;}
    query_close();query_open();
}
/* the game resets its samples counter: a count is live from here */
static void samples_reset(void) { samples_live=1;graphics_query_split(); }
/* the game reports its samples counter: the count so far is closed in order; with live-only queries the counter is dead
   until the next reset */
static void samples_report(void) {
    int m=pass_queries_init();
    if(m==2&&!samples_live){if(!samples_late++)LOG("[vulkan-query] a samples report with no counter reset since the last report: occlusion queries on every pass from now on");pass_queries_mode=1;}
    query_close();if(pass_queries_mode==2)samples_live=0;
    query_open();
}
static void pass_queries_report(void) {
    static uint64_t frame,passes,opened;
    if(presents<frame+300)return;
    if(frame)LOG("[vulkan-query] a frame: %.0f render passes, %.1f occlusion queries (mode %d), late reports %llu",(render_passes-passes)/300.0,(pass_queries_opened-opened)/300.0,pass_queries_mode,(unsigned long long)samples_late);
    frame=presents;passes=render_passes;opened=pass_queries_opened;
}
static void graphics_pass_begin(graphics_frame_entry *frame) {
    if(graphics_active_frame==frame)return;graphics_pass_end();passend_begin_note(frame);
    VkRenderPassBeginInfo info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=frame->key.pass->pass,.framebuffer=frame->frame,.renderArea={{0,0},{frame->key.width,frame->key.height}}};
    /* deferred clears as CLEAR load ops (draw_graphics_inner) */
    for(unsigned i=0;i<frame->key.pass->key.attachments;i++)
        if(frame->key.pass->key.load[i]==VK_ATTACHMENT_LOAD_OP_CLEAR||frame->key.pass->key.stencil_load[i]==VK_ATTACHMENT_LOAD_OP_CLEAR){info.clearValueCount=frame->key.pass->key.attachments;info.pClearValues=graphics_clear_values;}
    if(passlog_on())for(unsigned i=0;i<frame->key.pass->key.attachments;i++){const image_entry *e=frame->images[i];
        int layer=-1,level=-1;for(const cached_image_view *cv=e->views;cv;cv=cv->next)if(cv->view==frame->key.views[i]){layer=(int)cv->range.baseArrayLayer;level=(int)cv->range.baseMipLevel;}
        PASSLOG("  frame %llu attachment %u layer %d level %d",(unsigned long long)presents,i,layer,level);
        PASSLOG("pass %ux%u attachment %u: serial %llu %ux%u fmt %#x load %u lv %u target %d mem %p size %llu",frame->key.width,frame->key.height,i,(unsigned long long)e->serial,e->texture.d.width,e->texture.d.height,e->texture.d.format,frame->key.pass->key.load[i],e->texture.d.levels,e->texture.d.target,(void*)e->texture.mem,(unsigned long long)e->texture.L.size);}
    /* dynamic resolution watch: the G-buffer pass (4+ attachments) size per frame, logged when it changes */
    if(frame->key.pass->key.attachments>=4){static uint32_t last_w,last_h;static uint64_t changes;
        if(frame->key.width!=last_w||frame->key.height!=last_h){changes++;if(changes<=40||!(changes%100))LOG("[vulkan-dynres] frame %llu: G-buffer pass %ux%u (was %ux%u), change %llu",(unsigned long long)presents,frame->key.width,frame->key.height,last_w,last_h,(unsigned long long)changes);last_w=frame->key.width;last_h=frame->key.height;}}
    if(passtime_state>0)passtime_pass_begin((unsigned)(async_current-async_batches),command,(passtime_key){frame->key.width,frame->key.height,frame->key.pass->key.attachments,frame->key.pass->key.attachments?(uint32_t)frame->key.pass->key.formats[0]:0,0});
    fg_pass_begin(frame);
    vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);graphics_active_frame=frame;render_passes++;
    query_open();pass_queries_report();
}
static void graphics_cache_forget_image(image_entry *image) {
    // The executor has waited for GPU image use; destroy framebuffers before their referenced views.
    if(graphics_image_is_attachment(image))graphics_pass_end();
    for(unsigned bucket=0;bucket<1024;bucket++){
        graphics_frame_entry **link=&graphics_frames[bucket];
        while(*link){graphics_frame_entry *entry=*link;int contains=0;
            for(unsigned i=0;i<entry->key.pass->key.attachments;i++)contains|=entry->images[i]==image;
            if(contains){*link=entry->next;vkDestroyFramebuffer(device,entry->frame,NULL);free(entry);}else link=&entry->next;
        }
    }
}
static int graphics_shader_writes(const shader_entry *shader) {
    for(unsigned i=0;i<shader->count;i++){const shader_binding *binding=&shader->bindings[i];if((binding->kind==2&&(binding->flags&1))||(binding->kind==4&&(binding->flags&4)))return 1;}
    return 0;
}
