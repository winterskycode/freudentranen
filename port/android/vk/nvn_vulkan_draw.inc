static void dynres_note(float w, float h, uint32_t target_w, uint32_t target_h);
// Graphics pipelines and draws, separated from descriptor/compute binding for independent optimization.
static VkBlendFactor blend_factor(unsigned factor) {
    switch(factor){case 1:return VK_BLEND_FACTOR_ZERO;case 2:return VK_BLEND_FACTOR_ONE;case 3:return VK_BLEND_FACTOR_SRC_COLOR;case 4:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;case 5:return VK_BLEND_FACTOR_SRC_ALPHA;case 6:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_DST_ALPHA;case 8:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;case 9:return VK_BLEND_FACTOR_DST_COLOR;case 10:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;case 11:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;case 0x61:return VK_BLEND_FACTOR_CONSTANT_COLOR;case 0x62:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;case 0x63:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case 0x64:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;default:return VK_BLEND_FACTOR_ONE;}
}
static VkBlendOp blend_operation(unsigned op){return op>=1&&op<=5?op-1:VK_BLEND_OP_ADD;}
/* NVN quads (7) and quad strips (8; TOTK's Hero's Path route on the map) have no Vulkan topology: they are drawn as triangle
   lists from converted indices, keeping the winding and with the quad's last vertex v3 first in both triangles (NVN takes
   flat inputs from it, Vulkan from a triangle's first vertex). Quad v0 v1 v2 v3: (v3,v0,v1) (v3,v1,v2). Strip pair v0 v1 /
   v2 v3 (around: v0 v1 v3 v2): (v3,v0,v1) (v3,v2,v0), MacNativeGames nvn_quad_strip.h's (v0,v1,v3) (v2,v0,v3) rotated.
   Indexed input splits at the type's restart index; an incomplete quad or pair draws nothing. Returns the indices written. */
static int quad_primitive(unsigned primitive){return primitive==7||primitive==8;}
static uint32_t quad_index_bound(unsigned primitive,uint32_t count){return primitive==7?count/4*6:count>=4?(count/2-1)*6:0;}
static uint32_t quad_indices(uint32_t *out,unsigned primitive,const void *input,unsigned width,uint32_t count){
    const uint32_t restart=width==1?UINT8_MAX:width==2?UINT16_MAX:UINT32_MAX;uint32_t v[4],used=0,n=0;
    for(uint32_t i=0;i<count;i++){uint32_t x=i;if(input){x=0;memcpy(&x,(const uint8_t*)input+(size_t)i*width,width);if(x==restart){used=0;continue;}}
        v[used++]=x;if(used<4)continue;
        out[n++]=v[3];out[n++]=v[0];out[n++]=v[1];out[n++]=v[3];
        if(primitive==7){out[n++]=v[1];out[n++]=v[2];used=0;}else{out[n++]=v[2];out[n++]=v[0];v[0]=v[2];v[1]=v[3];used=2;}}
    return n;
}
static VkPrimitiveTopology topology(unsigned primitive){switch(primitive){case 0:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 1:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case 6:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;case 7:case 8:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; default:die("Vulkan primitive %u pending",primitive);return 0;}}
/* the attachment's format: its view's, else its texture object's (storage-shared images are mutable) */
static VkFormat attachment_format(image_entry *e,const tex_t *t,const view_t *v) {
    VkFormat f=vk_format(v&&v->set_format?v->format:t->d.format);return e->mutable_format&&f?f:e->format;
}
static VkImageView attachment_view(image_entry *e,const view_t *v,VkFormat format) {
    return image_view_formatted(e,VK_IMAGE_VIEW_TYPE_2D,texture_range(e,v),(VkComponentMapping){0},format)->view;
}
#include "nvn_vulkan_pass.inc"
static int smaa_scene(void);
static int asr_upscale_draw(const shader_entry *fs);
static int taa_enabled(void);static void taa_compose(const shader_entry *fs);static void taa_before_draw(const shader_entry *fs);
static void taa_viewport(const VkViewport *viewport,const image_entry *depth,unsigned colors,const tex_t *c0,const shader_entry *vs,const shader_entry *fs);
static void fg_before_draw(const shader_entry *vs);static void fg_trace_draw(const shader_entry *vs,const shader_entry *fs);static void fg_after_pass(void);

// Reuse pipelines by actual fixed state; viewport, scissor, blend constants and stencil masks/reference are dynamic.
typedef struct {
    shader_entry *vs,*fs; VkFormat formats[9]; uint32_t colors,primitive,attachments,depth_attachment,color_indices[8],required_subgroup_size;
    va_st attributes[16]; vs_st streams[16]; color_st color; chmask_st mask;
    blend_st blend[8]; poly_st polygon; ds_st depth; ms_st multisample;
    uint8_t depth_clamp, raster_discard, pad_[2];
} pipeline_key;
static int mo_on(void);static void mo_record(shader_entry *vs,const pipeline_key *key,ds_st depth,poly_st polygon,const VkBuffer *vb,const VkDeviceSize *vo,VkBuffer ib,VkDeviceSize io,VkIndexType it,uint32_t count,const p_draw_idx *indexed,const VkViewport *viewport,const VkRect2D *scissor);
/* 1 built, 0 building in the background, -1 failed */
typedef struct pipeline_entry { pipeline_key key; VkPipeline pipeline; struct pipeline_entry *next; _Atomic int ready; } pipeline_entry;
/* part 1: the draw plan */
#include "nvn_vulkan_plan.inc"
static pipeline_entry *graphics_pipelines[2048];
/* TOTK_VK_PIPEMISS=1: for each new graphics pipeline, the key fields that differ from the closest existing pipeline
   (fewest differing bytes), counted per field and logged every 2000 misses: finds state that churns pipelines. */
static void pipeline_miss_explain(const pipeline_key *k) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PIPEMISS"); on = e && atoi(e); } if (!on) return;
    static const struct { const char *name; size_t off, size; } f[] = {
#define F(m) {#m, offsetof(pipeline_key, m), sizeof(((pipeline_key *)0)->m)}
        F(vs), F(fs), F(formats), F(colors), F(primitive), F(attachments), F(depth_attachment), F(color_indices), F(required_subgroup_size),
        F(attributes), F(streams), F(color), F(mask), F(blend), F(polygon), F(depth), F(multisample), F(depth_clamp), F(raster_discard), F(pad_)
#undef F
    };
    enum { NF = sizeof f / sizeof *f };
    static uint64_t counts[NF + 1], misses;
    const pipeline_entry *best = NULL; unsigned best_bytes = UINT32_MAX;
    for (unsigned b = 0; b < 2048; b++) for (const pipeline_entry *e = graphics_pipelines[b]; e; e = e->next) {
        const uint8_t *x = (const uint8_t *)&e->key, *y = (const uint8_t *)k; unsigned d = 0;
        for (size_t i = 0; i < sizeof *k; i++) d += x[i] != y[i];
        if (d < best_bytes) { best_bytes = d; best = e; }
    }
    misses++;
    char line[512]; int n = 0;
    if (!best) counts[NF]++;
    else for (unsigned i = 0; i < NF; i++) if (memcmp((const uint8_t *)&best->key + f[i].off, (const uint8_t *)k + f[i].off, f[i].size)) {
        counts[i]++; if (misses <= 40 && n < (int)sizeof line - 40) n += snprintf(line + n, sizeof line - n, " %s", f[i].name); }
    if (misses <= 40 && best) LOG("[pipemiss] %llu: differs from the closest pipeline in%s", (unsigned long long)misses, line);
    if (!(misses % 2000)) { n = 0; for (unsigned i = 0; i < NF; i++) if (counts[i]) n += snprintf(line + n, sizeof line - n, " %s=%llu", f[i].name, (unsigned long long)counts[i]);
        LOG("[pipemiss] %llu misses; field differs from the closest:%s; first of its kind %llu", (unsigned long long)misses, line, (unsigned long long)counts[NF]); }
}
/* Shader bisection (debug): <files>/magenta.txt holds "n:k" (fragment shaders whose key value mod n == k) or a comma list of
   keys; those draw solid magenta (full-screen passes excepted). Read about once a second; a change bumps magenta_gen, which is
   part of the pipeline key, so every pipeline is rebuilt with the new filter (the old ones are kept: debug only). */
static unsigned long long magenta_n,magenta_k;static char magenta_list[16384];static uint8_t magenta_gen;
/* <files>/depthall.txt: these fragment shaders' draws test ALWAYS and write depth (read back the depth a draw
   produces) */
static char depthall_list[4096];
/* Live shader replacement (debug): <files>/override/<key>.spv replaces that shader's SPIR-V while the game runs (with
   freeze-frame: edit a shader and see the same frame again in a second). Polled about twice a second; a new, changed or
   deleted file bumps override_gen (part of the pipeline key) so pipelines are rebuilt; deleting restores the original. */
typedef struct shader_override { shader_entry *s; VkShaderModule orig_module; uint32_t *orig_spirv; size_t orig_words; struct linked_module *orig_linked; int64_t mtime; int seen; struct shader_override *next; } shader_override;
static shader_override *shader_overrides; static uint8_t override_gen;
static void shader_override_poll(void){
    static uint64_t next;uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);if(now<next)return;next=now+500000000ull;
    const char *files=getenv("TOTK_FILES_DIR");if(!files)return;char dir[600];snprintf(dir,sizeof dir,"%s/override",files);
    for(shader_override *o=shader_overrides;o;o=o->next)o->seen=0;
    int changed=0;DIR *d=opendir(dir);
    if(d){struct dirent *e;while((e=readdir(d))){size_t n=strlen(e->d_name);if(n!=20||strcmp(e->d_name+16,".spv"))continue;
        char key[17];memcpy(key,e->d_name,16);key[16]=0;char path[700];snprintf(path,sizeof path,"%s/%s",dir,e->d_name);struct stat st;if(stat(path,&st))continue;
        shader_entry *s=NULL;for(shader_entry *x=shader_list;x;x=x->next)if(!strcmp(x->key,key)){s=x;break;}
        /* not loaded (yet): tried again next time */
        if(!s)continue;
        shader_override *o=shader_overrides;while(o&&o->s!=s)o=o->next;
        if(o){o->seen=1;if(o->mtime==(int64_t)st.st_mtime)continue;}
        FILE *f=fopen(path,"rb");if(!f)continue;uint32_t *code=malloc((size_t)st.st_size+4);size_t bytes=code?fread(code,1,(size_t)st.st_size,f):0;fclose(f);
        if(!code||bytes<20||bytes%4||code[0]!=0x07230203){LOG("[override] %s: not SPIR-V (%zu bytes)",path,bytes);free(code);continue;}
        if(gpl_state>0&&s->stage==1)spirv_own_set(code,bytes/4,s->set);
        VkShaderModule module;VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};
        if(vkCreateShaderModule(device,&mi,&spv_callbacks,&module)){LOG("[override] %s: module creation failed",path);free(code);continue;}
        if(!o){o=calloc(1,sizeof *o);if(!o)die("shader override");o->s=s;o->orig_module=s->module;o->orig_spirv=s->spirv;o->orig_words=s->spirv_words;o->orig_linked=s->linked;o->next=shader_overrides;shader_overrides=o;}
        o->seen=1;o->mtime=(int64_t)st.st_mtime;s->module=module;
        if(s->stage==0){s->spirv=code;s->spirv_words=bytes/4;s->linked=NULL;}else free(code);
        s->compute_pipeline=VK_NULL_HANDLE;changed=1;LOG("[override] %s: replaced (%zu bytes)",key,bytes);}
      closedir(d);}
    for(shader_override **link=&shader_overrides;*link;){shader_override *o=*link;if(o->seen){link=&o->next;continue;}
        shader_entry *s=o->s;s->module=o->orig_module;s->spirv=o->orig_spirv;s->spirv_words=o->orig_words;s->linked=o->orig_linked;s->compute_pipeline=VK_NULL_HANDLE;
        LOG("[override] %s: original restored",s->key);*link=o->next;free(o);changed=1;}
    /* 0 while none is active: pipelines with an override are built on this thread and stay out of the warm-up list, so
       a forgotten override keeps every new pipeline off the background builders; the sequence keeps a re-applied
       override from matching pipelines built with an earlier one */
    static uint8_t seq;if(changed){if(!++seq)seq=1;override_gen=shader_overrides?seq:0;}
}
/* a magenta or depthall change: a new generation while either is on (pipelines rebuilt, built on this thread), 0 when
   both are off (the game's own pipelines again, background building again) */
