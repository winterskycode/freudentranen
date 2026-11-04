// 驱动编译缓存：独立锁保护Vulkan访问，后台文件IO从不持queue_lock。
// Driver compilation cache: a dedicated mutex protects Vulkan access; background file I/O never holds queue_lock.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
/* The driver cache outgrew 64 MiB (57 MiB loaded, more after a session): vkGetPipelineCacheData's size over the limit made
   every save "skipped", so pipelines compiled after the file reached that size were never kept, and the warm-up compiled
   2,244 of them again at every launch, 35 ms each, 79 s (2026-10-31). 512 MiB; the file is read once at start. */
#define PIPELINE_DISK_LIMIT (512u<<20)
#define PIPELINE_DISK_HEADER 80u
static pthread_mutex_t pipeline_disk_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pipeline_disk_join_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pipeline_disk_condition=PTHREAD_COND_INITIALIZER;
static pthread_t pipeline_disk_thread;
static int pipeline_disk_dir=-1,pipeline_disk_lock=-1,pipeline_disk_started,pipeline_disk_stop,pipeline_disk_requested,pipeline_disk_attempted;
static uint8_t pipeline_disk_identity[32];
static VkPhysicalDeviceProperties pipeline_disk_properties;
static char pipeline_disk_name[80];
static _Atomic uint64_t pipeline_cache_generation,pipeline_cache_saved,pipeline_cache_changed;
static uint64_t pipeline_disk_attempt;
/* The driver's cache size at the last write (or just after loading). Most creations are hits that add nothing: a 33 s run
   rewrote 294 MB three times for 14 KB of new data (2026-11-04), so a save first asks for the size. */
static size_t pipeline_disk_bytes;
static _Atomic uint64_t pipeline_disk_written;
static uint32_t pipeline_disk_u32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t pipeline_disk_u64(const uint8_t *p) {return pipeline_disk_u32(p)|(uint64_t)pipeline_disk_u32(p+4)<<32;}
static void pipeline_disk_put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void pipeline_disk_put64(uint8_t *p,uint64_t n) {pipeline_disk_put32(p,(uint32_t)n);pipeline_disk_put32(p+4,(uint32_t)(n>>32));}
static void pipeline_disk_digest(const void *data,size_t size,uint8_t result[32]) {
    dm_android_sha256_context ctx;dm_android_sha256_init(&ctx);dm_android_sha256_update(&ctx,data,(uint32_t)size);dm_android_sha256_final(result,&ctx);
}
static int pipeline_disk_blob_valid(const uint8_t *data,size_t size) {
    return size>=32&&size<=PIPELINE_DISK_LIMIT&&pipeline_disk_u32(data)==32&&pipeline_disk_u32(data+4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE&&
        pipeline_disk_u32(data+8)==pipeline_disk_properties.vendorID&&pipeline_disk_u32(data+12)==pipeline_disk_properties.deviceID&&
        !memcmp(data+16,pipeline_disk_properties.pipelineCacheUUID,VK_UUID_SIZE);
}
static int pipeline_disk_transfer(int fd,void *data,size_t size,int writing) {
    uint8_t *p=data;while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return 0;p+=n;size-=(size_t)n;}return 1;
}
static void *pipeline_disk_load(size_t *size) {
    *size=0;if(pipeline_disk_dir<0)return NULL;
    int fd=openat(pipeline_disk_dir,pipeline_disk_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){LOG("[vulkan-pipeline-cache] load=empty reason=%s",errno==ENOENT?"missing":"open_failed");return NULL;}
    struct stat st;uint8_t header[PIPELINE_DISK_HEADER],digest[32];void *blob=NULL;const char *reason="invalid_file";
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<(off_t)(PIPELINE_DISK_HEADER+32)||st.st_size>(off_t)(PIPELINE_DISK_HEADER+PIPELINE_DISK_LIMIT))goto done;
    if(!pipeline_disk_transfer(fd,header,sizeof header,0))goto done;
    if(memcmp(header,"DMVKPC01",8)||memcmp(header+8,pipeline_disk_identity,32)){reason="identity_mismatch";goto done;}
    uint64_t length=pipeline_disk_u64(header+40);
    if(length<32||length>PIPELINE_DISK_LIMIT||length!=(uint64_t)st.st_size-PIPELINE_DISK_HEADER){reason="invalid_length";goto done;}
    blob=malloc((size_t)length);if(!blob){reason="allocation_failed";goto done;}
    if(!pipeline_disk_transfer(fd,blob,(size_t)length,0)||!pipeline_disk_blob_valid(blob,(size_t)length)){reason="invalid_blob";goto done;}
    pipeline_disk_digest(blob,(size_t)length,digest);if(memcmp(digest,header+48,32)){reason="digest_mismatch";goto done;}
    *size=(size_t)length;reason=NULL;
