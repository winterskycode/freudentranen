#pragma once
#include <malloc.h>
#define malloc_size(p) malloc_usable_size((void *)(p))