static void magenta_bump(void){
    static uint8_t seq;if(!++seq)seq=1;magenta_gen=magenta_list[0]||magenta_n||depthall_list[0]?seq:0;
}
static void magenta_poll(void){
    static uint64_t next;static char last[sizeof magenta_list];uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC);if(now<next)return;next=now+1000000000ull;
    static char path[600];if(!path[0]){const char *d=getenv("TOTK_FILES_DIR");if(!d)return;snprintf(path,sizeof path,"%s/magenta.txt",d);}
    char text[sizeof magenta_list]={0};FILE *f=fopen(path,"r");if(f){size_t n=fread(text,1,sizeof text-1,f);text[n]=0;fclose(f);char *e=text+strlen(text);while(e>text&&(e[-1]=='\n'||e[-1]==' '))*--e=0;}
    { static char dlast[sizeof depthall_list];char dtext[sizeof depthall_list]={0};char dpath[620];snprintf(dpath,sizeof dpath,"%.*sdepthall.txt",(int)(strlen(path)-11),path);
      FILE *df=fopen(dpath,"r");if(df){size_t n=fread(dtext,1,sizeof dtext-1,df);dtext[n]=0;fclose(df);}
      if(strcmp(dtext,dlast)){memcpy(dlast,dtext,sizeof dlast);memcpy(depthall_list,dtext,sizeof depthall_list);magenta_bump();LOG("[vulkan] depth ALWAYS+write for: %s",dtext[0]?dtext:"(none)");} }
    if(!strcmp(text,last))return;memcpy(last,text,sizeof last);
    magenta_n=magenta_k=0;magenta_list[0]=0;
    if(strchr(text,':')){magenta_n=strtoull(text,NULL,10);magenta_k=strtoull(strchr(text,':')+1,NULL,10);}else memcpy(magenta_list,text,sizeof magenta_list);
    magenta_bump();LOG("[vulkan] magenta filter (generation %u): %s",magenta_gen,text[0]?text:"(off)");
}
static uint64_t pipeline_creations, pipeline_reuses;
static pipeline_entry *graphics_last_pipeline;
/* A fragment shader first seen with NVN's early fragment tests (program_shader; spirv_add_early_tests): its module is
   rebuilt with EarlyFragmentTests, the pipelines already built from the old one leave the table (rebuilt on next use;
   the old objects stay alive, as command buffers in flight and background builds may still hold them), and a marker in
   the shader cache makes later launches load it patched from the start (warm-up included). Translation thread only,
   as every other access to graphics_pipelines. TOTK_VK_EARLY_TESTS=0: off. */
static uint64_t early_tests_shaders,early_tests_pipelines;
static void shader_enable_early_tests(shader_entry *s) {
    s->early_tests=1;
    static int off=-1;if(off<0){const char *x=getenv("TOTK_VK_EARLY_TESTS");off=x&&!atoi(x);}if(off||!s->module)return;
    size_t bytes;uint32_t *code=read_shader_file_opt(s->key,"spv",&bytes);if(!code)return;
    size_t words;uint32_t *patched=spirv_add_early_tests(code,bytes/4,&words);free(code);if(!patched)return;
    if(gpl_state>0)spirv_own_set(patched,words,s->set);
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=words*4,.pCode=patched};VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device,&mi,&spv_callbacks,&module));free(patched);s->module=module;early_tests_shaders++;
    for(unsigned b=0;b<2048;b++)for(pipeline_entry **link=&graphics_pipelines[b];*link;){if((*link)->key.fs==s){*link=(*link)->next;early_tests_pipelines++;}else link=&(*link)->next;}
    graphics_last_pipeline=NULL;
    char path[1100];snprintf(path,sizeof path,"%s/%s.early",totk_cache_dir(),s->key);FILE *f=fopen(path,"w");if(f)fclose(f);
    LOG("[vulkan-early-tests] %s: EarlyFragmentTests (NVN early fragment tests); %llu shaders, %llu pipelines dropped so far",s->key,(unsigned long long)early_tests_shaders,(unsigned long long)early_tests_pipelines);
}
static void gpl_retire_poll(void);static void gpl_report(void);
static uint64_t pipeline_last_hits,pipeline_hash_calls;
static void graphics_pipeline_report(void) {
    static uint64_t frame,last_hits,hash_calls;
    gpl_retire_poll();gpl_report();
    if(!presents||presents%60||presents==frame)return;
    resource_create_report();
    uint64_t span=presents-frame,hits=pipeline_last_hits-last_hits,hashes=pipeline_hash_calls-hash_calls;
    LOG("[vulkan-pipeline-last] frame=%llu span=%llu key_bytes=%zu last_hits/frame=%.2f hash_calls/frame=%.2f hash_bytes_avoided/frame=%.0f full_key_compare=1",(unsigned long long)presents,(unsigned long long)span,sizeof(pipeline_key),hits/(double)span,hashes/(double)span,(double)hits*sizeof(pipeline_key)/span);
    frame=presents;last_hits=pipeline_last_hits;hash_calls=pipeline_hash_calls;
}
#include "nvn_vulkan_cmd_state.inc"
#include "nvn_vulkan_bindstats.inc"
// No fragment shader: one empty descriptor-set layout for every program, one object shared by runtime and warm-up so layout cache keys agree.
static shader_entry *graphics_empty_shader(void) {
    static shader_entry empty={.stage=1,.set=1};
    if(!empty.layout){pthread_mutex_lock(&graphics_object_lock);
        if(!empty.layout){VkDescriptorSetLayoutCreateInfo empty_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};VK_CHECK(vkCreateDescriptorSetLayout(device,&empty_info,NULL,&empty.layout));}
        pthread_mutex_unlock(&graphics_object_lock);}
    return &empty;
}
// Derive graphics pipeline creation state from the key, shared by runtime and warm-up; dynamic values (viewport, scissor,
// blend constants, depth bias, stencil masks/reference) stay zero/placeholder and are applied per draw as dynamic state,
// so creation results depend on fixed state only and both callers agree byte for byte.
/* The vertex shader's module for this fragment shader: the original, or a variant with the outputs the fragment
   shader reads and the vertex shader lacks (nvn_vulkan_spirv_link.inc). */
static VkShaderModule graphics_vs_module(shader_entry *vs,const shader_entry *fs) {
    uint32_t missing=fs->module?fs->io_mask&~vs->io_mask:0;
    if(!missing||!vs->spirv)return vs->module;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;pthread_mutex_lock(&lock);
    struct linked_module *m=vs->linked;while(m&&m->missing!=missing)m=m->next;
    if(!m){
        for(int l=0;l<32;l++)if(missing>>l&1&&!fs->io[l].base)die("Vulkan stage link: fragment shader %s input %d has no plain type",fs->key,l);
        size_t words;uint32_t *code=spirv_add_outputs(vs->spirv,vs->spirv_words,fs->io,missing,&words);
        m=calloc(1,sizeof *m);if(!m)die("Vulkan stage link allocation");m->missing=missing;
        VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=words*4,.pCode=code};VK_CHECK(vkCreateShaderModule(device,&mi,&spv_callbacks,&m->module));free(code);
        m->next=vs->linked;vs->linked=m;
        static unsigned said;if(said++<8)LOG("[vulkan] vertex shader %s gets outputs %#x for fragment shader %s",vs->key,missing,fs->key);
    }
    VkShaderModule module=m->module;pthread_mutex_unlock(&lock);return module;
}
/* part: 0 a whole pipeline; otherwise one VkGraphicsPipelineLibraryFlagBitsEXT bit, the library of that state built from
   the same derivation (nvn_vulkan_gpl.inc), so a linked pipeline and a whole one agree field for field */
static VkResult gpl_library_finish(unsigned part,VkGraphicsPipelineCreateInfo *pi,const pipeline_key *key,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,
    const VkPipelineVertexInputStateCreateInfo *vertex,const VkPipelineInputAssemblyStateCreateInfo *assembly,const VkPipelineRasterizationStateCreateInfo *raster,
    const VkPipelineMultisampleStateCreateInfo *multi,const VkPipelineDepthStencilStateCreateInfo *ds,const VkPipelineColorBlendStateCreateInfo *color,
    const VkPipelineDynamicStateCreateInfo *dynamic,const VkPipelineShaderStageCreateInfo *stages,const uint32_t *subgroup,VkPipeline *pipeline);
