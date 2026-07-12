#ifndef K210_KSTREAM_SLAVE_H
#define K210_KSTREAM_SLAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kstream_slave_stats {
    uint32_t downlink_used;
    uint32_t downlink_free;
    uint32_t uplink_used;
    uint32_t uplink_free;
    uint32_t console_rx_used;
    uint32_t console_tx_used;
    uint32_t commands;
    uint32_t faults;
    uint64_t downlink_bytes;
    uint64_t uplink_bytes;
} kstream_slave_stats_t;

bool kstream_slave_start(void);
void kstream_slave_get_stats(kstream_slave_stats_t *stats);

uint8_t *kstream_downlink_read_acquire(size_t *length);
void kstream_downlink_read_commit(size_t length);

uint8_t *kstream_uplink_write_acquire(size_t *length);
void kstream_uplink_write_commit(size_t length);

size_t kstream_console_read(void *data, size_t length);
size_t kstream_console_write(const void *data, size_t length);

#endif
