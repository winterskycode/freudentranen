#pragma once
#include <stdint.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
typedef mach_timebase_info_data_t *mach_timebase_info_t;
/* Nanoseconds on CLOCK_MONOTONIC: the timebase is 1/1. */
uint64_t mach_absolute_time(void);
uint64_t mach_continuous_time(void);
int mach_timebase_info(mach_timebase_info_data_t *info);