static VkResult graphics_pipeline_build(const pipeline_key *key,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout,VkPipeline *pipeline,int fatal,unsigned part);
static VkResult graphics_pipeline_create(const pipeline_key *key,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout,VkPipeline *pipeline,int fatal) {
    return graphics_pipeline_build(key,vs,fs,pass,layout,pipeline,fatal,0);
}
static VkResult graphics_pipeline_build(const pipeline_key *key,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout,VkPipeline *pipeline,int fatal,unsigned part) {
    if(key->colors>8){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan color attachments %u pending",key->colors);}
    // key->primitive is already the converted VkPrimitiveTopology; the runtime only ever produces the six point/line/triangle kinds, anything else is rejected.
    if(key->primitive>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan primitive %u pending",key->primitive);}
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned nattr=0,nstream=0;uint8_t stream_used[16]={0};
    for(unsigned i=0;i<16;i++){va_st a=key->attributes[i];if(!a.fmt||!vs->attributes[i])continue;VkFormat format=vk_format(a.fmt);
        if(format==VK_FORMAT_UNDEFINED){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex format %#x pending",a.fmt);}
        if(a.stream>15){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex stream %u pending",a.stream);}
        attrs[nattr++]=(VkVertexInputAttributeDescription){i,a.stream,format,a.offset};stream_used[a.stream]=1;}
    for(unsigned i=0;i<16;i++)if(stream_used[i]){const vs_st *s=&key->streams[i];
        if(s->divisor>1){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan vertex divisor %u pending",s->divisor);}
        streams[nstream++]=(VkVertexInputBindingDescription){i,s->stride,s->divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};}
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=nstream,.pVertexBindingDescriptions=streams,.vertexAttributeDescriptionCount=nattr,.pVertexAttributeDescriptions=attrs};
    /* Strips and fans restart at the all-ones index, as NVN does with restart on (the game's terrain strips use 0xFFFF;
       without it each 0xFFFF fetches a far-away vertex: spiky terrain over lakes and shores). Lists cannot restart. */
    VkBool32 restart=key->primitive==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP||key->primitive==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP||key->primitive==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=key->primitive,.primitiveRestartEnable=restart};
    VkViewport viewport={0,0,1,1,0,1};VkRect2D scissor={{0,0},{1,1}};
    VkPipelineViewportStateCreateInfo view={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=key->polygon.cull,.frontFace=key->polygon.front?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.depthBiasEnable=!!key->polygon.offset_en,.lineWidth=1,.depthClampEnable=key->depth_clamp,.rasterizerDiscardEnable=key->raster_discard};
    VkPipelineMultisampleStateCreateInfo multi={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT,.alphaToCoverageEnable=key->multisample.a2c};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=key->depth.dt,.depthWriteEnable=key->depth.dw,.depthCompareOp=key->depth.df?key->depth.df-1:VK_COMPARE_OP_ALWAYS,.stencilTestEnable=key->depth.st};
    ds.front.failOp=key->depth.fail_f?key->depth.fail_f-1:VK_STENCIL_OP_KEEP;ds.front.passOp=key->depth.zpass_f?key->depth.zpass_f-1:VK_STENCIL_OP_KEEP;ds.front.depthFailOp=key->depth.zfail_f?key->depth.zfail_f-1:VK_STENCIL_OP_KEEP;ds.front.compareOp=key->depth.sf_f?key->depth.sf_f-1:VK_COMPARE_OP_ALWAYS;
    ds.back.failOp=key->depth.fail_b?key->depth.fail_b-1:VK_STENCIL_OP_KEEP;ds.back.passOp=key->depth.zpass_b?key->depth.zpass_b-1:VK_STENCIL_OP_KEEP;ds.back.depthFailOp=key->depth.zfail_b?key->depth.zfail_b-1:VK_STENCIL_OP_KEEP;ds.back.compareOp=key->depth.sf_b?key->depth.sf_b-1:VK_COMPARE_OP_ALWAYS;
    /* Debug experiments: TOTK_VK_EXP=nocull,nodepth,nostencil (comma list) */
    static int exp_nocull=-1,exp_nodepth,exp_nostencil;if(exp_nocull<0){const char *e=getenv("TOTK_VK_EXP");exp_nocull=exp_env("nocull");exp_nodepth=exp_env("nodepth");exp_nostencil=exp_env("nostencil");if(e)LOG("[vulkan] experiments: %s",e);}
    /* magenta: every fragment shader replaced by one that writes (1,0,1,1) to target 0 (coverage test) */
    static VkShaderModule exp_magenta;static int exp_magenta_on=-1;if(exp_magenta_on<0)exp_magenta_on=exp_env("magenta");
    if(!exp_magenta&&(exp_magenta_on||magenta_gen)){{static const uint32_t code[]={
#include "exp_magenta_spv.inc"
        };VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof code,.pCode=code};VK_CHECK(vkCreateShaderModule(device,&mi,NULL,&exp_magenta));}}
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,.provokingVertexMode=VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    /* NVN's default (nvnCommandBufferSetProvokingVertexMode 0) */
    if(provoking_last)raster.pNext=&provoking;
    if(depthall_list[0]&&fs->module&&strstr(depthall_list,fs->key)){ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;}
    if(exp_nocull)raster.cullMode=VK_CULL_MODE_NONE;if(exp_nodepth)ds.depthTestEnable=VK_FALSE;if(exp_nostencil)ds.stencilTestEnable=VK_FALSE;
    VkPipelineColorBlendAttachmentState blend[8];for(unsigned i=0;i<key->colors;i++){blend_st b=key->blend[i];blend[i]=(VkPipelineColorBlendAttachmentState){.blendEnable=(key->color.blend_en>>i)&1,.srcColorBlendFactor=blend_factor(b.src_rgb),.dstColorBlendFactor=blend_factor(b.dst_rgb),.colorBlendOp=blend_operation(b.eq_rgb),.srcAlphaBlendFactor=blend_factor(b.src_a),.dstAlphaBlendFactor=blend_factor(b.dst_a),.alphaBlendOp=blend_operation(b.eq_a),.colorWriteMask=(key->mask.mask>>(4*i))&15};
        /* TOTK: a target the fragment shader does not write keeps its contents (Maxwell / Ryujinx); in Vulkan an unwritten
           output writes undefined values (zeros on Adreno), erasing G-buffer targets later draws leave alone */
        if(!(fs->frag_out_mask>>i&1))blend[i].colorWriteMask=0;}
    VkPipelineColorBlendStateCreateInfo color={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.flags=(key->pad_[1]&0x40)&&key->colors?VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT:0,.attachmentCount=key->colors,.pAttachments=blend};
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=graphics_vs_module(vs,fs),.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs->module,.pName="main"}};
    if(exp_magenta_on&&fs->module)stages[1].module=exp_magenta;
    /* full-screen passes read at most one: never magenta (they would cover everything) */
    unsigned vs_attrs=0;for(unsigned i=0;i<16;i++)vs_attrs+=vs->attributes[i]!=0;
    if(exp_magenta&&fs->module&&vs_attrs>=2&&((magenta_n&&strtoull(fs->key,NULL,16)%magenta_n==magenta_k)||(magenta_list[0]&&strstr(magenta_list,fs->key)))){stages[1].module=exp_magenta;LOG("[vulkan] magenta: %s",fs->key);}
    VkDynamicState dynamic_states[16]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_DEPTH_BIAS};
    unsigned ndynamic=7;
    if(eds_on){static const VkDynamicState e[]={VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,VK_DYNAMIC_STATE_STENCIL_OP_EXT,VK_DYNAMIC_STATE_CULL_MODE_EXT,VK_DYNAMIC_STATE_FRONT_FACE_EXT};
        for(unsigned i=0;i<sizeof e/sizeof *e;i++)dynamic_states[ndynamic++]=e[i];
        if(eds2_on)dynamic_states[ndynamic++]=VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT;}
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=ndynamic,.pDynamicStates=dynamic_states};
#ifdef __ANDROID__
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required[2];
    for(unsigned i=0;i<(fs->module?2u:1u);i++)
        if(!dm_vk_driver_prepare_shader_stage(&vk_api,&stages[i],&required[i])){if(!fatal)return VK_ERROR_INITIALIZATION_FAILED;die("Vulkan graphics subgroup policy: %s",dm_vk_driver_error(&vk_api));}
#endif
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=key->pad_[1]&0x80?VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT:0,.stageCount=fs->module?2:1,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&view,.pRasterizationState=&raster,.pMultisampleState=&multi,.pDepthStencilState=&ds,.pColorBlendState=&color,.pDynamicState=&dynamic,.layout=layout,.renderPass=pass->pass};
    if(part){uint32_t subgroup[2]={0,0};
#ifdef __ANDROID__
        for(unsigned i=0;i<(fs->module?2u:1u);i++)if(stages[i].pNext==&required[i])subgroup[i]=required[i].requiredSubgroupSize;
#endif
        return gpl_library_finish(part,&pi,key,vs,fs,pass,&vertex,&assembly,&raster,&multi,&ds,&color,&dynamic,stages,subgroup,pipeline);}
    return pipeline_create_graphics(&pi,pipeline);
}
#include "nvn_vulkan_gpl.inc"
/* Background pipeline creation (TOTK_VK_ASYNC_PSO, default on; 0: on the translation thread). A draw whose graphics
   pipeline is new queues it here and is skipped, and so is every draw with that pipeline until it is built (a compile
   is 5-70 ms on Turnip: a hitch in the frame, where this way the object shows a few frames late). Draws whose shaders
   write buffers or images are never skipped (the game may read their results back): those compile at once.
   Workers use the warm-up's path (graphics_pipeline_create: same driver cache keys); the pipeline cache is internally
   synchronized, entries are append-only, a worker publishes the pipeline before setting ready. */
/* 1: the entry draws with a fast-linked pipeline; swap in the whole one (nvn_vulkan_gpl.inc) */
typedef struct async_pso_job { pipeline_entry *entry; shader_entry *vs,*fs; graphics_pass_entry *pass; VkPipelineLayout layout; struct async_pso_job *next; int upgrade; } async_pso_job;
static pthread_mutex_t async_pso_lock=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t async_pso_cv=PTHREAD_COND_INITIALIZER;
static async_pso_job *async_pso_head,**async_pso_tail=&async_pso_head;
static int async_pso_state=-1;
static _Atomic uint64_t async_pso_built,async_pso_failed,async_pso_skipped,async_pso_queued,async_pso_ns;
static void *async_pso_main(void *unused) {
    /* not the translation thread's core, which created it */
    (void)unused;pthread_setname_np(pthread_self(),"totk-pso");totk_pin_self("TOTK_PSO_CPUS","all");
    /* below the game's threads: during a streaming burst the two compile threads can run 100 ms each on cores the game's
       own threads are waiting for; a compile may wait, the frame may not. TOTK_PSO_NICE (default 10, 0 keeps the normal
       priority) */
    { const char *e=getenv("TOTK_PSO_NICE");int nice=e?atoi(e):10;if(nice)setpriority(PRIO_PROCESS,(id_t)gettid(),nice); }
    /* the long-frame totals and slow-create details are the translation thread's */
    resource_create_quiet=1;pipeline_create_background=1;
    for(;;){
        pthread_mutex_lock(&async_pso_lock);
        while(!async_pso_head)pthread_cond_wait(&async_pso_cv,&async_pso_lock);
        async_pso_job *job=async_pso_head;async_pso_head=job->next;if(!async_pso_head)async_pso_tail=&async_pso_head;
        pthread_mutex_unlock(&async_pso_lock);
        uint64_t begin=dm_android_clock_ns(CLOCK_MONOTONIC);VkPipeline pipeline=VK_NULL_HANDLE;
        VkResult result=graphics_pipeline_create(&job->entry->key,job->vs,job->fs,job->pass,job->layout,&pipeline,0);
        atomic_fetch_add(&async_pso_ns,dm_android_clock_ns(CLOCK_MONOTONIC)-begin);
        /* the entry draws with its fast-linked pipeline up to this swap; a failed build keeps it */
        if(job->upgrade){
            if(result==VK_SUCCESS){VkPipeline old=__atomic_exchange_n(&job->entry->pipeline,pipeline,__ATOMIC_ACQ_REL);gpl_retire(old);atomic_fetch_add(&gpl_upgraded,1);}
            free(job);continue;}
        if(result==VK_SUCCESS){job->entry->pipeline=pipeline;atomic_store_explicit(&job->entry->ready,1,memory_order_release);atomic_fetch_add(&async_pso_built,1);}
        else{atomic_store_explicit(&job->entry->ready,-1,memory_order_release);atomic_fetch_add(&async_pso_failed,1);}
        uint64_t built=atomic_load(&async_pso_built);
        if(!(built%200)&&result==VK_SUCCESS)LOG("[vulkan-async-pso] built %llu (failed %llu), %.1f ms each, draws skipped while building %llu",(unsigned long long)built,
            (unsigned long long)atomic_load(&async_pso_failed),atomic_load(&async_pso_ns)/1e6/(built+atomic_load(&async_pso_failed)),(unsigned long long)atomic_load(&async_pso_skipped));
        free(job);
    }
    return NULL;
}
/* A draw may only be left out while its pipeline builds if every target it writes is redrawn each frame (written in each
   of the last TOTK_VK_SKIP_STEADY frames, default 30): the next frames redraw it and the object shows a few frames late.
   A target drawn once and kept -- a fused weapon's icon, the save list's blurred backdrop, the loading screen's map --
   would keep the hole for good (a blank icon, a black box), so those draws wait for their pipeline. */
