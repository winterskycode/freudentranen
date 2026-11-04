/* What host/prompts_romfs.c takes from the runtime's header, for building it alone on a PC (tests/romfs_rules_check.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int (*RomfsGenerator)(const char *path, const uint8_t *data, uint64_t size, uint8_t **out, uint64_t *out_size);
extern RomfsGenerator runtime_romfs_generator;
int runtime_romfs_load(const char *path, uint8_t **out, uint64_t *size);
uint64_t runtime_output_res_extra_bytes(void);
