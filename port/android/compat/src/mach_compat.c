/* Linux implementation of the Mach calls MacNativeGames uses.

   Guest memory: every mach_vm_map / mach_vm_allocate reservation is backed by its own memfd and recorded in a small
   registry, so mach_vm_remap(copy = FALSE) can alias pages by mapping the same file offset again (Linux cannot alias
   private anonymous pages; Darwin's remap does). Commits stay mprotect() on the reservation; compat_decommit punches the
   pages out of the file. Threads are identified by tid; their CPU time comes from the per-thread CPU clock. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

vm_size_t vm_page_size = 4096;

/* ---- time ---- */
uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t mach_continuous_time(void) { struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
int mach_timebase_info(mach_timebase_info_data_t *info) { info->numer = 1; info->denom = 1; return KERN_SUCCESS; }

/* ---- tasks and threads ---- */
mach_port_t mach_task_self(void) { return (mach_port_t)getpid(); }
mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }
mach_port_t pthread_mach_thread_np(pthread_t thread) { return (mach_port_t)pthread_gettid_np(thread); }
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }
kern_return_t mach_port_mod_refs(mach_port_t task, mach_port_t name, int right, int delta) { (void)task; (void)name; (void)right; (void)delta; return KERN_NOT_SUPPORTED; }

static uint64_t thread_cpu_ns(pid_t tid) {
    /* CPUCLOCK_PERTHREAD | CPUCLOCK_SCHED */
    clockid_t clock = (clockid_t)((~(unsigned)tid << 3) | 6);
    struct timespec t;
    return clock_gettime(clock, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

kern_return_t thread_info(thread_act_t thread, thread_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count) {
    uint64_t ns = thread_cpu_ns((pid_t)thread);
    if (flavor == THREAD_BASIC_INFO) {
        thread_basic_info_data_t *b = (void *)info; memset(b, 0, sizeof *b);
        b->user_time.seconds = (int)(ns / 1000000000ull); b->user_time.microseconds = (int)(ns % 1000000000ull / 1000);
        *count = THREAD_BASIC_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == THREAD_IDENTIFIER_INFO) {
        thread_identifier_info_data_t *i = (void *)info; memset(i, 0, sizeof *i); i->thread_id = thread;
        *count = THREAD_IDENTIFIER_INFO_COUNT; return KERN_SUCCESS;
    }
    if (flavor == THREAD_EXTENDED_INFO) {
        thread_extended_info_data_t *x = (void *)info; memset(x, 0, sizeof *x); x->pth_user_time = ns;
        char path[64]; snprintf(path, sizeof path, "/proc/self/task/%u/comm", thread);
        FILE *f = fopen(path, "r"); if (f) { if (fgets(x->pth_name, sizeof x->pth_name, f)) x->pth_name[strcspn(x->pth_name, "\n")] = 0; fclose(f); }
        *count = THREAD_EXTENDED_INFO_COUNT; return KERN_SUCCESS;
    }
    return KERN_NOT_SUPPORTED;
}

kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task;
    if (flavor != TASK_VM_INFO) return KERN_NOT_SUPPORTED;
    task_vm_info_data_t *v = (void *)info; memset(v, 0, sizeof *v);
    unsigned long size = 0, resident = 0, shared = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%lu %lu %lu", &size, &resident, &shared) != 3) size = resident = 0; fclose(f); }
    v->virtual_size = (uint64_t)size * 4096; v->resident_size = (uint64_t)resident * 4096; v->phys_footprint = v->resident_size;
    v->page_size = 4096;
    *count = TASK_VM_INFO_COUNT;
    return KERN_SUCCESS;
}

/* Thread enumeration and suspension are used by diagnostics only (watchpoints, dump on hang); not available here. */
kern_return_t task_threads(task_t task, thread_act_array_t *list, mach_msg_type_number_t *count) { (void)task; *list = NULL; *count = 0; return KERN_NOT_SUPPORTED; }
kern_return_t thread_suspend(thread_act_t thread) { (void)thread; return KERN_NOT_SUPPORTED; }
kern_return_t thread_resume(thread_act_t thread) { (void)thread; return KERN_NOT_SUPPORTED; }
kern_return_t thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t info, mach_msg_type_number_t count) {
    (void)thread; (void)flavor; (void)info; (void)count; return KERN_SUCCESS;
}
kern_return_t thread_policy_get(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t info, mach_msg_type_number_t *count, boolean_t *get_default) {
    (void)thread; (void)flavor; memset(info, 0, *count * sizeof(int)); if (get_default) *get_default = 1; return KERN_SUCCESS;
}