static unsigned skip_steady_frames(void) {static int v=-1;if(v<0){const char *e=getenv("TOTK_VK_SKIP_STEADY");v=e&&*e?atoi(e):30;if(v<1)v=1;}return (unsigned)v;}
static int draw_targets_steady(image_entry *const *written,unsigned n) {
    for(unsigned i=0;i<n;i++)if(!written[i]->gpu_written||written[i]->gpu_write_frame+1<presents||written[i]->write_streak<skip_steady_frames())return 0;
    return n>0;
}
/* TOTK_VK_SKIP_TRACE=1 */
static void skip_trace(const char *what,image_entry *const *written,unsigned n,const shader_entry *vs,const shader_entry *fs) {
    static int on=-1;static unsigned said;if(on<0){const char *e=getenv("TOTK_VK_SKIP_TRACE");on=e&&atoi(e);}
    if(!on||said++>=3000)return;
    char t[400];int k=snprintf(t,sizeof t,"[vulkan-skip] frame %llu %s vs %s fs %s:",(unsigned long long)presents,what,vs->key,fs&&fs->module?fs->key:"-");
    for(unsigned i=0;i<n&&k<(int)sizeof t-60;i++)k+=snprintf(t+k,sizeof t-k," %ux%u fmt %#x streak %u last %lld",written[i]->texture.d.width,written[i]->texture.d.height,written[i]->texture.d.format,
        written[i]->write_streak,written[i]->gpu_written?(long long)written[i]->gpu_write_frame-(long long)presents:-999);
    LOG("%s",t);
}
static int async_pso_on(void) {
    if(async_pso_state<0){const char *e=getenv("TOTK_VK_ASYNC_PSO");async_pso_state=!(e&&!atoi(e));
        for(int i=0;async_pso_state&&i<2;i++){pthread_t t;if(pthread_create(&t,NULL,async_pso_main,NULL)){async_pso_state=0;break;}pthread_detach(t);}
        LOG("[vulkan-async-pso] background pipeline creation: %s",async_pso_state?"on (2 workers)":"off");}
    return async_pso_state;
}
static void async_pso_queue_job(pipeline_entry *entry,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout,int upgrade) {
    async_pso_job *job=malloc(sizeof *job);if(!job)die("pipeline job allocation");*job=(async_pso_job){entry,vs,fs,pass,layout,NULL,upgrade};
    pthread_mutex_lock(&async_pso_lock);*async_pso_tail=job;async_pso_tail=&job->next;pthread_cond_signal(&async_pso_cv);pthread_mutex_unlock(&async_pso_lock);
    if(!upgrade)atomic_fetch_add(&async_pso_queued,1);
}
static void async_pso_queue(pipeline_entry *entry,shader_entry *vs,shader_entry *fs,graphics_pass_entry *pass,VkPipelineLayout layout) {async_pso_queue_job(entry,vs,fs,pass,layout,0);}
// Record genuine creations only (cache hits write nothing); the manifest lives in nvn_vulkan_warm.inc.
static void pso_record_graphics(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key);
static int pso_warmed(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key);
/* In-place storage writes of the draw being recorded (ssbo_write_in_place, nvn_vulkan_shader.inc) */
/* Before the draw opens its pass: a write that skipped its barrier for the pass open at binding gets it if the draw
   lands in another pass (another framebuffer, or the pass was ended since). */
static void ssbo_in_place_before_pass(const graphics_frame_entry *frame) {
    if(!ssbo_draw.nskipped||(graphics_active_frame==frame&&render_passes==ssbo_draw.pass))return;
    graphics_pass_end();
    for(unsigned i=0;i<ssbo_draw.nskipped;i++){post_op *q=ssbo_draw.skipped[i];VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=q->gpu_access,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=q->output.buffer,.size=q->size};
        vkCmdPipelineBarrier(command,q->gpu_stage,ssbo_draw.stage[i],0,0,NULL,1,&b,0,NULL);ssbo_pass_barriers++;}
    ssbo_draw.nskipped=0;
}
/* After the draw opened its pass: its in-place writes are in that pass. */
static void ssbo_in_place_after_pass(void) {
    for(unsigned i=0;i<ssbo_draw.nwritten;i++){ssbo_draw.written[i]->inplace_pass=render_passes;ssbo_draw.written[i]->inplace_epoch=ssbo_barrier_epoch;}
    ssbo_draw.nwritten=ssbo_draw.nskipped=0;
}
/* The pass a draw renders in, decided right before the draw begins it, after everything in its setup that can end a
   pass (a forwarded read's barrier: vertices a compute shader wrote; the scene copy before the UI; in-place storage
   writes' barriers). The open pass when it still matches (*continuing; a feedback read gets its in-pass barrier), else
   a new pass after the attachments' barriers, taking over the deferred clears and block-sized targets. Decided any
   earlier, a draw would keep a pass its own setup then ended and begin it again as it was: without the barriers a new
   pass gets (square blocks in the clouds) and with the first draw's load ops, CLEAR or DONT_CARE (the cloud particles
   cleared away, the cloud sea missing in 3 frames of 4). */
static graphics_frame_entry *draw_pass_decide(graphics_pass_entry *pass,graphics_pass_key *pass_key,VkAttachmentDescription *attachments,const VkAttachmentReference *colors,VkAttachmentReference depth,
                                              const VkImageView *views,image_entry *const *images,unsigned na,uint32_t width,uint32_t height,block_writeback *blocks,const int *block_load,unsigned nblocks,int *continuing) {
    /* experiment: the pass ends right before the decision */
    if(graphics_active_frame&&live_range("keyend",draw_count-live_frame_base))graphics_pass_end();
    /* the deferred clears baked into the pass as CLEAR load ops (pclear_attach): one done meanwhile as a transfer clear
       (the draw's setup used the image: the scene copy or SMAA before the UI) is loaded instead; cleared again, it would
       erase what was done to it since */
    { unsigned kept=0;int changed=0;
      for(unsigned k=0;k<pclear_attach_n;k++){unsigned i=pclear_attach_i[k];
          if(pclear_attach_e[k]->pclear){pclear_attach_e[kept]=pclear_attach_e[k];pclear_attach_i[kept++]=i;continue;}
          attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;if(attachments[i].stencilLoadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)attachments[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;changed=1;}
      pclear_attach_n=kept;
      if(changed){*pass_key=graphics_pass_key_make(pass_key->attachments,pass_key->colors,attachments,colors,depth);pass=graphics_pass_get(*pass_key,attachments,colors,depth,1);} }
    *continuing=0;
    if(!pclear_attach_n&&graphics_pass_matches(pass_key,views,width,height)){*continuing=1;
        /* a feedback draw inside the pass: the earlier draws' writes to the target, made visible to this draw's sampling */
        int dirty=0;for(unsigned k=0;k<fb_count;k++)if(fb_images[k]->fb_dirty){dirty=1;fb_images[k]->fb_dirty=0;}
        if(dirty){VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,1,&b,0,NULL,0,NULL);fb_barriers++;}
        return graphics_active_frame;
    }
    graphics_pass_end();
    /* done by the pass's CLEAR load ops, which happen even when the draw is left out (its pipeline still building): the
       image holds the clear from here on. Left unmarked, a never-populated target would take a guest-memory upload over
       the clear when the next draw samples it: the first Ultrahand frame reads stale texels as object IDs and crashes
       on them. */
    for(unsigned k=0;k<pclear_attach_n;k++){TRACEIMG(&pclear_attach_e[k]->texture,"deferred clear as load op");pclear_forget(pclear_attach_e[k]);pclear_loadops++;image_gpu_written(pclear_attach_e[k]);}
    pclear_attach_n=0;
    for(unsigned i=0;i<nblocks;i++){if(block_load[i])block_copy(&blocks[i],0);block_writeback_add(blocks[i]);}
    synccheck_writer=synccheck_reader=diag_fs?diag_fs->key:NULL;
    for(unsigned i=0;i<na;i++)if(i==depth.attachment){attachment_write_real=state.depth.dw||(state.depth.st&&(state.stencil_faces[0].writeMask|state.stencil_faces[1].writeMask));
            image_access(images[i],VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);attachment_write_real=1;}
        /* sampled by this draw too: the barrier covers the read */
        else {int fb=0;int slot=-1;for(int c=0;c<state.rt.n;c++)if(colors[c].attachment==i)slot=c;attachment_write_real=slot<0||((state.mask.mask>>(4*slot))&15)!=0;for(unsigned k=0;k<fb_count;k++)fb|=fb_images[k]==images[i];
            if(fb)images[i]->fb_dirty=0;
            image_access(images[i],VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|(fb?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:0),VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|(fb?VK_ACCESS_SHADER_READ_BIT:0));attachment_write_real=1;}
    return graphics_frame_get(pass,views,images,width,height);
}
/* frame profile (TOTK_VK_FRAMEPROF): wall time through a draw: 0 before bind, 1 bind, 2 render targets, 3 pipeline, 4 waiting for
   a build, 5 to the pass begun, 6 recording. */
