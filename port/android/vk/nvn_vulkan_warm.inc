// PSO warm-up: record genuinely created graphics pipeline descriptions, rebuild them in the background on the next
// launch, moving driver compilation away from first in-game encounters to start-up / the loading screen.
// Same design as the Metal backend (nvn_mtl.m warm_worker): a user manifest accumulates locally, a seed manifest
// ships with the build; manifests describe pipeline state only.
// User manifest: $DM_VK_PIPELINE_CACHE_DIR/pso_vk_user.tsv (DM_PSO_USER=off stops recording). Seed manifest:
// $DM_VK_SHADERS/pso_vk_manifest.tsv. DM_PSO_MANIFEST=off turns the warm-up off; DM_PSO_MANIFEST=<path> loads only that
// manifest.
// Line format: 'R\t<vs16hex>\t<fs16hex|->\t<pass_hex>\t<key_hex>'; the header verifies the version and both struct sizes.
#define PSO_MANIFEST_LIMIT (8u<<20)
/* a full 8 MB manifest is ~7,500 records */
#define PSO_RECORD_MAX 8192
#define PSO_USER_NAME "pso_vk_user.tsv"
#define PSO_SEED_NAME "pso_vk_manifest.tsv"
/* hash: of its manifest line */
typedef struct { char kind;char vs[17],fs[17];graphics_pass_key pass;pipeline_key key;uint64_t hash; } pso_record;
static pthread_mutex_t pso_manifest_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int pso_warm_running;
/* line hashes of the records the warm-up built, sorted; set before pso_warm_running drops */
static uint64_t *pso_built;static size_t pso_built_count;
static pso_record *pso_records;
static size_t pso_record_count;
static uint64_t *pso_seen;
static size_t pso_seen_capacity,pso_seen_count;
static uint64_t pso_user_bytes;
static int pso_user_fd=-1,pso_user_checked;
static int pso_hex_digit(char c) {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static void pso_hex_encode(const void *data,size_t size,char *text) {
    static const char digits[]="0123456789abcdef";const uint8_t *bytes=data;
    for(size_t i=0;i<size;i++){text[2*i]=digits[bytes[i]>>4];text[2*i+1]=digits[bytes[i]&15];}
}
static int pso_hex_decode(const char *text,size_t length,void *data,size_t size) {
    if(length!=2*size)return 0;uint8_t *bytes=data;
    for(size_t i=0;i<size;i++){int high=pso_hex_digit(text[2*i]),low=pso_hex_digit(text[2*i+1]);if(high<0||low<0)return 0;bytes[i]=(uint8_t)((high<<4)|low);}
    return 1;
}
static uint64_t pso_line_hash(const char *line,size_t size) {uint64_t hash=graphics_hash(line,size);return hash?hash:1;}
static void pso_seen_reserve(size_t capacity) {
    if(capacity<=pso_seen_capacity)return;
    uint64_t *table=calloc(capacity,sizeof *table);if(!table)die("Vulkan PSO dedup allocation");
    size_t previous=pso_seen_capacity;uint64_t *old=pso_seen;
    pso_seen=table;pso_seen_capacity=capacity;
    for(size_t i=0;i<previous;i++){uint64_t value=old[i];if(!value)continue;size_t mask=capacity-1,at=(size_t)(value^(value>>32))&mask;while(table[at])at=(at+1)&mask;table[at]=value;}
    free(old);
}
static int pso_seen_add(uint64_t hash) {
    if((pso_seen_count+1)*2>pso_seen_capacity)pso_seen_reserve(pso_seen_capacity?pso_seen_capacity*2:4096);
    size_t mask=pso_seen_capacity-1,at=(size_t)(hash^(hash>>32))&mask;
    while(pso_seen[at]){if(pso_seen[at]==hash)return 0;at=(at+1)&mask;}
    pso_seen[at]=hash;pso_seen_count++;return 1;
}
// Zero struct padding when recording so manifest lines depend on field values only and dedup stays stable across sessions.
static void pso_key_normalize(pipeline_key *out,const pipeline_key *in) {
    memset(out,0,sizeof *out);
    memcpy(out->formats,in->formats,sizeof in->formats);
    out->colors=in->colors;out->primitive=in->primitive;out->attachments=in->attachments;out->depth_attachment=in->depth_attachment;
    memcpy(out->color_indices,in->color_indices,sizeof in->color_indices);
    out->required_subgroup_size=in->required_subgroup_size;
    memcpy(out->attributes,in->attributes,sizeof in->attributes);
    memcpy(out->streams,in->streams,sizeof in->streams);
    out->color=in->color;out->mask=in->mask;
    memcpy(out->blend,in->blend,sizeof in->blend);
    out->polygon=in->polygon;out->depth=in->depth;out->multisample=in->multisample;
    /* feedback loop, raster order (not the bisection generations) */
    out->depth_clamp=in->depth_clamp;out->raster_discard=in->raster_discard;out->pad_[1]=in->pad_[1]&0xc0;
}
static int pso_user_file(void) {
    if(pso_user_checked)return pso_user_fd;
    pso_user_checked=1;
    const char *disable=getenv("DM_PSO_USER");if(disable&&!strcmp(disable,"off"))return -1;
    if(pipeline_disk_dir<0)return -1;
    int fd=openat(pipeline_disk_dir,PSO_USER_NAME,O_RDWR|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){LOG("[vulkan-pso] user_manifest=disabled errno=%d",errno);return -1;}
    struct stat st;
    if(fstat(fd,&st)){close(fd);return -1;}
    pso_user_bytes=(uint64_t)st.st_size;
    if(!st.st_size){
        char header[80];int n=snprintf(header,sizeof header,"# DMVKRPSO1 pass=%zu key=%zu\n",sizeof(graphics_pass_key),sizeof(pipeline_key));
        if(n<=0||(size_t)n>=sizeof header||!pipeline_disk_transfer(fd,header,(size_t)n,1)){close(fd);return -1;}
        pso_user_bytes=(uint64_t)n;
    }
    pso_user_fd=fd;return fd;
}
/* A graphics record's manifest line, newline included; its length, or 0. */
static int pso_graphics_line(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key,char *line,size_t size) {
    pipeline_key normalized;pso_key_normalize(&normalized,key);
    int n=snprintf(line,size,"R\t%s\t%s\t",vs->key,fs->module?fs->key:"-");
    if(n<=0||(size_t)n+2*sizeof *pass+1+2*sizeof normalized+1>=size)return 0;
    pso_hex_encode(pass,sizeof *pass,line+n);n+=2*(int)sizeof *pass;line[n++]='\t';
    pso_hex_encode(&normalized,sizeof normalized,line+n);n+=2*(int)sizeof normalized;line[n++]='\n';
    return n;
}
static int pso_hash_cmp(const void *a,const void *b) {uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;}
/* Whether the warm-up has built this pipeline this session: its compile is then in the driver's pipeline cache, and
   creating it at its first draw is a lookup (well under a millisecond), so the draw needn't be skipped for the
   background build (nvn_vulkan_draw.inc). Skipping it there makes every element flicker once the first time it is
   drawn, even with the whole start-up warmed: the title fade, the logo, the save pictures, the place name. */
static int pso_warmed(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key) {
    if(atomic_load_explicit(&pso_warm_running,memory_order_acquire)||!pso_built_count)return 0;
    char line[2048];int n=pso_graphics_line(vs,fs,pass,key,line,sizeof line);if(n<=0)return 0;
    uint64_t hash=pso_line_hash(line,(size_t)n-1);
    return bsearch(&hash,pso_built,pso_built_count,sizeof hash,pso_hash_cmp)!=NULL;
}
static void pso_record_graphics(const shader_entry *vs,const shader_entry *fs,const graphics_pass_key *pass,const pipeline_key *key) {
    int fd=pso_user_file();if(fd<0||pso_user_bytes>=PSO_MANIFEST_LIMIT)return;
    char line[2048];int n=pso_graphics_line(vs,fs,pass,key,line,sizeof line);if(n<=0)return;
    const char *fragment=fs->module?fs->key:"-";uint64_t hash=pso_line_hash(line,(size_t)n-1);
    pthread_mutex_lock(&pso_manifest_mutex);
    if(pso_seen_add(hash)&&pso_user_bytes+(uint64_t)n<=PSO_MANIFEST_LIMIT){
        if(pipeline_disk_transfer(fd,line,(size_t)n,1)){pso_user_bytes+=(uint64_t)n;LOG("[vulkan-pso] record=R vs=%s fs=%s records=%zu",vs->key,fragment,pso_seen_count);}
        else LOG("[vulkan-pso] record_write_failed errno=%d",errno);
    }
    pthread_mutex_unlock(&pso_manifest_mutex);
}
/* Compute pipelines too ('C\t<shader key>'): rebuilt while the next session loads, as graphics ones. */
static void pso_record_compute(const shader_entry *cs) {
    int fd=pso_user_file();if(fd<0||pso_user_bytes>=PSO_MANIFEST_LIMIT)return;
    char line[64];int n=snprintf(line,sizeof line,"C\t%s\n",cs->key);if(n<=0||(size_t)n>=sizeof line)return;
    uint64_t hash=pso_line_hash(line,(size_t)n-1);
    pthread_mutex_lock(&pso_manifest_mutex);
    if(pso_seen_add(hash)&&pso_user_bytes+(uint64_t)n<=PSO_MANIFEST_LIMIT){
        if(pipeline_disk_transfer(fd,line,(size_t)n,1)){pso_user_bytes+=(uint64_t)n;LOG("[vulkan-pso] record=C cs=%s records=%zu",cs->key,pso_seen_count);}
        else LOG("[vulkan-pso] record_write_failed errno=%d",errno);
    }
    pthread_mutex_unlock(&pso_manifest_mutex);
}
static void pso_decode_c(char *line,size_t length) {
    if(pso_record_count>=PSO_RECORD_MAX||length!=18)return;
    pso_record *record=&pso_records[pso_record_count];memset(record,0,sizeof *record);record->kind='C';
    memcpy(record->vs,line+2,16);for(int i=0;i<16;i++)if(pso_hex_digit(record->vs[i])<0)return;
    pso_record_count++;
}
static void pso_decode_r(char *line,size_t length) {
    if(pso_record_count>=PSO_RECORD_MAX)return;
    line[length]=0;
    char *vs=line+2;char *t1=strchr(vs,'\t');if(!t1)return;*t1=0;
    char *fs=t1+1;char *t2=strchr(fs,'\t');if(!t2)return;*t2=0;
    char *pass_hex=t2+1;char *t3=strchr(pass_hex,'\t');if(!t3)return;*t3=0;
    char *key_hex=t3+1;
    size_t vs_length=strlen(vs),fs_length=strlen(fs),pass_length=strlen(pass_hex),key_length=strlen(key_hex);
    if(vs_length!=16||!(fs_length==16||(fs_length==1&&fs[0]=='-')))return;
    if(pass_length!=2*sizeof(graphics_pass_key)||key_length!=2*sizeof(pipeline_key))return;
    pso_record *record=&pso_records[pso_record_count];
    memset(record,0,sizeof *record);record->kind='R';
    memcpy(record->vs,vs,16);memcpy(record->fs,fs,fs_length);
    if(!pso_hex_decode(pass_hex,pass_length,&record->pass,sizeof record->pass))return;
    if(!pso_hex_decode(key_hex,key_length,&record->key,sizeof record->key))return;
    record->key.vs=NULL;record->key.fs=NULL;
    pso_record_count++;
}
static void pso_manifest_parse_fd(int fd,const char *name,int collect) {
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0)return;
    if(st.st_size>(off_t)PSO_MANIFEST_LIMIT){LOG("[vulkan-pso] manifest_oversize name=%s bytes=%lld",name,(long long)st.st_size);return;}
    size_t size=(size_t)st.st_size;char *data=malloc(size+1);
    if(!data)return;
    if(!pipeline_disk_transfer(fd,data,size,0)){free(data);return;}
    data[size]=0;
    char *cursor=data,*end=data+size;size_t before=pso_record_count;
    if(cursor<end&&*cursor=='#'){
        char *newline=memchr(cursor,'\n',(size_t)(end-cursor));
        size_t pass_size=0,key_size=0;
        if(sscanf(cursor,"# DMVKRPSO1 pass=%zu key=%zu",&pass_size,&key_size)!=2||pass_size!=sizeof(graphics_pass_key)||key_size!=sizeof(pipeline_key)){
            LOG("[vulkan-pso] manifest_skipped name=%s reason=header",name);free(data);return;}
        cursor=newline?newline+1:end;
    }
    while(cursor<end){
        char *newline=memchr(cursor,'\n',(size_t)(end-cursor));
        char *line=cursor;size_t length=newline?(size_t)(newline-cursor):(size_t)(end-cursor);
        cursor=newline?newline+1:end;
        while(length&&line[length-1]=='\r')length--;
        if(!length||line[0]=='#')continue;
        uint64_t hash=pso_line_hash(line,length);
        if(!pso_seen_add(hash))continue;
        size_t at=pso_record_count;
        if(collect&&length>2&&line[0]=='R'&&line[1]=='\t')pso_decode_r(line,length);
        else if(collect&&length>2&&line[0]=='C'&&line[1]=='\t')pso_decode_c(line,length);
        if(pso_record_count>at)pso_records[at].hash=hash;
    }
    LOG("[vulkan-pso] manifest name=%s records=%zu total=%zu",name,pso_record_count-before,pso_record_count);
    free(data);
}
/* The user manifest only grows, and a new translator revision renames every shader: records of shaders gone from the
   cache can never be built, yet they fill the warm-up's record limit and the file's 8 MB, so the warm-up builds
   nothing and new pipelines are not recorded (every first draw skipped by the async build: one-frame flickers at
   start). Before the file is opened for appending, it is rewritten without records whose shaders (<key>.meta) are in
   none of the shader caches, or emptied if its header is from other struct sizes. Every cache counts (the folders
   named shader-cache* beside DM_VK_SHADERS: one per GPU driver), so trying another driver keeps this one's records. */