/* ---- memory ---- */
typedef struct { uintptr_t base; size_t size; int fd; } Backing;
#define MAX_BACKINGS 4096
static Backing backings[MAX_BACKINGS];
static unsigned nbackings;
static pthread_mutex_t backing_lock = PTHREAD_MUTEX_INITIALIZER;

static int prot_of(vm_prot_t p) { return (p & VM_PROT_READ ? PROT_READ : 0) | (p & VM_PROT_WRITE ? PROT_WRITE : 0) | (p & VM_PROT_EXECUTE ? PROT_EXEC : 0); }

static Backing *backing_at(uintptr_t a) {
    for (unsigned i = 0; i < nbackings; i++) if (a - backings[i].base < backings[i].size) return &backings[i];
    return NULL;
}

/* Aliases: mach_vm_remap(copy = FALSE) mappings of a backing's pages at another address. Overlays: a range of a backing
   whose pages were replaced by a dma-buf (zero-copy GPU memory, compat_guest_overlay); every mapping of those memfd
   offsets (the backing's own and its aliases) maps the dma-buf instead, and so do later aliases of them. */
typedef struct { uintptr_t base; size_t size; int fd; off_t off; } Alias;
typedef struct { int fd; off_t off; size_t size; int dmabuf; } Overlay;
#define MAX_ALIASES 65536
/* one per zero-copy pool: the game has over 2,048 of them with the 4 KiB floor; the process limits are 32,768 fds
   and 65,530 mappings */
