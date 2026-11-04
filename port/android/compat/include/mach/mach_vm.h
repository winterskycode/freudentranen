#pragma once
#include <mach/mach.h>
/* mach_vm_region_recurse over the same region registry as vm_region_recurse_64 (src/mach_compat.c) */
static inline kern_return_t mach_vm_region_recurse(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t *s, natural_t *depth,
                                                   vm_region_recurse_info_t info, mach_msg_type_number_t *cnt) {
    vm_address_t v = (vm_address_t)*a; vm_size_t n = 0;
    kern_return_t kr = vm_region_recurse_64(t, &v, &n, depth, info, cnt);
    *a = v; *s = n; return kr;
}