enum { PSO_SHADER_DIRS = 16 };
static int pso_shader_present(char dirs[][1024],int n,const char *key,size_t length) {
    if(length!=16)return 0;char path[1100];
    for(int i=0;i<n;i++){snprintf(path,sizeof path,"%s/%.16s.meta",dirs[i],key);if(!access(path,F_OK))return 1;}
    return 0;
}
static void pso_user_compact(void) {
    const char *dir=getenv("DM_VK_SHADERS");if(!dir||!*dir||pipeline_disk_dir<0)return;
    static char dirs[PSO_SHADER_DIRS][1024];int ndirs=0;snprintf(dirs[ndirs++],sizeof dirs[0],"%s",dir);
    { char parent[1024];snprintf(parent,sizeof parent,"%s",dir);char *slash=strrchr(parent,'/');
      if(slash){*slash=0;const char *own=slash+1;DIR *p=opendir(parent);struct dirent *e;
          while(p&&(e=readdir(p))&&ndirs<PSO_SHADER_DIRS)if(!strncmp(e->d_name,"shader-cache",12)&&strcmp(e->d_name,own))snprintf(dirs[ndirs++],sizeof dirs[0],"%s/%s",parent,e->d_name);
          if(p)closedir(p);} }
    const char *disable=getenv("DM_PSO_USER");if(disable&&!strcmp(disable,"off"))return;
    int fd=openat(pipeline_disk_dir,PSO_USER_NAME,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>(off_t)(2*PSO_MANIFEST_LIMIT)){close(fd);return;}
    size_t size=(size_t)st.st_size;char *data=malloc(size+1),*kept=malloc(size+1);
    if(!data||!kept||!pipeline_disk_transfer(fd,data,size,0)){close(fd);free(data);free(kept);return;}
    close(fd);data[size]=0;
    size_t pass_size=0,key_size=0,out=0,lines=0,dropped=0;char *cursor=data,*end=data+size;
    int header=sscanf(data,"# DMVKRPSO1 pass=%zu key=%zu",&pass_size,&key_size)==2&&pass_size==sizeof(graphics_pass_key)&&key_size==sizeof(pipeline_key);
    while(header&&cursor<end){
        char *newline=memchr(cursor,'\n',(size_t)(end-cursor));size_t length=newline?(size_t)(newline-cursor):(size_t)(end-cursor);
        char *line=cursor;cursor=newline?newline+1:end;
        int keep=line[0]=='#';
        if(!keep&&length>2&&line[1]=='\t'&&(line[0]=='R'||line[0]=='C')){lines++;
            char *fs=memchr(line+2,'\t',length-2);
            keep=pso_shader_present(dirs,ndirs,line+2,fs?(size_t)(fs-line-2):length-2);
            if(keep&&line[0]=='R'){char *fs_end=memchr(fs+1,'\t',(size_t)(line+length-fs-1));
                keep=fs_end&&((fs_end-fs==2&&fs[1]=='-')||pso_shader_present(dirs,ndirs,fs+1,(size_t)(fs_end-fs-1)));}}
        if(keep){memcpy(kept+out,line,length);out+=length;kept[out++]='\n';}else if(line[0]!='#')dropped++;
    }
    /* other struct sizes: nothing in it can be read; start over */
    if(!header){out=0;dropped=lines=1;}
    if(dropped){
        char temporary[128];snprintf(temporary,sizeof temporary,".%s.%ld.tmp",PSO_USER_NAME,(long)getpid());
        int tfd=openat(pipeline_disk_dir,temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        int ok=tfd>=0&&pipeline_disk_transfer(tfd,kept,out,1)&&!fsync(tfd);if(tfd>=0)close(tfd);
        if(ok&&renameat(pipeline_disk_dir,temporary,pipeline_disk_dir,PSO_USER_NAME))ok=0;
        if(!ok)unlinkat(pipeline_disk_dir,temporary,0);
        LOG("[vulkan-pso] user manifest compacted: %s, %zu of %zu records dropped (shaders no longer in the cache), %zu -> %zu bytes",
            ok?"rewritten":"rewrite failed",dropped,lines,size,out);
    }
    free(data);free(kept);
}
static void pso_manifest_parse_path(const char *path,int collect) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return;
    pso_manifest_parse_fd(fd,path,collect);
    close(fd);
}
static int pso_warm_compute(const pso_record *record) {
    shader_entry *cs=shader_load_by_key(record->vs,0,0);if(!cs||cs->stage!=5||!cs->module||!cs->layout)return 0;
    /* TOTK_VK_BINDLESS: the global arrays are set 0 */
    VkDescriptorSetLayout sets[2]={bl_layout,cs->layout};
    VkPipelineLayout layout;VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=bl_set?2:1,.pSetLayouts=bl_set?sets:&cs->layout};
    if(vkCreatePipelineLayout(device,&li,NULL,&layout)!=VK_SUCCESS)return 0;
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=cs->module,.pName="main"},.layout=layout};
    int ok=1;VkPipeline pipeline=VK_NULL_HANDLE;
