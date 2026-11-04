// SHA-256，替换 CommonCrypto 调用。| SHA-256 in place of CommonCrypto calls.
#pragma once
#include <stdint.h>
typedef struct { uint32_t state[8]; uint64_t length; unsigned used; unsigned char buffer[64]; } dm_android_sha256_context;
int dm_android_sha256_init(dm_android_sha256_context *context);
int dm_android_sha256_update(dm_android_sha256_context *context, const void *data, uint32_t length);
int dm_android_sha256_final(unsigned char output[32], dm_android_sha256_context *context);
