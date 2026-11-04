#pragma once
#include <mach/mach.h>
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
/* Real-time scheduling classes do not exist for apps on Android: accepted and ignored (priority is set elsewhere). */
kern_return_t thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t info, mach_msg_type_number_t count);
kern_return_t thread_policy_get(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t info, mach_msg_type_number_t *count, boolean_t *get_default);