#ifdef __ANDROID__
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required;
    if(!dm_vk_driver_prepare_shader_stage(&vk_api,&ci.stage,&required))ok=0;
#endif
    /* stays in the driver cache */
    if(ok&&pipeline_create_compute(&ci,&pipeline)==VK_SUCCESS)vkDestroyPipeline(device,pipeline,NULL);else ok=0;
    vkDestroyPipelineLayout(device,layout,NULL);
    return ok;
}
static int pso_warm_one(const pso_record *record) {
    if(record->kind=='C')return pso_warm_compute(record);
    // Manifests may come from this device or from a shipped seed; the warm path must skip anything suspicious without side effects.
    const graphics_pass_key *pass_key=&record->pass;
    if(pass_key->attachments<1||pass_key->attachments>9||pass_key->colors>8)return 0;
    if(pass_key->depth!=VK_ATTACHMENT_UNUSED&&pass_key->depth>=pass_key->attachments)return 0;
    for(unsigned i=0;i<pass_key->attachments;i++)if(pass_key->load[i]>VK_ATTACHMENT_LOAD_OP_DONT_CARE||pass_key->stencil_load[i]>VK_ATTACHMENT_LOAD_OP_DONT_CARE)return 0;
    for(unsigned i=0;i<pass_key->colors;i++)if(pass_key->color_indices[i]!=VK_ATTACHMENT_UNUSED&&pass_key->color_indices[i]>=pass_key->attachments)return 0;
    shader_entry *vs=shader_load_by_key(record->vs,0,0);if(!vs)return 0;
    shader_entry *fs;
    if(record->fs[0]=='-'&&!record->fs[1])fs=graphics_empty_shader();
    else{fs=shader_load_by_key(record->fs,0,0);if(!fs)return 0;}
    // The same pass/layout caches as the runtime: warmed objects are exactly what the game thread hits first.
    VkAttachmentDescription attachments[9]={0};VkAttachmentReference colors[8]={0},depth={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
    for(unsigned i=0;i<pass_key->attachments;i++)attachments[i]=(VkAttachmentDescription){.format=pass_key->formats[i],.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=pass_key->load[i],.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=pass_key->stencil_load[i],.stencilStoreOp=i==pass_key->depth?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    for(unsigned i=0;i<pass_key->colors;i++)colors[i]=(VkAttachmentReference){pass_key->color_indices[i],VK_IMAGE_LAYOUT_GENERAL};
    if(pass_key->depth!=VK_ATTACHMENT_UNUSED)depth.attachment=pass_key->depth;
    graphics_pass_entry *pass=graphics_pass_get(*pass_key,attachments,colors,depth,0);
    // The driver rejected this pass (usually an attachment format in the record that is illegal on this device/driver):
    // skip that record without interrupting the whole warm-up.
    if(!pass)return 0;
    VkPipelineLayout layout=graphics_layout_get(vs,fs);
    VkPipeline pipeline=VK_NULL_HANDLE;
    uint64_t begin=dm_android_clock_ns(CLOCK_MONOTONIC);
    VkResult result=graphics_pipeline_create(&record->key,vs,fs,pass,layout,&pipeline,0);
    uint64_t wall=dm_android_clock_ns(CLOCK_MONOTONIC)-begin;
    if(result!=VK_SUCCESS)return 0;
    if(wall>50000000)LOG("[vulkan-pso-warm] slow vs=%s fs=%s wall_ms=%.1f",record->vs,record->fs,wall/1e6);
    // Create then destroy: the compilation stays in the driver pipeline cache, so the game's first use is a cache hit, not a compile.
    vkDestroyPipeline(device,pipeline,NULL);
    return 1;
}
/* After the pipeline list, every translated shader in the cache directory is loaded (reflection, module, descriptor
   layout) while the session loads: a shader the game first uses later is then a lookup, not 1-10 ms of file reads
   and parsing on the translation thread. Variants translated ahead of time on the PC that this device has not used
   yet (<key>.aot, tools/aot_shaders.py) are left on disk: loading all ~20,000 would hold memory for shaders a
   session never meets. */
static void pso_preload_shaders(void) {
    const char *dir=getenv("DM_VK_SHADERS");if(!dir)return;
    DIR *d=opendir(dir);if(!d)return;
    uint64_t begin=dm_android_clock_ns(CLOCK_MONOTONIC),loaded=0,seen=0,aot_waiting=0;struct dirent *f;
    while((f=readdir(d))){size_t n=strlen(f->d_name);if(n!=21||strcmp(f->d_name+16,".meta"))continue;
        char key[17];memcpy(key,f->d_name,16);key[16]=0;int hex=1;for(int i=0;i<16;i++)hex&=pso_hex_digit(key[i])>=0;if(!hex)continue;
        char aot[1100];snprintf(aot,sizeof aot,"%s/%s.aot",dir,key);if(!access(aot,F_OK)){aot_waiting++;continue;}
        seen++;if(shader_load_by_key(key,0,0))loaded++;}
    closedir(d);
    LOG("[vulkan-pso-warm] preloaded shaders=%llu of %llu (%llu made ahead of time, not used yet) wall_ms=%.1f",(unsigned long long)loaded,(unsigned long long)seen,(unsigned long long)aot_waiting,(dm_android_clock_ns(CLOCK_MONOTONIC)-begin)/1e6);
}
static void *pso_warm_main(void *unused) {
    (void)unused;pthread_setname_np(pthread_self(),"D2R PSO warm");
    // warm creations never account (long-frame totals and slow-create details cover the game thread only)
    resource_create_quiet=1;
    uint64_t begin=dm_android_clock_ns(CLOCK_MONOTONIC),built=0,skipped=0;
    uint64_t *hashes=malloc(pso_record_count*sizeof *hashes);
    /* how long each creation takes: a pipeline the driver's cache serves is well under 2 ms, a compile is 10 ms and
       more */
    uint64_t fast=0,slow=0,slow_ns=0,fast_ns=0;
    for(size_t i=0;i<pso_record_count;i++){
        uint64_t t0=dm_android_clock_ns(CLOCK_MONOTONIC);int ok=pso_warm_one(&pso_records[i]);uint64_t dt=dm_android_clock_ns(CLOCK_MONOTONIC)-t0;
        if(ok){if(hashes&&pso_records[i].kind=='R')hashes[pso_built_count++]=pso_records[i].hash;built++;if(dt<2000000){fast++;fast_ns+=dt;}else{slow++;slow_ns+=dt;}}else skipped++;
        if((i&15)==15)LOG("[vulkan-pso-warm] progress=%zu/%zu built=%llu skipped=%llu",i+1,pso_record_count,(unsigned long long)built,(unsigned long long)skipped);
    }
    LOG("[vulkan-pso-warm] done records=%zu built=%llu skipped=%llu wall_ms=%.1f; creations under 2 ms: %llu (%.1f ms in all), 2 ms and over: %llu (%.1f ms in all, %.1f ms each)",pso_record_count,(unsigned long long)built,(unsigned long long)skipped,(dm_android_clock_ns(CLOCK_MONOTONIC)-begin)/1e6,
        (unsigned long long)fast,fast_ns/1e6,(unsigned long long)slow,slow_ns/1e6,slow?slow_ns/1e6/slow:0.0);
    free(pso_records);pso_records=NULL;pso_record_count=0;
    if(hashes)qsort(hashes,pso_built_count,sizeof *hashes,pso_hash_cmp);else pso_built_count=0;
    pso_built=hashes;
    /* save the driver cache at once: during the build it never settles long enough for the timed save */
    pipeline_disk_request();
    pso_preload_shaders();
    atomic_store_explicit(&pso_warm_running,0,memory_order_release);
    /* the shader libraries after the pipelines, when asked for (nvn_vulkan_gpl.inc) */
    gpl_warm_start(0);
    return NULL;
}
// Called at the end of initialize(): read manifests, build the dedup set, start the background warm thread; idle when the local cache directory and explicit manifest are both absent.
static void prepare_check(void);
static void pso_warm_start(void) {
    prepare_check();
    const char *setting=getenv("DM_PSO_MANIFEST");
    int explicit_path=setting&&*setting&&strcmp(setting,"off");
    if(pipeline_disk_dir<0&&!explicit_path)return;
    int collect=!(setting&&!strcmp(setting,"off"));
    if(collect){pso_records=calloc(PSO_RECORD_MAX,sizeof *pso_records);if(!pso_records){LOG("[vulkan-pso-warm] allocation_failed records=%d",PSO_RECORD_MAX);collect=0;}}
    const char *shaders=getenv("DM_VK_SHADERS");
    char seed[1024];seed[0]=0;
    if(shaders&&*shaders)snprintf(seed,sizeof seed,"%s/%s",shaders,PSO_SEED_NAME);
    pthread_mutex_lock(&pso_manifest_mutex);
    if(explicit_path)pso_manifest_parse_path(setting,collect);
    else{
        // Local accumulation first (closer to actual use order), then the shipped seed; a line counts once.
        pso_user_compact();pso_user_file();
        if(pipeline_disk_dir>=0){int fd=openat(pipeline_disk_dir,PSO_USER_NAME,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd>=0){pso_manifest_parse_fd(fd,PSO_USER_NAME,collect);close(fd);}}
        if(seed[0])pso_manifest_parse_path(seed,collect);
    }
    pthread_mutex_unlock(&pso_manifest_mutex);
    /* nothing to warm: the shader libraries at once, when asked for */
    if(!collect||!pso_record_count){free(pso_records);pso_records=NULL;gpl_warm_start(0);return;}
    LOG("[vulkan-pso-warm] start records=%zu seed=%s user_dir=%s",pso_record_count,seed[0]?seed:"(none)",getenv("DM_VK_PIPELINE_CACHE_DIR")?getenv("DM_VK_PIPELINE_CACHE_DIR"):"(none)");
    atomic_store(&pso_warm_running,1);
    pthread_t thread;
    int error=pthread_create(&thread,NULL,pso_warm_main,NULL);
    if(error){atomic_store(&pso_warm_running,0);free(pso_records);pso_records=NULL;pso_record_count=0;LOG("[vulkan-pso-warm] worker_create_failed=%d",error);return;}
    pthread_detach(thread);
}