done:
    close(fd);if(reason){free(blob);blob=NULL;LOG("[vulkan-pipeline-cache] load=empty reason=%s",reason);}else LOG("[vulkan-pipeline-cache] load=accepted bytes=%zu namespace=%s",*size,pipeline_disk_name);
    return blob;
}
static int pipeline_disk_write(const uint8_t *blob,size_t size) {
    uint8_t header[PIPELINE_DISK_HEADER]={0};memcpy(header,"DMVKPC01",8);memcpy(header+8,pipeline_disk_identity,32);pipeline_disk_put64(header+40,size);pipeline_disk_digest(blob,size,header+48);
    char temporary[128];int n=snprintf(temporary,sizeof temporary,".%s.%ld.%llu.tmp",pipeline_disk_name,(long)getpid(),(unsigned long long)dm_android_clock_ns(CLOCK_MONOTONIC));
    if(n<0||(size_t)n>=sizeof temporary)return 0;
    int fd=openat(pipeline_disk_dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return 0;
    int ok=pipeline_disk_transfer(fd,header,sizeof header,1)&&pipeline_disk_transfer(fd,(void*)blob,size,1)&&!fsync(fd);
    if(close(fd))ok=0;
    if(ok&&renameat(pipeline_disk_dir,temporary,pipeline_disk_dir,pipeline_disk_name))ok=0;
    if(ok&&fsync(pipeline_disk_dir))ok=0;
    if(!ok)unlinkat(pipeline_disk_dir,temporary,0);return ok;
}
static void pipeline_disk_save(void) {
    uint8_t *blob=NULL;size_t bytes=0;uint64_t generation=0,snapshot_ns=0;
    VkResult result=VK_INCOMPLETE;
    // 有界重试；分配在cache锁外，截断/变化中的blob不替换旧文件。
    // Bound retries, allocate outside the cache lock, and never replace the old file with an incomplete snapshot.
    for(unsigned attempt=0;attempt<2;attempt++){
        size_t capacity=0;uint64_t begin=dm_android_clock_ns(CLOCK_MONOTONIC);
        generation=atomic_load(&pipeline_cache_generation);   /* read first: a creation ending during the copy counts as newer, and is saved next time */
        result=vkGetPipelineCacheData(device,pipeline_cache,&capacity,NULL);
        snapshot_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-begin;
        if(result==VK_SUCCESS&&capacity==pipeline_disk_bytes){atomic_store(&pipeline_cache_saved,generation);return;}
        if(result!=VK_SUCCESS||capacity<32||capacity>PIPELINE_DISK_LIMIT)break;
        blob=malloc(capacity);if(!blob)break;bytes=capacity;
        begin=dm_android_clock_ns(CLOCK_MONOTONIC);
        result=vkGetPipelineCacheData(device,pipeline_cache,&bytes,blob);snapshot_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-begin;
        if(result==VK_SUCCESS&&bytes<=capacity&&pipeline_disk_blob_valid(blob,bytes))break;
        free(blob);blob=NULL;bytes=0;
    }
    if(!blob||result!=VK_SUCCESS||!pipeline_disk_blob_valid(blob,bytes)){free(blob);LOG("[vulkan-pipeline-cache] save=skipped result=%d snapshot_wall_ms=%.3f limit=%u",result,snapshot_ns/1e6,PIPELINE_DISK_LIMIT);return;}
    uint64_t io_start=dm_android_clock_ns(CLOCK_MONOTONIC);int ok=pipeline_disk_write(blob,bytes);free(blob);
    if(ok){atomic_store(&pipeline_cache_saved,generation);pipeline_disk_bytes=bytes;atomic_store(&pipeline_disk_written,dm_android_clock_ns(CLOCK_MONOTONIC));}
    LOG("[vulkan-pipeline-cache] save=%s bytes=%zu generation=%llu snapshot_wall_ms=%.3f io_wall_ms=%.3f queue_lock_held=0",ok?"complete":"failed",bytes,(unsigned long long)generation,snapshot_ns/1e6,(dm_android_clock_ns(CLOCK_MONOTONIC)-io_start)/1e6);
}
static void *pipeline_disk_main(void *unused) {
    (void)unused;pthread_setname_np(pthread_self(),"D2R PSO cache");totk_pin_self("TOTK_PSO_CPUS","all");   /* not the translation thread's core, which created it */
    pthread_mutex_lock(&pipeline_disk_mutex);
    for(;;){
        while(!pipeline_disk_requested&&!pipeline_disk_stop)pthread_cond_wait(&pipeline_disk_condition,&pipeline_disk_mutex);
        int stop=pipeline_disk_stop;pipeline_disk_requested=0;pthread_mutex_unlock(&pipeline_disk_mutex);
        if(atomic_load(&pipeline_cache_generation)!=atomic_load(&pipeline_cache_saved))pipeline_disk_save();
        pthread_mutex_lock(&pipeline_disk_mutex);if(stop)break;
    }
    pthread_mutex_unlock(&pipeline_disk_mutex);return NULL;
}
static void pipeline_disk_request_locked(uint64_t now) {
    pipeline_disk_attempted=1;pipeline_disk_attempt=now;
    if(!pipeline_disk_started){int error=pthread_create(&pipeline_disk_thread,NULL,pipeline_disk_main,NULL);if(error){LOG("[vulkan-pipeline-cache] worker_create_failed=%d",error);return;}pipeline_disk_started=1;}
    pipeline_disk_requested=1;pthread_cond_signal(&pipeline_disk_condition);
}
// 只在低频present边界请求；最多一个待办，不在游戏线程读取cache或写文件。
// Request only at infrequent present boundaries, with one pending job; the game thread never extracts or writes cache data.
static void pipeline_disk_poll(void) {
    if(pipeline_disk_dir<0||!presents||presents%60)return;
    uint64_t generation=atomic_load(&pipeline_cache_generation),saved=atomic_load(&pipeline_cache_saved);if(generation==saved)return;
    uint64_t now=dm_android_clock_ns(CLOCK_MONOTONIC),changed=atomic_load(&pipeline_cache_changed);
    /* the whole file is rewritten each time: not more than every 5 minutes. Leaving the game saves too (mtl_drain), so
       only a crash loses what was compiled since */
    uint64_t written=atomic_load(&pipeline_disk_written);if(written&&now-written<300000000000ull)return;
    pthread_mutex_lock(&pipeline_disk_mutex);
    uint64_t cooldown=pipeline_disk_attempted?30000000000ull:5000000000ull;
    if(pipeline_disk_stop||now-pipeline_disk_attempt<cooldown||(now-changed<3000000000ull&&now-pipeline_disk_attempt<60000000000ull)){pthread_mutex_unlock(&pipeline_disk_mutex);return;}
    pipeline_disk_request_locked(now);pthread_mutex_unlock(&pipeline_disk_mutex);
}
/* A save now, whatever the timing above (the PSO warm-up calls it when it is done). */
static void pipeline_disk_request(void) {
    if(pipeline_disk_dir<0)return;
    pthread_mutex_lock(&pipeline_disk_mutex);if(!pipeline_disk_stop)pipeline_disk_request_locked(dm_android_clock_ns(CLOCK_MONOTONIC));pthread_mutex_unlock(&pipeline_disk_mutex);
}
// mtl_drain 在释放queue_lock后调用；挂起入口另行阻止新NVN调用，其它入口仍由cache锁保护。
// Called after mtl_drain releases queue_lock; suspend separately gates NVN calls, while the cache lock protects other callers.
static void pipeline_disk_stop_join(void) {
    pthread_mutex_lock(&pipeline_disk_join_mutex);
    pthread_mutex_lock(&pipeline_disk_mutex);
    if(pipeline_disk_dir<0){pthread_mutex_unlock(&pipeline_disk_mutex);pthread_mutex_unlock(&pipeline_disk_join_mutex);return;}
    if(!pipeline_disk_started&&atomic_load(&pipeline_cache_generation)!=atomic_load(&pipeline_cache_saved)){
        int error=pthread_create(&pipeline_disk_thread,NULL,pipeline_disk_main,NULL);if(!error)pipeline_disk_started=1;else LOG("[vulkan-pipeline-cache] final_worker_failed=%d",error);
    }
    int join=pipeline_disk_started;pipeline_disk_stop=1;pthread_cond_signal(&pipeline_disk_condition);pthread_mutex_unlock(&pipeline_disk_mutex);
    if(join)pthread_join(pipeline_disk_thread,NULL);
    pthread_mutex_lock(&pipeline_disk_mutex);pipeline_disk_started=0;pipeline_disk_stop=0;pthread_mutex_unlock(&pipeline_disk_mutex);
    pthread_mutex_unlock(&pipeline_disk_join_mutex);
}
static void pipeline_cache_note(VkResult result) {
    if(result==VK_SUCCESS){atomic_fetch_add(&pipeline_cache_generation,1);atomic_store(&pipeline_cache_changed,dm_android_clock_ns(CLOCK_MONOTONIC));}
}
/* VkPipelineCache is internally synchronized (no EXTERNALLY_SYNCHRONIZED flag): creations run in parallel (the background
   pipeline workers, nvn_vulkan_draw.inc, and the translation thread's compute pipelines), and the change bookkeeping is
   atomics, so a creation waits neither for another compile nor for a save's copy of the cache. */
static __thread int pipeline_create_background;   /* set on the pipeline thread: its creations are not the frame's */
static uint64_t pipeline_sync_ns,pipeline_sync_count;   /* creations on the translation thread, inside a frame (the hitch log) */
static VkResult pipeline_create_graphics(const VkGraphicsPipelineCreateInfo *info,VkPipeline *pipeline) {
    uint64_t t0=dm_android_clock_ns(CLOCK_MONOTONIC);
    VkResult result=vkCreateGraphicsPipelines(device,pipeline_cache,1,info,NULL,pipeline);
    if(!pipeline_create_background){pipeline_sync_ns+=dm_android_clock_ns(CLOCK_MONOTONIC)-t0;pipeline_sync_count++;}pipeline_cache_note(result);return result;
}
static VkResult pipeline_create_compute(const VkComputePipelineCreateInfo *info,VkPipeline *pipeline) {
    VkResult result=vkCreateComputePipelines(device,pipeline_cache,1,info,NULL,pipeline);pipeline_cache_note(result);return result;
}
/* Caches of other namespaces (another driver or driver build, another translator generation) are never read again unless
   that driver comes back: each is 30-200 MB, and on the Odin 3 414 of them had piled up to 11.8 GB by 2026-11-03. The
   writer (it holds writer.lock) removes those untouched for 7 days, and temporary files a crashed save left behind. */
static void pipeline_disk_prune(void) {
    if(pipeline_disk_dir<0||pipeline_disk_lock<0)return;
    int fd=dup(pipeline_disk_dir);if(fd<0)return;DIR *d=fdopendir(fd);if(!d){close(fd);return;}
    time_t now=time(NULL);unsigned removed=0;uint64_t bytes=0;struct dirent *e;
    while((e=readdir(d))){
        const char *n=e->d_name;size_t len=strlen(n);struct stat st;
        int bin=len==68&&!strcmp(n+64,".bin")&&strcmp(n,pipeline_disk_name),tmp=n[0]=='.'&&len>4&&!strcmp(n+len-4,".tmp");
        if(!bin&&!tmp)continue;
        if(fstatat(pipeline_disk_dir,n,&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode))continue;
        if(now-st.st_mtime<(bin?7*86400:3600))continue;
        if(!unlinkat(pipeline_disk_dir,n,0)){removed++;bytes+=(uint64_t)st.st_size;}
    }
    closedir(d);
    if(removed)LOG("[vulkan-pipeline-cache] pruned %u old caches (%.0f MB)",removed,bytes/1048576.0);
}
static void pipeline_cache_initialize(const VkPhysicalDeviceProperties *properties) {
    pipeline_disk_properties=*properties;
    const char *directory=getenv("DM_VK_PIPELINE_CACHE_DIR");
    if(directory&&*directory){
        pipeline_disk_dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(pipeline_disk_dir>=0){pipeline_disk_lock=openat(pipeline_disk_dir,"writer.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
            if(pipeline_disk_lock<0||flock(pipeline_disk_lock,LOCK_EX|LOCK_NB)){if(pipeline_disk_lock>=0)close(pipeline_disk_lock);pipeline_disk_lock=-1;close(pipeline_disk_dir);pipeline_disk_dir=-1;LOG("[vulkan-pipeline-cache] disk_disabled=writer_lock");}}
    }
    // namespace包含真实设备/驱动与应用ABI；blob内部头另外核实，文件名不是验证依据。
    // Namespace includes actual device/driver and application ABI; validate the blob header independently of its filename.
    uint8_t identity[48]={0};pipeline_disk_put32(identity,properties->vendorID);pipeline_disk_put32(identity+4,properties->deviceID);pipeline_disk_put32(identity+8,properties->driverVersion);pipeline_disk_put32(identity+12,properties->apiVersion);memcpy(identity+16,properties->pipelineCacheUUID,16);pipeline_disk_put32(identity+32,gpu_subgroup_size);
#ifdef __ANDROID__
    pipeline_disk_put32(identity+36,vk_api.actual_driver_id);pipeline_disk_put32(identity+40,vk_api.using_custom);pipeline_disk_put32(identity+44,vk_api.requested_subgroup_size);
#endif
    dm_android_sha256_context digest;dm_android_sha256_init(&digest);const char schema[]="d2r-android-pipeline-cache-v1";dm_android_sha256_update(&digest,schema,sizeof schema);dm_android_sha256_update(&digest,identity,sizeof identity);
#ifdef __ANDROID__
    /* the driver's name, not its folder: the app's library folder gets a new random name with every install, which made
       each update start with an empty cache (and left a 30-50 MB file behind each time). The driver build is in
       driverVersion and pipelineCacheUUID above, and the driver checks the blob's own header */
    dm_android_sha256_update(&digest,vk_api.driver_name,(uint32_t)strlen(vk_api.driver_name));
#endif
    const char *profile=getenv("DM_VK_SHADERS");if(profile)dm_android_sha256_update(&digest,profile,(uint32_t)strlen(profile));
    shader_variant_identity(&digest);dm_android_sha256_final(pipeline_disk_identity,&digest);
    for(unsigned i=0;i<32;i++)snprintf(pipeline_disk_name+2*i,3,"%02x",pipeline_disk_identity[i]);strcpy(pipeline_disk_name+64,".bin");
    size_t size=0;void *data=pipeline_disk_load(&size);VkPipelineCacheCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=size,.pInitialData=data};
    VkResult result=vkCreatePipelineCache(device,&info,NULL,&pipeline_cache);free(data);
    if(result!=VK_SUCCESS&&size){LOG("[vulkan-pipeline-cache] driver_rejected_initial=%d retry_empty=1",result);info.initialDataSize=0;info.pInitialData=NULL;result=vkCreatePipelineCache(device,&info,NULL,&pipeline_cache);}
    VK_CHECK(result);pipeline_disk_attempt=dm_android_clock_ns(CLOCK_MONOTONIC);
    if(vkGetPipelineCacheData(device,pipeline_cache,&pipeline_disk_bytes,NULL)!=VK_SUCCESS)pipeline_disk_bytes=0;
    LOG("[vulkan-pipeline-cache] disk_enabled=%d vendor=%#x device=%#x driver=%#x namespace=%s limit=%u",pipeline_disk_dir>=0,properties->vendorID,properties->deviceID,properties->driverVersion,pipeline_disk_name,PIPELINE_DISK_LIMIT);
    pipeline_disk_prune();
}
