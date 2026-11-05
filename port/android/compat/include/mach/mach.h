/* Linux implementation of the Mach calls MacNativeGames uses (port/android/compat/src/mach_compat.c).
   Tasks and threads are identified by pid/tid; VM calls map to mmap/mremap over a memfd-backed guest region registry,
   so mach_vm_remap can alias pages (Linux can only alias shared mappings). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>
#include <mach/mach_time.h>

typedef int kern_return_t;
typedef unsigned int mach_port_t, vm_map_t, thread_act_t, thread_t, task_t, host_t, natural_t, mem_entry_name_port_t;
typedef mach_port_t *thread_act_array_t;
typedef unsigned int mach_msg_type_number_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t, mach_vm_offset_t, memory_object_offset_t;
typedef uintptr_t vm_address_t, vm_size_t, vm_offset_t;
typedef int vm_prot_t, vm_inherit_t, boolean_t, vm_region_flavor_t, thread_flavor_t, task_flavor_t, thread_policy_flavor_t;
typedef int *vm_region_info_t, *vm_region_recurse_info_t, *thread_info_t, *task_info_t, *thread_policy_t;
typedef int policy_t;

#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_PROTECTION_FAILURE 2
#define KERN_NO_SPACE 3
#define KERN_INVALID_ARGUMENT 4
#define KERN_FAILURE 5
#define KERN_RESOURCE_SHORTAGE 6
#define KERN_NOT_SUPPORTED 46
#define MACH_PORT_NULL 0u
#define MEMORY_OBJECT_NULL 0u
#define TRUE 1
#define FALSE 0

#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
#define VM_PROT_DEFAULT 3
#define VM_INHERIT_SHARE 0
#define VM_INHERIT_COPY 1
#define VM_INHERIT_NONE 2
#define VM_INHERIT_DEFAULT 1
#define VM_FLAGS_FIXED 0x0
#define VM_FLAGS_ANYWHERE 0x1
#define VM_FLAGS_OVERWRITE 0x4000

extern vm_size_t vm_page_size;

typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved;
                 memory_object_offset_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
typedef struct { vm_prot_t protection, max_protection; unsigned int pages_resident, pages_dirtied, pages_swapped_out,
                 /* is_submap: always 0 here */
                 pages_shared_now_private, pages_reusable; boolean_t is_submap; } vm_region_submap_info_data_64_t;
#define VM_REGION_SUBMAP_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_submap_info_data_64_t) / sizeof(int)))

typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage; policy_t policy; int run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
typedef struct { uint64_t thread_id, thread_handle, dispatch_qaddr; } thread_identifier_info_data_t;
#define THREAD_IDENTIFIER_INFO 4
#define THREAD_IDENTIFIER_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_identifier_info_data_t) / sizeof(int)))
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags,
                 pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
typedef struct { uint64_t virtual_size, region_count, page_size, resident_size, resident_size_peak, device, device_peak,
                 internal, internal_peak, external, external_peak, reusable, reusable_peak, purgeable_volatile_pmap,
                 purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak,
                 compressed_lifetime, phys_footprint, min_address, max_address; } task_vm_info_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))

mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
mach_port_t pthread_mach_thread_np(pthread_t thread);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
#define MACH_PORT_RIGHT_SEND 0
/* not supported: no port is kept */
kern_return_t mach_port_mod_refs(mach_port_t task, mach_port_t name, int right, int delta);
kern_return_t thread_info(thread_act_t thread, thread_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count);
kern_return_t task_info(task_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count);
kern_return_t task_threads(task_t task, thread_act_array_t *list, mach_msg_type_number_t *count);
kern_return_t thread_suspend(thread_act_t thread);
kern_return_t thread_resume(thread_act_t thread);
kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size);

kern_return_t mach_vm_allocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags);
kern_return_t mach_vm_deallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size);
kern_return_t mach_vm_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                          mem_entry_name_port_t object, memory_object_offset_t offset, boolean_t copy, vm_prot_t cur,
                          vm_prot_t max, vm_inherit_t inheritance);
kern_return_t mach_vm_remap(vm_map_t task, mach_vm_address_t *target, mach_vm_size_t size, mach_vm_offset_t mask, int flags,
                            vm_map_t src_task, mach_vm_address_t src, boolean_t copy, vm_prot_t *cur, vm_prot_t *max,
                            vm_inherit_t inheritance);
kern_return_t mach_vm_region(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t *size, vm_region_flavor_t flavor,
                             vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object);
kern_return_t mach_vm_read_overwrite(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data,
                                     mach_vm_size_t *out);
kern_return_t vm_region_recurse_64(vm_map_t task, vm_address_t *address, vm_size_t *size, natural_t *depth,
                                   vm_region_recurse_info_t info, mach_msg_type_number_t *count);

static inline kern_return_t vm_read_overwrite(vm_map_t task, vm_address_t address, vm_size_t size, vm_address_t data, vm_size_t *out) {
    mach_vm_size_t n = 0; kern_return_t r = mach_vm_read_overwrite(task, address, size, data, &n); if (out) *out = (vm_size_t)n; return r;
}

/* Thread register state and hardware watchpoints are Darwin debugging interfaces (hang dumps, the watch diagnostics):
   the types exist so that code compiles; the calls report "not supported". */
typedef int *thread_state_t;
typedef int thread_state_flavor_t;
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
typedef struct { uint64_t __bvr[16], __bcr[16], __wvr[16], __wcr[16], __mdscr_el1; } arm_debug_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define ARM_DEBUG_STATE64 15
#define ARM_DEBUG_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_debug_state64_t) / sizeof(int)))
#define arm_thread_state64_get_pc(s) ((s).__pc)
#define arm_thread_state64_get_lr(s) ((s).__lr)
#define arm_thread_state64_get_fp(s) ((s).__fp)
#define arm_thread_state64_get_sp(s) ((s).__sp)
static inline kern_return_t thread_get_state(thread_act_t t, thread_state_flavor_t f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_NOT_SUPPORTED; }
static inline kern_return_t thread_set_state(thread_act_t t, thread_state_flavor_t f, thread_state_t s, mach_msg_type_number_t n) { (void)t; (void)f; (void)s; (void)n; return KERN_NOT_SUPPORTED; }

/* Android: give a range of memfd-backed guest memory back to the system (hole punch) and leave it PROT_NONE;
   the range keeps its backing, so later commits and aliases still work. Plain anonymous memory is replaced instead. */
int compat_decommit(void *address, size_t size);
/* Zero-copy GPU memory: replace a range of memfd-backed guest memory (and every alias of those pages) with a dma-buf of
   the same size; the current contents are copied in first. unoverlay copies them back and restores the memfd. */
int compat_guest_overlay(void *address, size_t size, int dmabuf);
int compat_guest_unoverlay(void *address, size_t size);