static uint64_t draw_seg_ns[7],draw_seg_t;static int draw_seg_on=-1;
#define DRAW_SEG(i) do{if(draw_seg_on<0){const char *e_=getenv("TOTK_VK_FRAMEPROF");draw_seg_on=e_&&atoi(e_);}if(draw_seg_on){uint64_t n_=dm_android_clock_ns(CLOCK_MONOTONIC);if((i)>=0&&draw_seg_t)draw_seg_ns[(i)<0?0:(i)]+=n_-draw_seg_t;draw_seg_t=n_;}}while(0)
static uint64_t rt_memo_hits;
static void draw_graphics_inner(const p_draw *draw,const p_draw_idx *indexed,const draw_plan *plan);
static void draw_graphics(const p_draw *draw,const p_draw_idx *indexed,const draw_plan *plan) {
    /* measurement only: TOTK_VK_EXP=conddraw draws what a render condition turned off (gated clears stay off) */
    /* TOTK: SetRenderEnable(false) */
    if(state.render_disabled){static int all=-1;if(all<0)all=exp_env("conddraw");if(!all||!state.render_cond_off)return;}
    DRAW_SEG(-1);uint64_t phase=dm_cpu_phase_begin();draw_graphics_inner(draw,indexed,plan);dm_cpu_phase_end(DM_CPU_PHASE_DRAW,phase);
}
static void draw_graphics_inner(const p_draw *draw,const p_draw_idx *indexed,const draw_plan *plan) {
    // NVN draws with zero vertices/instances access no indices/resources and add no occlusion samples.
    if ((draw && draw->count <= 0) || (indexed && (indexed->count <= 0 || !indexed->instances))) return;
    snapshot_prepare();
    /* TOTK: a draw with no program draws nothing (counted, logged a few times) */
    if(!state.program){static uint64_t skipped;if(skipped++<8||!(skipped%1024))LOG("[vulkan] draw without program skipped (%llu)",(unsigned long long)skipped);return;}
    shader_entry *vs=NULL,*fs=NULL;
    /* the plan made when the recording completed (nvn_vulkan_plan.inc): its shaders where its check passes, else resolved here */
    int planned=plan&&plan_check(plan);
    if(planned){vs=plan->vs;fs=plan->fs_variant?plan->fs:NULL;}
    else for(unsigned i=0;i<state.program->nstage;i++){shader_entry *s=program_shader(state.program,i);if(s->stage==0)vs=s;else if(s->stage==1)fs=s;else die("Vulkan graphics stage %u pending",s->stage);}
    if(!vs)die("Vulkan draw missing vertex stage");
    {uint64_t phase=dm_cpu_phase_begin();dependencies_prepare_draw(vs,fs,draw,indexed);dm_cpu_phase_end(DM_CPU_PHASE_DEPENDENCY,phase);}
    // Submit early only after the previous pass naturally ended, before binding or acquiring resource versions.
    /* mid-pass cut (ASYNC_CHUNK_SPLIT_DRAWS) */
    if(graphics_active_frame&&async_chunk_due())graphics_pass_end();
    if(!graphics_active_frame)async_chunk_boundary_locked();
    /* measurement only: TOTK_VK_EXP=restart<N> ends the pass before every Nth draw (the cost of a same-framebuffer
       restart) */
    { static int every=-1;if(every<0){const char *x=getenv("TOTK_VK_EXP"),*r=exp_word(x,"restart",1);every=r?atoi(r):0;}
      static unsigned n;if(every>0&&graphics_active_frame&&++n%(unsigned)every==0)graphics_pass_end();
      if(graphics_active_frame&&live_range("restart",draw_count-live_frame_base))graphics_pass_end(); }
    if(!fs)fs=graphics_empty_shader();
    /* fragment shaders drawn inside a live samples count (the game's occlusion feedback) */
    { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_QUERYFS");on=x&&atoi(x);}
      if(on&&graphics_shader_writes(fs)){static struct { const char *key; unsigned n; } w[64];static unsigned nw;static uint64_t lastw;unsigned i=0;while(i<nw&&w[i].key!=fs->key)i++;
        if(i==nw&&nw<64)w[nw++]=(typeof(w[0])){fs->key,0};if(i<64)w[i].n++;
        if(presents>=lastw+300){if(lastw)for(unsigned k=0;k<nw;k++)LOG("[vulkan-storefs] %s %.2f draws/frame with storage writes",w[k].key,w[k].n/300.);lastw=presents;nw=0;}}
      if(on&&graphics_query_open){static struct { const char *key; unsigned n; } q[64];static unsigned nq;static uint64_t last;unsigned i=0;while(i<nq&&q[i].key!=fs->key)i++;
        if(i==nq&&nq<64)q[nq++]=(typeof(q[0])){fs->key,0};if(i<64)q[i].n++;
        if(presents>=last+300){if(last)for(unsigned k=0;k<nq;k++)LOG("[vulkan-queryfs] %s %.2f draws/frame inside a live samples count",q[k].key,q[k].n/300.);last=presents;nq=0;}} }
    /* TOTK_TAA=asr: ASR wrote the canvas in place of the game's upscale */
    if(taa_enabled()==2&&asr_upscale_draw(fs))return;
    int compose_hook=taa_enabled()||smaa_scene();
    /* TOTK_TAA / SMAA: the first draw reading compose's output resolves it first */
    if(compose_hook)taa_before_draw(fs);
    /* the compose draw: note its target */
    int taa_draw=compose_hook&&fs->module&&!strncmp(fs->game_id,"0ac9a54938974396",16);
    if(taa_draw)taa_compose(fs);
    post_op *const *output_scope=post_copy_tail;forward_scope_marker=output_scope;
    // Initializer-list evaluation order is unspecified in C; the second set's dynamic offsets follow the first,
    // so the two binds must be ordered statements.
    uint32_t dynamic_offsets[256];unsigned dynamic_count=0;
    DRAW_SEG(0);uint64_t bind_phase=dm_cpu_phase_begin();
    VkDescriptorSet sets[2];
    fb_count=0;fb_slots=0;fb_in_draw=1;ssbo_in_place_draw_begin();
    sets[0]=bind_shader(vs,NULL,dynamic_offsets,&dynamic_count);
    sets[1]=bind_shader(fs,NULL,dynamic_offsets,&dynamic_count);
    fb_in_draw=0;
    dm_cpu_phase_end(DM_CPU_PHASE_BIND,bind_phase);DRAW_SEG(1);
    uint64_t layout_phase=dm_cpu_phase_begin();VkPipelineLayout layout=planned?plan->layout:graphics_layout_get(vs,fs);dm_cpu_phase_end(DM_CPU_PHASE_LAYOUT,layout_phase);
    block_writeback blocks[8];int block_load[8]={0};unsigned nblocks=0;
    /* the attachments this draw writes: marked GPU-written only once the draw is known to run (a draw whose pipeline is
       still building is left out, and must not leave its targets marked as drawn) */
    image_entry *written[18];unsigned nwritten=0;pclear_attach_n=0;
    VkImageView views[9]={0};image_entry *images[9]={0};VkAttachmentDescription attachments[9]={0};VkAttachmentReference colors[8]={0},depth={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};unsigned na=0;uint32_t width=UINT32_MAX,height=UINT32_MAX;
    diag_fs=fs;synccheck_reader=fs?fs->key:NULL;
    /* The same render targets as the last draw (the same objects, images, populated state): the same views, attachments
       and pass, without the image and view lookups (most draws share their targets with the previous one). */
    static struct { int valid; p_set_rt rt; uint64_t serial[9], gen; int populated[9]; unsigned na; uint32_t width, height; image_entry *images[9]; VkImageView views[9]; VkAttachmentDescription attachments[9]; VkAttachmentReference colors[8], depth; graphics_pass_key pass_key; graphics_pass_entry *pass; } rtm;
    graphics_pass_key pass_key;graphics_pass_entry *pass;int rt_hit=0;
    if(rtm.valid&&rtm.gen==image_generation&&!memcmp(&rtm.rt,&state.rt,sizeof state.rt)){rt_hit=1;
        for(int i=0;i<state.rt.n&&rt_hit;i++)if(state.rt.color[i]&&state.rt.color[i]->serial!=rtm.serial[i])rt_hit=0;
        if(rt_hit&&state.rt.depth&&state.rt.depth->serial!=rtm.serial[8])rt_hit=0;
        /* a deferred clear is baked into the pass (pclear_attach): built afresh */
        for(unsigned k=0;k<rtm.na&&rt_hit;k++)if(rtm.images[k]->populated!=rtm.populated[k]||rtm.images[k]->pclear)rt_hit=0;}
    if(rt_hit){na=rtm.na;width=rtm.width;height=rtm.height;memcpy(images,rtm.images,sizeof images);memcpy(views,rtm.views,sizeof views);memcpy(attachments,rtm.attachments,sizeof attachments);memcpy(colors,rtm.colors,sizeof colors);depth=rtm.depth;pass_key=rtm.pass_key;pass=rtm.pass;rt_memo_hits++;
        /* the attachments this draw writes, as the loop records them */
        for(unsigned k=0;k<na;k++)written[nwritten++]=images[k];}
    else{
    for(int i=0;i<state.rt.n;i++){
        colors[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};if(!state.rt.color[i])continue;image_entry *e=image_get(state.rt.color[i]);const view_t *v=state.rt.has_cview[i]?&state.rt.cview[i]:NULL;
        image_entry *aux=block_aux_get(e,v);if(aux){block_load[nblocks]=e->populated;blocks[nblocks++]=block_region(e,aux,texture_range(e,v));aux->populated=e->populated;written[nwritten++]=e;e=aux;}
        VkImageSubresourceRange range=texture_range(e,v);uint32_t w=dimension(e->texture.d.width,range.baseMipLevel),h=dimension(e->texture.d.height,range.baseMipLevel);if(w<width)width=w;if(h<height)height=h;
        VkFormat af=aux?e->format:attachment_format(e,state.rt.color[i],v);images[na]=e;views[na]=attachment_view(e,v,af);attachments[na]=(VkAttachmentDescription){.format=af,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};pclear_attach(e,range,&attachments[na],na);colors[i].attachment=na++;written[nwritten++]=e;
    }
    if(state.rt.depth){image_entry *e=image_get(state.rt.depth);const view_t *v=state.rt.has_dview?&state.rt.dview:NULL;VkImageSubresourceRange range=texture_range(e,v);uint32_t w=dimension(e->texture.d.width,range.baseMipLevel),h=dimension(e->texture.d.height,range.baseMipLevel);if(w<width)width=w;if(h<height)height=h;images[na]=e;views[na]=attachment_view(e,v,0);attachments[na]=(VkAttachmentDescription){.format=e->format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=e->populated?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};pclear_attach(e,range,&attachments[na],na);depth.attachment=na++;written[nwritten++]=e;}
    /* TOTK: a draw with no render target, kept for its fragment shader's storage writes (an agl shader samples a cube
       map into a 1x1 viewport for the ambient light the game reads back; without it the world is nearly black). The
       pass has no attachments and the viewport's extent (else the scissor's), as the Metal backend does. */
    if(!na){
        const float *vp=state.viewports.v[0];double x1=vp[0]+(vp[2]>0?vp[2]:0),y1=vp[1]+(vp[3]>0?vp[3]:0);
        if((x1<1||y1<1)&&state.scissor.w>0){x1=(double)state.scissor.x+state.scissor.w;y1=(double)state.scissor.y+state.scissor.h;}
        width=x1<1?1:x1>16384?16384:(uint32_t)ceil(x1);height=y1<1?1:y1>16384?16384:(uint32_t)ceil(y1);
    }
    pass_key=graphics_pass_key_make(na,state.rt.n,attachments,colors,depth);
    /* this draw's own; pipelines are built against it (compatible with the open pass when that is kept) */
    pass=graphics_pass_get(pass_key,attachments,colors,depth,1);
    if(na&&!nblocks&&!pclear_attach_n){rtm.valid=1;rtm.rt=state.rt;rtm.gen=image_generation;for(int i=0;i<8;i++)rtm.serial[i]=i<state.rt.n&&state.rt.color[i]?state.rt.color[i]->serial:0;rtm.serial[8]=state.rt.depth?state.rt.depth->serial:0;
        for(unsigned k=0;k<na;k++)rtm.populated[k]=images[k]->populated;rtm.na=na;rtm.width=width;rtm.height=height;memcpy(rtm.images,images,sizeof images);memcpy(rtm.views,views,sizeof views);memcpy(rtm.attachments,attachments,sizeof attachments);memcpy(rtm.colors,colors,sizeof colors);rtm.depth=depth;rtm.pass_key=pass_key;rtm.pass=pass;}
    else rtm.valid=0;}
    graphics_frame_entry *frame;int continuing=0;
    /* block-sized targets have no feedback-loop variant: synchronized here, ahead of the pipeline key */
    if(nblocks&&fb_count){for(unsigned k=0;k<fb_count;k++)image_access(fb_images[k],VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);fb_count=0;}
    // Copy only the vertex prefix reachable by the actual indices, rather than the entire large bound pool for every draw.
    VkBuffer index_buffer=VK_NULL_HANDLE;VkIndexType index_type=VK_INDEX_TYPE_UINT16;uint32_t index_count=indexed?(uint32_t)indexed->count:0;uint64_t index_off=0;int index_in_place=0;
    int64_t highest=draw?(int64_t)draw->first+draw->count-1:0;
    if(indexed){unsigned size=indexed->itype==0?1:indexed->itype==1?2:4;uint64_t bytes=(uint64_t)indexed->count*size;
        if(bytes>UINTPTR_MAX-indexed->ibuf)die("Vulkan draw index range overflow");
        // Reacquire after any vertex-dependency flush rather than retaining the preflight handle.
        vk_buffer raw={0};uint32_t maximum=0;
        if(inplace_on()){if(size!=1&&!quad_primitive(indexed->prim)){VkBuffer pool=zc_buffer_at(indexed->ibuf,bytes,&index_off);if(pool){raw.buffer=pool;index_in_place=1;inplace_index++;}else{inplace_fallback++;inplace_why[IF_INDEX_POOL]++;inplace_classify(indexed->ibuf,bytes);}}else{inplace_fallback++;inplace_why[IF_INDEX_KIND]++;}}
        if(!index_in_place&&!index_stash_take(indexed,bytes,&raw,&maximum)){raw=snapshot_buffer(indexed->ibuf,bytes);maximum=index_buffer_maximum(raw,size);}vk_buffer indices=raw;
        if(quad_primitive(indexed->prim)){post_op *p=post_new(3);p->source=buffer_create((uint64_t)quad_index_bound(indexed->prim,indexed->count)*4+4);
            index_count=quad_indices((uint32_t*)p->source.mapped,indexed->prim,raw.mapped,size,indexed->count);indices=p->source;
            static int said;if(!said++)LOG("[vulkan] NVN indexed %s: %d %u-bit indices drawn as %u triangles",indexed->prim==7?"quads":"quad strip",indexed->count,size*8,index_count/3);size=4;}
        /* the 8-bit restart index stays a restart */
        else if(size==1){post_op *p=post_new(3);p->source=buffer_create((uint64_t)indexed->count*2);for(int i=0;i<indexed->count;i++){uint8_t v=((const uint8_t*)raw.mapped)[i];((uint16_t*)p->source.mapped)[i]=v==0xff?0xffff:v;} indices=p->source;}
        highest=(int64_t)maximum+indexed->base_vertex;index_buffer=indices.buffer;index_type=size==4?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16;
    }
    /* non-indexed quads: indices from 0, drawn from draw->first (vertex ids stay first + i) */
    if(!indexed&&quad_primitive(draw->prim)&&draw->count>0){post_op *p=post_new(3);p->source=buffer_create((uint64_t)quad_index_bound(draw->prim,(uint32_t)draw->count)*4+4);
        index_count=quad_indices((uint32_t*)p->source.mapped,draw->prim,NULL,4,(uint32_t)draw->count);index_buffer=p->source.buffer;index_type=VK_INDEX_TYPE_UINT32;
        static int said;if(!said++)LOG("[vulkan] NVN %s: %d vertices drawn as %u triangles",draw->prim==7?"quads":"quad strip",draw->count,index_count/3);}
    if(highest<0)die("Vulkan negative vertex fetch");
    VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription streams[16];unsigned nattr=0,nstream=0;uint8_t stream_used[16]={0};uint32_t attribute_end[16]={0};VkBuffer vertex_buffers[16]={0};VkDeviceSize offsets[16]={0};
    /* the key, vertex input and primitive: from the plan where its check passes (the targets as planned, inherited state as assumed) */
    int keyed=planned&&plan_check_key(plan,na,attachments,colors,depth,nblocks);
    pipeline_key key;ds_st dyn_depth;poly_st dyn_polygon;VkPrimitiveTopology primitive;
    if(keyed){nattr=plan->nattr;nstream=plan->nstream;memcpy(attrs,plan->attrs,nattr*sizeof *attrs);memcpy(streams,plan->streams,nstream*sizeof *streams);memcpy(stream_used,plan->stream_used,sizeof stream_used);memcpy(attribute_end,plan->attribute_end,sizeof attribute_end);primitive=plan->primitive;}
    else{
    for(unsigned i=0;i<16;i++){va_st a=state.attributes[i];if(!a.fmt||!vs->attributes[i])continue;VkFormat format=vk_format(a.fmt);if(format==VK_FORMAT_UNDEFINED)die("Vulkan vertex format %#x pending",a.fmt);{static uint8_t checked[256];if(!checked[a.fmt&255]){checked[a.fmt&255]=1;VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(physical,format,&fp);if(!(fp.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))LOG("[vulkan] 【红】vertex format %#x (VkFormat %d) cannot be fetched on this device",a.fmt,format);}}attrs[nattr++]=(VkVertexInputAttributeDescription){i,a.stream,format,a.offset};stream_used[a.stream]=1;int bw,bh;uint32_t end=a.offset+nvn_fmt_bpb(a.fmt,&bw,&bh);if(end>attribute_end[a.stream])attribute_end[a.stream]=end;}
    for(unsigned i=0;i<16;i++)if(stream_used[i]){const vs_st *s=&state.streams[i];if(s->divisor>1)die("Vulkan vertex divisor %u pending",s->divisor);streams[nstream++]=(VkVertexInputBindingDescription){i,s->stride,s->divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};}
    primitive=topology(draw?draw->prim:indexed->prim);}
    for(unsigned i=0;i<16;i++)if(stream_used[i]){const vs_st *s=&state.streams[i];const NVNbufferRange *v=&state.vertices[i];if(!v->address||!v->size)die("Vulkan missing vertex buffer %u",i);
        if(inplace_on()){uint64_t off;VkBuffer pool=zc_buffer_at(v->address,1,&off);if(pool){vertex_buffers[i]=pool;offsets[i]=off;inplace_vbuf++;continue;}inplace_fallback++;inplace_why[IF_VBUF_POOL]++;inplace_classify(v->address,v->size);}
        /* instance streams: up to the last instance (copying instance 0 only would leave particles reading garbage) */
        uint64_t element=s->divisor?draw_last_instance(draw,indexed):(uint64_t)highest; uint64_t needed=index_in_place?v->size:element*s->stride+attribute_end[i];if(needed>v->size)needed=v->size;static fwd_memo vertex_memo[16];VkDescriptorBufferInfo fetched=forward_readonly_buffer_m(&vertex_memo[i],v->address,needed,1,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);vertex_buffers[i]=fetched.buffer;offsets[i]=fetched.offset;}
    // NVN permits reversed or constant ranges, as does Vulkan; state initialization supplies the 0..1 default.
    float *v=state.viewports.v[0];VkViewport viewport={v[0],v[1],v[2]>0?v[2]:width,v[3]>0?v[3]:height,state.depth_range.n,state.depth_range.f};
    // Bound real-range diagnostics to 12 distinct pairs, without readbacks or rendering changes.
    static uint64_t depth_ranges_seen[12];static unsigned depth_ranges_count;uint64_t depth_range_bits;
    if(depth_ranges_count<12){memcpy(&depth_range_bits,&state.depth_range,sizeof depth_range_bits);unsigned i=0;while(i<depth_ranges_count&&depth_ranges_seen[i]!=depth_range_bits)i++;
        if(i==depth_ranges_count){depth_ranges_seen[depth_ranges_count++]=depth_range_bits;LOG("[vulkan-depth] range=%g,%g test=%u write=%u compare=%u target=%s",viewport.minDepth,viewport.maxDepth,(unsigned)state.depth.dt,(unsigned)state.depth.dw,(unsigned)state.depth.df,state.rt.depth?"depth":"color-only");}}
    int x=state.scissor.x>0?state.scissor.x:0,y=state.scissor.y>0?state.scissor.y:0;int sw=state.scissor.w>0?state.scissor.w:width,sh=state.scissor.h>0?state.scissor.h:height;if(x+sw>(int)width)sw=width-x;if(y+sh>(int)height)sh=height-y;if(sw<0||sh<0)die("Vulkan scissor outside target");VkRect2D scissor={{x,y},{sw,sh}};
    // Metal clip Y points up while Vulkan's positive viewport points down; a negative viewport preserves NVN orientation.
    { static uint64_t larger;if((viewport.width>width+0.5f||viewport.height>height+0.5f)&&width>=512){larger++;if(larger<=20||!(larger%2000))LOG("[vulkan-dynres] frame %llu: viewport %gx%g on a %ux%u target (%llu so far)",(unsigned long long)presents,viewport.width,viewport.height,width,height,(unsigned long long)larger);} }
    /* frame generation's depth, TOTK_DYNRES_STATS */
    if(depth.attachment!=VK_ATTACHMENT_UNUSED&&state.rt.n>=3)dynres_note(viewport.width,viewport.height,width,height);
    viewport.y += viewport.height; viewport.height = -viewport.height;
    /* TOTK_TAA: the G-buffer pass's size */
    taa_viewport(&viewport,depth.attachment!=VK_ATTACHMENT_UNUSED?images[depth.attachment]:NULL,state.rt.n,state.rt.n?state.rt.color[0]:NULL,vs,fs);
    if(keyed){key=plan->key;dyn_depth=plan->dyn_depth;dyn_polygon=plan->dyn_polygon;}
    else{
    key=(pipeline_key){.vs=vs,.fs=fs,.colors=state.rt.n,.primitive=primitive,.color=state.color,.mask=state.mask,.polygon=state.polygon,.depth=depth.attachment!=VK_ATTACHMENT_UNUSED?state.depth:(ds_st){0},.multisample=state.multisample,.attachments=na,.depth_attachment=depth.attachment,.depth_clamp=(uint8_t)state.depth_clamp,.raster_discard=(uint8_t)state.raster_discard};
#ifdef __ANDROID__
    key.required_subgroup_size=vk_api.requested_subgroup_size;
#endif
    for(int i=0;i<state.rt.n;i++){key.color_indices[i]=colors[i].attachment;if(colors[i].attachment!=VK_ATTACHMENT_UNUSED)key.formats[i]=attachments[colors[i].attachment].format;if((state.color.blend_en>>i)&1)key.blend[i]=state.blend[i];}
    if(depth.attachment!=VK_ATTACHMENT_UNUSED)key.formats[8]=attachments[depth.attachment].format;
    for(unsigned i=0;i<16;i++){if(state.attributes[i].fmt&&vs->attributes[i])key.attributes[i]=state.attributes[i];if(stream_used[i])key.streams[i]=state.streams[i];}
    /* The state words are the game's NVN objects copied as they are: bits beyond the fields we use differ between
       otherwise equal states and would make new pipelines. Keep only the fields. */
    {poly_st p={0};p.cull=key.polygon.cull;p.front=key.polygon.front;p.mode=key.polygon.mode;p.offset_en=key.polygon.offset_en;key.polygon=p;
     color_st c={0};c.blend_en=key.color.blend_en;c.logic_op=key.color.logic_op;c.alpha_func=key.color.alpha_func;key.color=c;
     ds_st d={0};d.dt=key.depth.dt;d.dw=key.depth.dw;d.df=key.depth.df;d.st=key.depth.st;d.sf_f=key.depth.sf_f;d.sf_b=key.depth.sf_b;d.fail_f=key.depth.fail_f;d.zfail_f=key.depth.zfail_f;
     d.zpass_f=key.depth.zpass_f;d.fail_b=key.depth.fail_b;d.zfail_b=key.depth.zfail_b;d.zpass_b=key.depth.zpass_b;key.depth=d;
     for(unsigned i=0;i<8;i++){blend_st b={0},o=key.blend[i];b.target=o.target;b.src_rgb=o.src_rgb;b.dst_rgb=o.dst_rgb;b.src_a=o.src_a;b.dst_a=o.dst_a;b.eq_rgb=o.eq_rgb;b.eq_a=o.eq_a;key.blend[i]=b;}}
    /* extended dynamic state: these are set per draw below, so pipelines differing only in them are one pipeline */
    dyn_depth=key.depth;dyn_polygon=key.polygon;
    if(depthall_list[0]&&fs->module&&strstr(depthall_list,fs->key)){dyn_depth.dt=1;dyn_depth.dw=1;dyn_depth.df=VK_COMPARE_OP_ALWAYS+1;}
    if(eds_on){memset(&key.depth,0,sizeof key.depth);key.polygon.cull=0;key.polygon.front=0;if(eds2_on)key.polygon.offset_en=0;}
    }
    /* top bit: built with the feedback-loop flag; 0x40: also in rasterization order (rorder_ok), for a feedback draw whose
       fragments can overlap each other: more than two triangles or more than one instance (a decal quad cannot) */
    /* the draw writes a target it samples (write mask and shader output) */
    int overlap=0,writes_read=0;
    for(unsigned i=0;i<8;i++)if(fb_slots>>i&1&&(state.mask.mask>>(4*i)&15)&&fs&&fs->frag_out_mask>>i&1)writes_read=1;
    if(fb_count&&rorder_ok){int prim=draw?draw->prim:indexed->prim,n=draw?draw->count:indexed->count,inst=draw?draw->instances:indexed->instances;
        overlap=writes_read&&(inst>1||(prim==4?n/3:prim==5||prim==6?n-2:n)>2);}
    { static uint64_t last,f,w,o;if(fb_count){f++;w+=writes_read;o+=overlap;}
      if(presents>=last+600){if(last&&f)LOG("[vulkan-feedback] a frame: %.1f feedback draws, %.1f write a target they sample, %.1f of those can overlap themselves (rasterization order)",f/600.0,w/600.0,o/600.0);last=presents;f=w=o=0;} }
    key.pad_[0]=magenta_gen;key.pad_[1]=(override_gen&0x3f)|(fb_count?0x80:0)|(overlap?0x40:0);
    // Persistent pipelines are append-only and accessed under queue_lock; only a full-key match bypasses hashing.
    DRAW_SEG(2);unsigned pipeline_bucket=0;pipeline_entry *cached=graphics_last_pipeline;
    if(cached&&!memcmp(&cached->key,&key,sizeof key))pipeline_last_hits++;
    else{pipeline_hash_calls++;pipeline_bucket=graphics_hash(&key,sizeof key)&2047;cached=graphics_pipelines[pipeline_bucket];while(cached&&memcmp(&cached->key,&key,sizeof key))cached=cached->next;}
    if(!cached)pipeline_miss_explain(&key);
    int steady=draw_targets_steady(written,nwritten);
    int background=!cached&&async_pso_on()&&!graphics_shader_writes(vs)&&!graphics_shader_writes(fs)&&!magenta_gen&&!override_gen&&!pso_warmed(vs,fs,&pass->key,&key);
    /* not built by the warm-up: linked from pipeline libraries (nvn_vulkan_gpl.inc), the whole one built behind it. With the
       libraries prepared every such pipeline is a link of a few microseconds and draws at once; without, a target redrawn
       every frame keeps the background build that leaves the draw out meanwhile (a library compile would be the frame's) */
    if(!cached&&gpl_on()&&!magenta_gen&&!override_gen&&async_pso_on()&&!pso_warmed(vs,fs,&pass->key,&key)&&(gpl_prepared||!(background&&steady))){
        resource_create_start creation=resource_create_begin();pipeline_entry *e=calloc(1,sizeof *e);if(!e)die("Vulkan pipeline cache allocation");e->key=key;
        if(gpl_link(&key,vs,fs,pass,layout,&e->pipeline)==VK_SUCCESS){atomic_store(&e->ready,1);e->next=graphics_pipelines[pipeline_bucket];graphics_pipelines[pipeline_bucket]=e;pipeline_creations++;
            /* TOTK_VK_GPL_NOUPGRADE=1 (diagnostic): fast-linked pipelines stay */
            resource_create_end(1,creation,vs->key,fs->module?fs->key:NULL);{static int keep=-1;if(keep<0){const char *x=getenv("TOTK_VK_GPL_NOUPGRADE");keep=x&&atoi(x);}if(!keep)async_pso_queue_job(e,vs,fs,pass,layout,1);} pso_record_graphics(vs,fs,&pass->key,&key);cached=e;pipeline_reuses--;}
        else free(e);}
    if(background&&steady&&!cached)skip_trace("queued, skipped",written,nwritten,vs,fs);else if(background&&!cached)skip_trace("built now, a kept target",written,nwritten,vs,fs);
    /* built in the background (one the warm-up built is a driver-cache hit: made here) */
    if(background&&steady&&!cached){
        cached=calloc(1,sizeof *cached);if(!cached)die("Vulkan pipeline cache allocation");cached->key=key;
        cached->next=graphics_pipelines[pipeline_bucket];graphics_pipelines[pipeline_bucket]=cached;pipeline_creations++;
        async_pso_queue(cached,vs,fs,pass,layout);pso_record_graphics(vs,fs,&pass->key,&key);}
    if(!cached){resource_create_start creation=resource_create_begin();cached=calloc(1,sizeof *cached);if(!cached)die("Vulkan pipeline cache allocation");cached->key=key;
        // Derive pipeline-creation state only on a miss; per-draw dynamic state is applied separately below. Creation shares graphics_pipeline_create with warm-up so driver cache keys agree.
        VK_CHECK(graphics_pipeline_create(&key,vs,fs,pass,layout,&cached->pipeline,1));atomic_store(&cached->ready,1);
        cached->next=graphics_pipelines[pipeline_bucket];graphics_pipelines[pipeline_bucket]=cached;pipeline_creations++;resource_create_end(1,creation,vs->key,fs->module?fs->key:NULL);
        /* bisection pipelines stay out of the warm-up list */
        if(!magenta_gen&&!override_gen)pso_record_graphics(vs,fs,&pass->key,&key);}else pipeline_reuses++;
    DRAW_SEG(3);graphics_last_pipeline=cached;
    {int ready=atomic_load_explicit(&cached->ready,memory_order_acquire);
     /* the background build failed: here, with the error reported */
     if(ready<0){VK_CHECK(graphics_pipeline_create(&cached->key,vs,fs,pass,layout,&cached->pipeline,1));atomic_store(&cached->ready,1);}
     /* still building, and a target that is kept: wait for it */
     else if(!ready&&!steady){
         skip_trace("waits for its build, a kept target",written,nwritten,vs,fs);
         while(!(ready=atomic_load_explicit(&cached->ready,memory_order_acquire)))usleep(100);
         if(ready<0){VK_CHECK(graphics_pipeline_create(&cached->key,vs,fs,pass,layout,&cached->pipeline,1));atomic_store(&cached->ready,1);}}
     /* still building: this draw is left out. If it ended the previous pass to start its own, the writebacks of
        compressed targets it queued for that pass (the stand-in, unchanged without the draw) are done now */
     else if(!ready){
         /* its CLEAR load ops still happen */
         if(pclear_attach_n){graphics_pass_begin(draw_pass_decide(pass,&pass_key,attachments,colors,depth,views,images,na,width,height,blocks,block_load,nblocks,&continuing));graphics_pass_end();}
         if(!graphics_active_frame)block_writeback_flush();
         atomic_fetch_add(&async_pso_skipped,1);return;}}
    { char keys[40];if(gpu_watch_address()){snprintf(keys,sizeof keys,"vs %s fs %s",vs->key,fs&&fs->module?fs->key:"-");gpu_write_shaders=keys;}
      for(unsigned i=0;i<nwritten;i++)image_gpu_written(written[i]);
      gpu_write_shaders=NULL; }
    /* TOTK_VK_FSWATCH=<fs key>: its draws in each of 60 frames, a digit a frame */
    { static const char *w=(const char *)1;static unsigned char n[60];static unsigned len;static uint64_t frame;
      if(w==(const char *)1)w=getenv("TOTK_VK_FSWATCH");
      if(w&&*w){if(presents!=frame){if(frame&&++len==60){char d[61];for(unsigned i=0;i<60;i++)d[i]=(char)('0'+(n[i]>9?9:n[i]));d[60]=0;LOG("[fswatch] %s frames to %llu: %s",w,(unsigned long long)presents,d);len=0;memset(n,0,sizeof n);}frame=presents;}
          if(fs&&fs->module&&!strncmp(fs->key,w,16))n[len]++;} }
    /* draws / instances / indices a frame by fragment shader, every 60 frames (TOTK_VK_SHADERCOUNT=<n>: the n shader pairs
       with the most vertices; 1: 20) */
    { static int on=-1;if(on<0){const char *x=getenv("TOTK_VK_SHADERCOUNT");on=x?atoi(x):0;}
      if(on){static struct { const char *fs,*vs; uint64_t draws,inst,idx; } t[512];static unsigned n;static uint64_t last;
          const char *fk=fs&&fs->module?fs->key:"-";unsigned k=0;while(k<n&&(t[k].fs!=fk||t[k].vs!=vs->key))k++;
          if(k==n&&n<512){t[n].fs=fk;t[n].vs=vs->key;n++;}
          if(k<n){t[k].draws++;t[k].inst+=indexed?indexed->instances:(draw->instances>0?(uint64_t)draw->instances:1);t[k].idx+=indexed?(uint64_t)indexed->count:(uint64_t)draw->count;}
          if(presents>=last+60){if(last)for(int shown=0;shown<(on>1?on:20);shown++){unsigned b=n;for(unsigned j=0;j<n;j++)if(t[j].draws&&(b==n||t[j].idx*t[j].inst>t[b].idx*t[b].inst))b=j;if(b==n)break;
                  LOG("[shadercount] frame=%llu fs=%s vs=%s draws=%.1f instances=%.1f indices=%.0f",(unsigned long long)presents,t[b].fs,t[b].vs,t[b].draws/60.0,t[b].inst/60.0,t[b].idx/60.0);t[b].draws=0;}
              for(unsigned j=0;j<n;j++)t[j].draws=t[j].inst=t[j].idx=0;last=presents;}}}
    if(inplace_on()){static uint64_t last;if(presents>=last+600){if(last){LOG("[vulkan-inplace] a frame: %.0f uniform, %.0f storage/texel reads, %.0f storage writes, %.0f vertex, %.0f index bindings in place; %.0f fell back",inplace_ubo/600.0,inplace_ssbo/600.0,inplace_ssbo_w/600.0,inplace_vbuf/600.0,inplace_index/600.0,inplace_fallback/600.0);
        LOG("[vulkan-inplace] fell back a frame: uniform not in a zero-copy pool %.0f, dirty (a write registered over it) %.0f, kept %.0f, misaligned %.0f; storage not in a pool %.0f, misaligned %.0f; vertex not in a pool %.0f; index not in a pool %.0f, 8-bit or quads %.0f; inline uniform updates %.0f a frame (%.0f inside a pass)",
            inplace_why[IF_UBO_POOL]/600.0,inplace_why[IF_UBO_DIRTY]/600.0,inplace_why[IF_UBO_KEPT]/600.0,inplace_why[IF_UBO_ALIGN]/600.0,inplace_why[IF_SSBO_POOL]/600.0,inplace_why[IF_SSBO_ALIGN]/600.0,inplace_why[IF_VBUF_POOL]/600.0,inplace_why[IF_INDEX_POOL]/600.0,inplace_why[IF_INDEX_KIND]/600.0,inplace_updates/600.0,inplace_updates_inpass/600.0);
        LOG("[vulkan-inplace] ranges not in a zero-copy pool a frame: outside any pool %.0f, in a pool under 4 KiB %.0f, under 16 KiB %.0f, under 64 KiB %.0f, in a larger pool that is not exported %.0f, in a zero-copy pool but past its end %.0f (MB a frame: %.2f %.2f %.2f %.2f %.2f %.2f)",
            inplace_class[0]/600.0,inplace_class[1]/600.0,inplace_class[2]/600.0,inplace_class[3]/600.0,inplace_class[4]/600.0,inplace_class[5]/600.0,inplace_class_bytes[0]/600.0/1048576,inplace_class_bytes[1]/600.0/1048576,inplace_class_bytes[2]/600.0/1048576,inplace_class_bytes[3]/600.0/1048576,inplace_class_bytes[4]/600.0/1048576,inplace_class_bytes[5]/600.0/1048576);
        if(inplace_state==2)LOG("[vulkan-inplace-check] a frame: %.0f blocks checked against the submit capture at bind, %.2f differ; %.0f checked at batch completion, %.2f differ",inplace_checked/600.0,inplace_differ/600.0,inplace_late_checked/600.0,inplace_late_differ/600.0);inplace_checked=inplace_differ=inplace_late_checked=inplace_late_differ=0;
        memset(inplace_class,0,sizeof inplace_class);memset(inplace_class_bytes,0,sizeof inplace_class_bytes);
        memset(inplace_why,0,sizeof inplace_why);inplace_updates=inplace_updates_inpass=0;}last=presents;inplace_ubo=inplace_ssbo=inplace_ssbo_w=inplace_vbuf=inplace_index=inplace_fallback=0;}}
    DRAW_SEG(4);    {static uint64_t last,a,b,c,v;if(presents>=last+600){if(last)LOG("[vulkan-clear] a frame: %.1f clears deferred (%.1f of them in a view format), %.1f done by CLEAR load ops, %.1f flushed",
        (pclear_deferred-a)/600.0,(pclear_viewformat-v)/600.0,(pclear_loadops-b)/600.0,(pclear_flushed-c)/600.0);last=presents;a=pclear_deferred;b=pclear_loadops;c=pclear_flushed;v=pclear_viewformat;}}
    if(fb_count){static uint64_t last,d0,b0;fb_draws++;
        if(presents>=last+600){if(last)LOG("[vulkan-feedback] %.1f feedback draws a frame kept in their pass, %.1f in-pass barriers",(fb_draws-d0)/600.0,(fb_barriers-b0)/600.0);last=presents;d0=fb_draws;b0=fb_barriers;}}
    fg_trace_draw(vs,fs);
    fg_before_draw(vs);
    frame=draw_pass_decide(pass,&pass_key,attachments,colors,depth,views,images,na,width,height,blocks,block_load,nblocks,&continuing);
    ssbo_in_place_before_pass(frame);
    /* not expected: nothing between the decision and here ends a kept pass */
    if(continuing&&graphics_active_frame!=frame){
        static unsigned said;if(said++<8)LOG("[vulkan] 【红】a kept pass ended before its draw began it: decided again");
        frame=draw_pass_decide(pass,&pass_key,attachments,colors,depth,views,images,na,width,height,blocks,block_load,nblocks,&continuing);}
    graphics_pass_begin(frame);
    DRAW_SEG(5);    ssbo_in_place_after_pass();
    mvs_mesh=indexed?((uint64_t)indexed->ibuf*0x9E3779B97F4A7C15ull)^((uint64_t)indexed->count<<32)^(uint32_t)indexed->base_vertex:0;
    fg_after_pass();
    if(bindstats_on())bindstats_target(width,height,state.rt.n,depth.attachment!=VK_ATTACHMENT_UNUSED);
    if(bindstats_on())bindstats_draw(vs,fs,cached->pipeline,layout,sets,dynamic_offsets,dynamic_count,&viewport,&scissor);
    graphics_command_apply(__atomic_load_n(&cached->pipeline,__ATOMIC_ACQUIRE),layout,sets,dynamic_offsets,dynamic_count,&viewport,&scissor,state.blend_color.rgba,state.polygon_offset.units,state.polygon_offset.factor,state.stencil_faces);
    if(eds_on)graphics_command_eds(dyn_depth,dyn_polygon);
    for(unsigned i=0;i<16;i++)if(vertex_buffers[i])graphics_command_vertices(i,vertex_buffers[i],offsets[i]);
    {void graphics_probe_select(const char *);graphics_probe_select(fs&&fs->module?fs->key:NULL);}
    graphics_probe_draw(draw,indexed);
    unsigned drawstat_slot=(unsigned)(async_current-async_batches);int drawstat_query=async_current?drawstat_draw_begin(drawstat_slot,command,vs->key,fs?fs->key:NULL,indexed?indexed->count:draw->count,indexed?indexed->instances:(uint32_t)draw->instances,primitive):-1;
    if(indexed){if(index_count){graphics_command_index(index_buffer,index_off,index_type);vkCmdDrawIndexed(command,index_count,indexed->instances,0,indexed->base_vertex,indexed->base_instance);
        /* object motion: listed for a second draw after the G-buffer pass (nvn_vulkan_motion.inc) */
        if(mo_on())mo_record(vs,&key,dyn_depth,dyn_polygon,vertex_buffers,offsets,index_buffer,index_off,index_type,index_count,indexed,&viewport,&scissor);}}
    else if(index_buffer){if(index_count){graphics_command_index(index_buffer,0,index_type);vkCmdDrawIndexed(command,index_count,draw->instances>0?(uint32_t)draw->instances:1,0,draw->first,(uint32_t)draw->base_instance);}}
    /* TOTK: instanced DrawArrays */
    else vkCmdDraw(command,draw->count,draw->instances>0?(uint32_t)draw->instances:1,draw->first,(uint32_t)draw->base_instance);
    drawstat_draw_end(drawstat_slot,command,drawstat_query);
    /* after the decision: a later draw sampling these needs the in-pass barrier */
    for(unsigned i=0;i<nwritten;i++)written[i]->fb_dirty=1;
    forward_record_outputs(output_scope,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    DRAW_SEG(6);    graphics_probe_after_draw(fs&&fs->module?fs->key:NULL);
    draw_count++;passlog_draws++;if(!passtime_draws++)passtime_fs=fs&&fs->module?fs->key:NULL;passtime_written|=fs&&fs->module?fs->frag_out_mask:0;
    if(passlog_on())for(int st=0;st<2;st++){shader_entry *sh=st?fs:vs;if(!sh||!sh->bindings)continue;
        for(unsigned i=0;i<sh->count;i++){const shader_binding *bd=&sh->bindings[i];if(bd->kind!=3&&bd->kind!=4)continue;
            uint32_t handle=bd->kind==4?shader_handle(bd,sh->stage,0):shader_handle(bd,sh->stage,0);texture_descriptor d=descriptor_texture(handle&0xfffff);
            image_entry *ie=d.valid?image_lookup(d.texture.serial):NULL;
            PASSLOG("    %s %s binding %u type %#x handle %#x -> id %u %s serial %llu %ux%u fmt %#x target %d lv %u mem %p size %llu image-of %llu",st?"fs":"vs",bd->kind==4?"image":"texture",bd->binding,bd->type,handle,handle&0xfffff,d.valid?"":"(unregistered)",
                d.valid?(unsigned long long)d.texture.serial:0ull,d.valid?d.texture.d.width:0,d.valid?d.texture.d.height:0,d.valid?d.texture.d.format:0,d.valid?d.texture.d.target:-1,
                d.valid?d.texture.d.levels:0,d.valid?(void*)d.texture.mem:NULL,d.valid?(unsigned long long)d.texture.L.size:0ull,ie?(unsigned long long)ie->serial:0ull);}}
    /* experiment: what the pass log's UBO reads do */
    { static int ubowait=-1;if(ubowait<0)ubowait=exp_env("ubowait");
      if(ubowait&&!passlog_on())for(int st=0;st<2;st++)for(unsigned i=0;i<16;i++){const p_bind_buf *u=&state.ubo[st][i];if(u->addr&&u->size){float f[4];snapshot_read(u->addr,f,u->size<16?(uint32_t)u->size:16);}} }
    /* experiment: the pass log's vertex-data reads */
    { static int vtxwait=-1;if(vtxwait<0)vtxwait=exp_env("vtxwait");
      if(vtxwait&&!passlog_on())for(unsigned i=0;i<16;i++){va_st at=state.attributes[i];if(!at.fmt||!vs->attributes[i])continue;const NVNbufferRange *vb=&state.vertices[at.stream];float v[4];
          if(vb->address&&vb->size>at.offset)snapshot_read(vb->address+at.offset,v,16);} }
    if(passlog_on())for(int st=0;st<2;st++)for(unsigned i=0;i<16;i++){const p_bind_buf *u=&state.ubo[st][i];if(!u->addr||!u->size)continue;
        float f[4]={0};uint32_t n=u->size<16?(uint32_t)u->size:16;int bad=snapshot_read(u->addr,f,n);forward_slice sl;forward_result fr=forward_find(u->addr,u->size<65536?u->size:65536,4,NULL,&sl);
        PASSLOG("    %s ubo%u addr %#llx size %llu forward=%d %s %g %g %g %g",st?"fs":"vs",i,(unsigned long long)u->addr,(unsigned long long)u->size,(int)fr,bad?"(unread)":"",f[0],f[1],f[2],f[3]);}
    if(passlog_on()){const p_bind_buf *u=&state.ubo[0][0];float f[8]={0};int bad=u->addr&&u->size?snapshot_read(u->addr,f,u->size<32?u->size:32):-1;
        PASSLOG("    vs ubo0 addr %#llx size %llu %s: %g %g %g %g %g %g %g %g",(unsigned long long)u->addr,(unsigned long long)u->size,bad?"(unread)":"",f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]);
        for(unsigned i=0;i<16;i++){va_st at=state.attributes[i];if(!at.fmt||!vs->attributes[i])continue;const NVNbufferRange *vb=&state.vertices[at.stream];float v[4]={0};
            int vbad=vb->address&&vb->size>at.offset?snapshot_read(vb->address+at.offset,v,16):-1;
            PASSLOG("    attr %u fmt %#x stream %u offset %u stride %u buffer %#llx size %llu %s: %g %g %g %g",i,at.fmt,at.stream,at.offset,state.streams[at.stream].stride,(unsigned long long)vb->address,(unsigned long long)vb->size,vbad?"(unread)":"",v[0],v[1],v[2],v[3]);}}
    PASSLOG("  draw %s/%s %s count %d instances %d vp %g,%g %gx%g scissor %d,%d %dx%d blend %#x mask %#x",vs->key,fs->module?fs->key:"-",indexed?"indexed":"arrays",indexed?indexed->count:draw->count,indexed?indexed->instances:draw->instances,viewport.x,viewport.y,viewport.width,viewport.height,x,y,sw,sh,(unsigned)state.color.blend_en,(unsigned)state.mask.mask);
    /* Storage writes keep the pass: GPU readers of what they wrote barrier themselves (forward_read_barrier, forward_seed,
       image_access), and the host reads only after the batch's fence, so one shader-write -> host barrier at the commit
       (mtl_sub_commit) covers them all. Ending the pass after every such draw would split the open world's G-buffer into
       ~45 one-draw passes a frame, each loading and storing 5-6 attachments. TOTK_VK_EXP=writepassend ends it. */
    if(graphics_shader_writes(vs)||graphics_shader_writes(fs)){
        if(inplace_on())inplace_pass_writes=1;
        static int split=-1;if(split<0)split=exp_env("writepassend");
        if(split)graphics_pass_end();
        if(shader_host_writes(vs)||shader_host_writes(fs))graphics_host_writes=1;
    }
    batch_draws++;
    if (draw_count <= 12 && getenv("DM_VK_TRACE")) LOG("[vulkan] draw=%llu %s/%s rt=%ux%u vp=%g,%g %gx%g sc=%d,%d %dx%d mask=%#x cull=%u front=%u samples=%llu", (unsigned long long)draw_count, vs->key, fs->key, width, height, viewport.x, viewport.y, viewport.width, viewport.height, x, y, sw, sh, state.mask.mask, (unsigned)state.polygon.cull, (unsigned)state.polygon.front, (unsigned long long)samples_passed);
}
#define PLAN_PART2
/* part 2: the plan builder and the checks */
#include "nvn_vulkan_plan.inc"
#undef PLAN_PART2
#include "nvn_vulkan_warm.inc"
#include "nvn_vulkan_prepare.inc"
#include "nvn_vulkan_prefetch.inc"