#define MAX_OVERLAYS 16384
static Alias aliases[MAX_ALIASES]; static unsigned naliases;
static Overlay overlays[MAX_OVERLAYS]; static unsigned noverlays;
unsigned compat_overlay_count(void) { return noverlays; }
/* caller holds backing_lock */
static void alias_drop_inside(uintptr_t lo, uintptr_t hi) {
    for (unsigned i = 0; i < naliases;) if (aliases[i].base >= lo && aliases[i].base + aliases[i].size <= hi) aliases[i] = aliases[--naliases]; else i++;
}
static void overlay_drop_fd(int fd) { for (unsigned i = 0; i < noverlays;) if (overlays[i].fd == fd) overlays[i] = overlays[--noverlays]; else i++; }
/* Map [va, va+size) to memfd fd at off, using the dma-buf for the parts that are overlaid. Caller holds backing_lock. */
static int map_file_range(uintptr_t va, size_t size, int prot, int fd, off_t off) {
    off_t end = off + (off_t)size;
    while (off < end) {
        const Overlay *best = NULL; off_t next = end;
        for (unsigned i = 0; i < noverlays; i++) { const Overlay *o = &overlays[i]; if (o->fd != fd) continue;
            if (o->off <= off && off < o->off + (off_t)o->size) { best = o; break; }
            if (o->off > off && o->off < next) next = o->off; }
        off_t piece_end = best ? (best->off + (off_t)best->size < end ? best->off + (off_t)best->size : end) : next;
        size_t n = (size_t)(piece_end - off);
        /* MAP_NORESERVE as in backed_map: a piece without it never merges with its neighbours, and they pile up to
           vm.max_map_count */
        void *p = best ? mmap((void *)va, n, prot, MAP_SHARED | MAP_FIXED, best->dmabuf, off - best->off)
                       : mmap((void *)va, n, prot, MAP_SHARED | MAP_NORESERVE | MAP_FIXED, fd, off);
        if (p != (void *)va) return -1;
        va += n; off = piece_end;
    }
    return 0;
}
/* Every mapping of memfd fd overlapping [off, off+size): calls fn with the address range of the intersection. */
static int for_each_mapping(int fd, off_t off, size_t size, int (*fn)(uintptr_t va, size_t n, off_t file_off, void *ctx), void *ctx) {
    off_t end = off + (off_t)size;
    for (unsigned i = 0; i < nbackings; i++) if (backings[i].fd == fd) {
        off_t lo = off > 0 ? off : 0, hi = end < (off_t)backings[i].size ? end : (off_t)backings[i].size;
        if (lo < hi && fn(backings[i].base + (uintptr_t)lo, (size_t)(hi - lo), lo, ctx)) return -1; }
    for (unsigned i = 0; i < naliases; i++) if (aliases[i].fd == fd) {
        off_t lo = off > aliases[i].off ? off : aliases[i].off, hi = end < aliases[i].off + (off_t)aliases[i].size ? end : aliases[i].off + (off_t)aliases[i].size;
        if (lo < hi && fn(aliases[i].base + (uintptr_t)(lo - aliases[i].off), (size_t)(hi - lo), lo, ctx)) return -1; }
    return 0;
}
typedef struct { int fd; int prot; } remap_ctx;
static int remap_one(uintptr_t va, size_t n, off_t file_off, void *ctx) { const remap_ctx *c = ctx; return map_file_range(va, n, c->prot, c->fd, file_off); }

/* The memfd and offset behind [a, a+size): a backing's own mapping or an alias. Caller holds backing_lock. */
static int resolve(uintptr_t a, size_t size, int *fd, off_t *off) {
    Backing *b = backing_at(a);
    if (b) { if (a + size > b->base + b->size) return 0; *fd = b->fd; *off = (off_t)(a - b->base); return 1; }
    for (unsigned i = 0; i < naliases; i++) if (a - aliases[i].base < aliases[i].size) {
        if (a + size > aliases[i].base + aliases[i].size) return 0;
        *fd = aliases[i].fd; *off = aliases[i].off + (off_t)(a - aliases[i].base); return 1; }
    return 0;
}
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
int compat_guest_overlay(void *address, size_t size, int dmabuf) {
    pthread_mutex_lock(&backing_lock);
    int bfd; off_t off;
    if (!resolve((uintptr_t)address, size, &bfd, &off) || noverlays == MAX_OVERLAYS) { pthread_mutex_unlock(&backing_lock); return -1; }
    for (unsigned i = 0; i < noverlays; i++)
        if (overlays[i].fd == bfd && overlays[i].off < off + (off_t)size && off < overlays[i].off + (off_t)overlays[i].size) { pthread_mutex_unlock(&backing_lock); return -1; }
    void *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
    if (tmp == MAP_FAILED) { pthread_mutex_unlock(&backing_lock); return -1; }
    /* the pool's contents so far */
    memcpy(tmp, address, size); munmap(tmp, size);
    overlays[noverlays++] = (Overlay){bfd, off, size, dmabuf};
    remap_ctx c = {bfd, PROT_READ | PROT_WRITE};
    int r = for_each_mapping(bfd, off, size, remap_one, &c);
    /* A failed remap leaves the range split between two backings, and the caller closes the dma-buf. The memfd still holds
       the pages, so everything goes back to it. */
    if (r) { noverlays--; for_each_mapping(bfd, off, size, remap_one, &c); }
    /* the dma-buf replaces these memfd pages: free them */
    if (!r) fallocate(bfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, (off_t)size);
    /* The remap leaves the range's page tables empty, so every page the game touches afterwards faults on its own
       threads. TOTK_OVERLAY_POPULATE=1 populates the new mapping here instead; it is off because the pool is made on
       the game thread, so populating here only moves the cost. */
    { static int populate = -1; if (populate < 0) { const char *e = getenv("TOTK_OVERLAY_POPULATE"); populate = e && atoi(e) != 0; }
      /* an older kernel: touch the pages */
      if (!r && populate && madvise(address, size, MADV_POPULATE_WRITE)) { volatile uint8_t *p = address;
          for (size_t i = 0; i < size; i += 4096) p[i] = p[i]; } }
    pthread_mutex_unlock(&backing_lock);
    return r;
}
int compat_guest_unoverlay(void *address, size_t size) {
    pthread_mutex_lock(&backing_lock);
    int fd = -1; off_t off = 0; unsigned k = noverlays;
    int found = resolve((uintptr_t)address, size, &fd, &off);
    for (unsigned i = 0; found && i < noverlays; i++) if (overlays[i].fd == fd && overlays[i].off == off && overlays[i].size == size) k = i;
    if (k == noverlays) { pthread_mutex_unlock(&backing_lock); return -1; }
    /* contents back into the memfd */
    ssize_t w = pwrite(fd, address, size, off);
    overlays[k] = overlays[--noverlays];
    remap_ctx c = {fd, PROT_READ | PROT_WRITE};
    int r = w == (ssize_t)size ? for_each_mapping(fd, off, size, remap_one, &c) : -1;
    pthread_mutex_unlock(&backing_lock);
    return r;
}

