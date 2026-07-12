#pragma once

#include "klink_v1.h"

enum klink_v1_fault {
    KLINK_FAULT_NONE = 0,
    KLINK_FAULT_BAD_CELL = 1,
    KLINK_FAULT_SEQUENCE = 2,
    KLINK_FAULT_CREDIT = 3,
    KLINK_FAULT_ACK = 4,
};

enum klink_v1_event_flag {
    KLINK_EVENT_NONE = 0,
    KLINK_EVENT_RX = 1u << 0,
    KLINK_EVENT_RX_DUPLICATE = 1u << 1,
    KLINK_EVENT_TX_ACKED = 1u << 2,
    KLINK_EVENT_FAULT = 1u << 3,
};

typedef struct klink_v1_event {
    uint32_t flags;
    uint8_t rx_channel;
    uint8_t rx_type;
    uint16_t rx_sequence;
    uint8_t tx_acked_channel;
    uint16_t tx_acked_sequence;
    uint8_t fault;
} klink_v1_event_t;

typedef struct klink_v1_stats {
    uint32_t tx_cells;
    uint32_t tx_repeated;
    uint32_t tx_acked;
    uint32_t rx_cells;
    uint32_t rx_delivered;
    uint32_t rx_duplicates;
    uint32_t rx_realtime_gaps;
    uint32_t bad_cells;
    uint32_t faults;
} klink_v1_stats_t;

typedef struct klink_v1_endpoint {
    uint16_t tx_next[KLINK_V1_CHANNELS];
    uint16_t tx_last_acked[KLINK_V1_CHANNELS];
    uint16_t rx_expected[KLINK_V1_CHANNELS];
    uint16_t rx_last[KLINK_V1_CHANNELS];
    uint16_t rx_realtime_last[KLINK_V1_CHANNELS];
    uint8_t local_credit[KLINK_V1_CHANNELS];
    uint8_t peer_credit[KLINK_V1_CHANNELS];
    uint8_t tx_last_acked_mask;
    uint8_t rx_seen_mask;
    uint8_t rx_realtime_seen_mask;
    uint8_t tx_queued_mask;
    uint8_t ack_pending_mask;
    uint8_t status_cursor;
    uint8_t fault;
    uint8_t fault_channel;
    uint16_t fault_expected;
    uint16_t fault_observed;
    bool inflight;
    uint8_t inflight_channel;
    klink_v1_cell_t inflight_cell;
    klink_v1_cell_t queued[KLINK_V1_CHANNELS];
    klink_v1_stats_t stats;
} klink_v1_endpoint_t;

#if defined(__cplusplus)
extern "C" {
#endif

void klink_v1_endpoint_init(klink_v1_endpoint_t *endpoint, uint8_t initial_credit);
void klink_v1_set_credit(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t credit);
bool klink_v1_release_credit(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t count);
bool klink_v1_queue(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t type,
                    uint16_t flags, const void *payload, size_t size);
void klink_v1_build_tx(klink_v1_endpoint_t *endpoint, klink_v1_cell_t *cell);
uint32_t klink_v1_process_rx(klink_v1_endpoint_t *endpoint,
                             const klink_v1_cell_t *cell,
                             klink_v1_event_t *event);

#if defined(__cplusplus)
}
#endif
