#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct sha256_stream {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    uint32_t block_bytes;
} sha256_stream_t;

void sha256_stream_init(sha256_stream_t *ctx);
void sha256_stream_update(sha256_stream_t *ctx, const void *data, size_t size);
void sha256_stream_final(sha256_stream_t *ctx, uint8_t digest[32]);