/* Map [address, address+size) again as one read-write mapping (dma-buf parts included). Page-sized mprotect splits a
   mapping; memfd pieces merge back if their flags match, dma-buf ones never do, and enough of them hit vm.max_map_count, after which mprotect
   fails (nvn_dirty.c then calls this). */
int compat_guest_remap(void *address, size_t size) {
    pthread_mutex_lock(&backing_lock);
    int fd; off_t off;
    int r = resolve((uintptr_t)address, size, &fd, &off) ? map_file_range((uintptr_t)address, size, PROT_READ | PROT_WRITE, fd, off) : -1;
    pthread_mutex_unlock(&backing_lock);
    return r;
}

/* A memfd-backed mapping of size bytes at at (fixed, must be free unless overwrite) or anywhere (at == 0). */
static void *backed_map(uintptr_t at, size_t size, int prot, int overwrite) {
    int fd = memfd_create("guest", MFD_CLOEXEC);
    if (fd < 0) return MAP_FAILED;
    if (ftruncate(fd, (off_t)size)) { close(fd); return MAP_FAILED; }
    int flags = MAP_SHARED | MAP_NORESERVE | (at ? (overwrite ? MAP_FIXED : MAP_FIXED_NOREPLACE) : 0);
    void *p = mmap((void *)at, size, prot, flags, fd, 0);
    if (p == MAP_FAILED || (at && (uintptr_t)p != at)) { if (p != MAP_FAILED) munmap(p, size); close(fd); return MAP_FAILED; }
    pthread_mutex_lock(&backing_lock);
    /* an overwrite replaces whatever backing covered the range: drop records wholly inside it */
    for (unsigned i = 0; i < nbackings;)
        if (backings[i].base >= (uintptr_t)p && backings[i].base + backings[i].size <= (uintptr_t)p + size) { overlay_drop_fd(backings[i].fd); close(backings[i].fd); backings[i] = backings[--nbackings]; }
        else i++;
    alias_drop_inside((uintptr_t)p, (uintptr_t)p + size);
    if (nbackings == MAX_BACKINGS) { pthread_mutex_unlock(&backing_lock); munmap(p, size); close(fd); return MAP_FAILED; }
    backings[nbackings++] = (Backing){(uintptr_t)p, size, fd};
    pthread_mutex_unlock(&backing_lock);
    return p;
}

kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags) {
    (void)task;
    int anywhere = flags & VM_FLAGS_ANYWHERE, overwrite = flags & VM_FLAGS_OVERWRITE;
    void *p = backed_map(anywhere ? 0 : (uintptr_t)*address, (size_t)size, PROT_READ | PROT_WRITE, overwrite);
    if (p == MAP_FAILED) return anywhere ? KERN_RESOURCE_SHORTAGE : KERN_NO_SPACE;
    *address = (uintptr_t)p;
    return KERN_SUCCESS;
}

kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                          mem_entry_name_port_t object, memory_object_offset_t offset, boolean_t copy, vm_prot_t cur,
                          vm_prot_t max, vm_inherit_t inheritance) {
    (void)task; (void)max; (void)inheritance; (void)copy; (void)offset;
    if (object != MEMORY_OBJECT_NULL || mask) return KERN_NOT_SUPPORTED;
    int anywhere = flags & VM_FLAGS_ANYWHERE, overwrite = flags & VM_FLAGS_OVERWRITE;
    void *p = backed_map(anywhere ? 0 : (uintptr_t)*address, (size_t)size, prot_of(cur), overwrite);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    *address = (uintptr_t)p;
    return KERN_SUCCESS;
}

kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size) {
    (void)task;
    if (munmap((void *)(uintptr_t)address, (size_t)size)) return KERN_INVALID_ADDRESS;
    pthread_mutex_lock(&backing_lock);
    for (unsigned i = 0; i < nbackings;)
        if (backings[i].base >= address && backings[i].base + backings[i].size <= address + size) { overlay_drop_fd(backings[i].fd); close(backings[i].fd); backings[i] = backings[--nbackings]; }
        else i++;
    alias_drop_inside(address, address + size);
    pthread_mutex_unlock(&backing_lock);
    return KERN_SUCCESS;
}
kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size) { return mach_vm_deallocate(task, address, size); }

/* copy = FALSE: map the same backing pages at the target (an alias); copy = TRUE: a private copy of their contents. */
kern_return_t mach_vm_remap(vm_map_t task, mach_vm_address_t *target, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                            vm_map_t src_task, mach_vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max,
                            vm_inherit_t inheritance) {
    (void)task; (void)src_task; (void)inheritance;
    if (mask || (flags & VM_FLAGS_ANYWHERE)) return KERN_NOT_SUPPORTED;
    void *dst = (void *)(uintptr_t)*target;
    if (copy) {
        void *p = mmap(dst, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p != dst) return KERN_NO_SPACE;
        pthread_mutex_lock(&backing_lock); alias_drop_inside((uintptr_t)dst, (uintptr_t)dst + size); pthread_mutex_unlock(&backing_lock);
        memcpy(dst, (void *)(uintptr_t)src, (size_t)size);
    } else {
        pthread_mutex_lock(&backing_lock);
        /* an alias of an alias resolves too */
        int fd = -1; off_t off = 0; int ok = resolve((uintptr_t)src, (size_t)size, &fd, &off);
        if (!ok) { pthread_mutex_unlock(&backing_lock); fprintf(stderr, "[compat] mach_vm_remap: %#llx+%#llx is not memfd-backed guest memory\n", (unsigned long long)src, (unsigned long long)size); return KERN_INVALID_ADDRESS; }
        int r = map_file_range((uintptr_t)dst, (size_t)size, PROT_READ | PROT_WRITE, fd, off);
        if (!r) { alias_drop_inside((uintptr_t)dst, (uintptr_t)dst + size);
            if (naliases < MAX_ALIASES) aliases[naliases++] = (Alias){(uintptr_t)dst, (size_t)size, fd, off};
            else fprintf(stderr, "[compat] alias registry full: zero-copy overlays will miss %#llx\n", (unsigned long long)(uintptr_t)dst); }
        pthread_mutex_unlock(&backing_lock);
        if (r) return KERN_NO_SPACE;
    }
    if (cur) *cur = VM_PROT_READ | VM_PROT_WRITE;
    if (max) *max = VM_PROT_ALL;
    return KERN_SUCCESS;
}

int compat_decommit(void *address, size_t size) {
    pthread_mutex_lock(&backing_lock);
    Backing *b = backing_at((uintptr_t)address);
    int fd = b && (uintptr_t)address + size <= b->base + b->size ? b->fd : -1; off_t off = fd >= 0 ? (off_t)((uintptr_t)address - b->base) : 0;
    pthread_mutex_unlock(&backing_lock);
    if (mprotect(address, size, PROT_NONE)) return -1;
    if (fd >= 0) return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, (off_t)size);
    return madvise(address, size, MADV_DONTNEED);
}

/* /proc/self/maps: the region containing *address, else the next one above it (Darwin's semantics). */
static int find_region(uint64_t want, uint64_t *start, uint64_t *end, int *prot) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e; char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perm) != 3 || e <= want) continue;
        *start = s; *end = e;
        *prot = (perm[0] == 'r' ? VM_PROT_READ : 0) | (perm[1] == 'w' ? VM_PROT_WRITE : 0) | (perm[2] == 'x' ? VM_PROT_EXECUTE : 0);
        found = 1; break;
    }
    fclose(f);
    return found;
}

kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size, vm_region_flavor_t flavor,
                             vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object) {
    (void)task;
    if (flavor != VM_REGION_BASIC_INFO_64) return KERN_NOT_SUPPORTED;
    uint64_t s, e; int prot;
    if (!find_region(*address, &s, &e, &prot)) return KERN_INVALID_ADDRESS;
    vm_region_basic_info_data_64_t *b = (void *)info; memset(b, 0, sizeof *b);
    b->protection = prot; b->max_protection = VM_PROT_ALL;
    *address = s; *size = e - s; *count = VM_REGION_BASIC_INFO_COUNT_64;
    if (object) *object = MACH_PORT_NULL;
    return KERN_SUCCESS;
}

kern_return_t vm_region_recurse_64(vm_map_t task, vm_address_t *address, vm_size_t *size, natural_t *depth,
                                   vm_region_recurse_info_t info, mach_msg_type_number_t *count) {
    (void)task; (void)depth;
    uint64_t s, e; int prot;
    if (!find_region(*address, &s, &e, &prot)) return KERN_INVALID_ADDRESS;
    vm_region_submap_info_data_64_t *r = (void *)info; memset(r, 0, sizeof *r);
    r->protection = prot;
    /* resident pages from mincore over at most 64 MiB at a time (memory reports only) */
    size_t n = (size_t)(e - s), pages = n / 4096; unsigned char vec[16384];
    for (size_t done = 0; done < pages;) {
        size_t k = pages - done < sizeof vec ? pages - done : sizeof vec;
        if (mincore((void *)(uintptr_t)(s + done * 4096), k * 4096, vec)) break;
        for (size_t i = 0; i < k; i++) r->pages_resident += vec[i] & 1;
        done += k;
    }
    r->pages_dirtied = r->pages_resident;
    *address = (vm_address_t)s; *size = (vm_size_t)(e - s); *count = VM_REGION_SUBMAP_INFO_COUNT_64;
    return KERN_SUCCESS;
}

/* A read that fails instead of faulting on unmapped or protected memory. */
kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data,
                                     mach_vm_size_t *out) {
    (void)task;
    struct iovec local = {(void *)(uintptr_t)data, (size_t)size}, remote = {(void *)(uintptr_t)address, (size_t)size};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n != (ssize_t)size) { if (out) *out = n > 0 ? (mach_vm_size_t)n : 0; return KERN_INVALID_ADDRESS; }
    if (out) *out = size;
    return KERN_SUCCESS;
}
