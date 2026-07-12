#include "klink_v1_link.h"

#include <string.h>

static const uint8_t s_tx_priority[KLINK_V1_CHANNELS] = {
    KLINK_CH_CONTROL,
    KLINK_CH_ROBOT,
    KLINK_CH_AUDIO_OUT,
    KLINK_CH_AUDIO_IN,
    KLINK_CH_CAMERA,
    KLINK_CH_BULK,
    KLINK_CH_DIAG,
    KLINK_CH_RESERVED,
};

static uint8_t bit_for(uint8_t channel)
{
    return (uint8_t)(1u << channel);
}

static void latch_fault(klink_v1_endpoint_t *endpoint, uint8_t fault,
                        uint8_t channel, uint16_t expected, uint16_t observed,
                        klink_v1_event_t *event)
{
    if (endpoint->fault == KLINK_FAULT_NONE) {
        endpoint->fault = fault;
        endpoint->fault_channel = channel;
        endpoint->fault_expected = expected;
        endpoint->fault_observed = observed;
        endpoint->stats.faults++;
    }
    if (event) {
        event->flags |= KLINK_EVENT_FAULT;
        event->fault = endpoint->fault;
    }
}

void klink_v1_endpoint_init(klink_v1_endpoint_t *endpoint, uint8_t initial_credit)
{
    if (!endpoint)
        return;
    memset(endpoint, 0, sizeof(*endpoint));
    if (initial_credit > 31u)
        initial_credit = 31u;
    for (uint8_t channel = 0; channel < KLINK_V1_CHANNELS; ++channel)
        endpoint->local_credit[channel] = initial_credit;
}

void klink_v1_set_credit(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t credit)
{
    if (!endpoint || channel >= KLINK_V1_CHANNELS)
        return;
    endpoint->local_credit[channel] = credit > 31u ? 31u : credit;
}

bool klink_v1_release_credit(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t count)
{
    if (!endpoint || channel >= KLINK_V1_CHANNELS || count == 0u)
        return false;
    uint16_t next = (uint16_t)endpoint->local_credit[channel] + count;
    endpoint->local_credit[channel] = next > 31u ? 31u : (uint8_t)next;
    return true;
}

bool klink_v1_queue(klink_v1_endpoint_t *endpoint, uint8_t channel, uint8_t type,
                    uint16_t flags, const void *payload, size_t size)
{
    if (!endpoint || endpoint->fault != KLINK_FAULT_NONE ||
        channel >= KLINK_V1_CHANNELS || type > 31u ||
        (endpoint->tx_queued_mask & bit_for(channel)) != 0u)
        return false;

    klink_v1_cell_t *cell = &endpoint->queued[channel];
    klink_v1_cell_clear(cell, channel, type);
    cell->flags = (uint16_t)(flags & ~(KLINK_F_ACK_VALID | KLINK_F_CREDIT_VALID));
    if (!klink_v1_cell_set_payload(cell, payload, size))
        return false;
    endpoint->tx_queued_mask |= bit_for(channel);
    return true;
}

static int select_queued_channel(const klink_v1_endpoint_t *endpoint)
{
    for (uint8_t i = 0; i < KLINK_V1_CHANNELS; ++i) {
        uint8_t channel = s_tx_priority[i];
        uint8_t bit = bit_for(channel);
        if ((endpoint->tx_queued_mask & bit) == 0u)
            continue;
        const klink_v1_cell_t *cell = &endpoint->queued[channel];
        if ((cell->flags & KLINK_F_RELIABLE) != 0u && endpoint->peer_credit[channel] == 0u)
            continue;
        return channel;
    }
    return -1;
}

static uint8_t select_status_channel(klink_v1_endpoint_t *endpoint, bool *has_ack)
{
    for (uint8_t step = 0; step < KLINK_V1_CHANNELS; ++step) {
        uint8_t channel = (uint8_t)((endpoint->status_cursor + step) & 0x07u);
        if ((endpoint->ack_pending_mask & bit_for(channel)) != 0u) {
            endpoint->status_cursor = (uint8_t)((channel + 1u) & 0x07u);
            endpoint->ack_pending_mask &= (uint8_t)~bit_for(channel);
            *has_ack = true;
            return channel;
        }
    }
    uint8_t channel = endpoint->status_cursor;
    endpoint->status_cursor = (uint8_t)((endpoint->status_cursor + 1u) & 0x07u);
    *has_ack = false;
    return channel;
}

static void attach_status(klink_v1_endpoint_t *endpoint, klink_v1_cell_t *cell)
{
    bool has_ack = false;
    uint8_t channel = select_status_channel(endpoint, &has_ack);
    cell->flags |= KLINK_F_CREDIT_VALID;
    cell->ack_channel_credit = KLINK_ACK_CREDIT(channel, endpoint->local_credit[channel]);
    if (has_ack) {
        cell->flags |= KLINK_F_ACK_VALID;
        cell->acknowledgement = endpoint->rx_last[channel];
    } else {
        cell->flags &= (uint16_t)~KLINK_F_ACK_VALID;
        cell->acknowledgement = 0u;
    }
}

static void build_fault_cell(klink_v1_endpoint_t *endpoint, klink_v1_cell_t *cell)
{
    uint8_t payload[8] = {
        endpoint->fault,
        endpoint->fault_channel,
        (uint8_t)(endpoint->fault_expected & 0xffu),
        (uint8_t)(endpoint->fault_expected >> 8),
        (uint8_t)(endpoint->fault_observed & 0xffu),
        (uint8_t)(endpoint->fault_observed >> 8),
        0u,
        0u,
    };
    klink_v1_cell_clear(cell, KLINK_CH_CONTROL, KLINK_T_FAULT);
    cell->flags = KLINK_F_URGENT;
    (void)klink_v1_cell_set_payload(cell, payload, sizeof(payload));
}

void klink_v1_build_tx(klink_v1_endpoint_t *endpoint, klink_v1_cell_t *cell)
{
    if (!endpoint || !cell)
        return;

    bool repeated = false;
    if (endpoint->fault != KLINK_FAULT_NONE) {
        build_fault_cell(endpoint, cell);
    } else if (endpoint->inflight) {
        *cell = endpoint->inflight_cell;
        repeated = true;
    } else {
        int selected = select_queued_channel(endpoint);
        if (selected >= 0) {
            uint8_t channel = (uint8_t)selected;
            *cell = endpoint->queued[channel];
            endpoint->tx_queued_mask &= (uint8_t)~bit_for(channel);
            cell->sequence = endpoint->tx_next[channel]++;
            if ((cell->flags & KLINK_F_RELIABLE) != 0u) {
                endpoint->peer_credit[channel]--;
                endpoint->inflight = true;
                endpoint->inflight_channel = channel;
                endpoint->inflight_cell = *cell;
            }
        } else {
            klink_v1_cell_clear(cell, KLINK_CH_CONTROL, KLINK_T_IDLE);
        }
    }

    attach_status(endpoint, cell);
    klink_v1_cell_finalize(cell);
    endpoint->stats.tx_cells++;
    if (repeated)
        endpoint->stats.tx_repeated++;
}

static void process_status(klink_v1_endpoint_t *endpoint, const klink_v1_cell_t *cell,
                           klink_v1_event_t *event)
{
    uint8_t channel = KLINK_ACK_CHANNEL(cell->ack_channel_credit);
    if ((cell->flags & KLINK_F_CREDIT_VALID) != 0u)
        endpoint->peer_credit[channel] = KLINK_CREDIT(cell->ack_channel_credit);

    if ((cell->flags & KLINK_F_ACK_VALID) == 0u)
        return;

    uint16_t ack = cell->acknowledgement;
    if (endpoint->inflight && channel == endpoint->inflight_channel &&
        ack == endpoint->inflight_cell.sequence) {
        endpoint->inflight = false;
        endpoint->tx_last_acked[channel] = ack;
        endpoint->tx_last_acked_mask |= bit_for(channel);
        endpoint->stats.tx_acked++;
        event->flags |= KLINK_EVENT_TX_ACKED;
        event->tx_acked_channel = channel;
        event->tx_acked_sequence = ack;
        return;
    }

    if ((endpoint->tx_last_acked_mask & bit_for(channel)) != 0u &&
        endpoint->tx_last_acked[channel] == ack)
        return;

    latch_fault(endpoint, KLINK_FAULT_ACK, channel,
                endpoint->inflight ? endpoint->inflight_cell.sequence : 0u,
                ack, event);
}

static void process_reliable(klink_v1_endpoint_t *endpoint, const klink_v1_cell_t *cell,
                             uint8_t channel, klink_v1_event_t *event)
{
    uint8_t bit = bit_for(channel);
    uint16_t sequence = cell->sequence;
    uint16_t expected = endpoint->rx_expected[channel];

    if ((endpoint->rx_seen_mask & bit) != 0u && sequence == endpoint->rx_last[channel]) {
        endpoint->ack_pending_mask |= bit;
        endpoint->stats.rx_duplicates++;
        event->flags |= KLINK_EVENT_RX_DUPLICATE;
        event->rx_channel = channel;
        event->rx_type = KLINK_TYPE(cell->channel_type);
        event->rx_sequence = sequence;
        return;
    }

    if (sequence != expected) {
        latch_fault(endpoint, KLINK_FAULT_SEQUENCE, channel, expected, sequence, event);
        return;
    }
    if (endpoint->local_credit[channel] == 0u) {
        latch_fault(endpoint, KLINK_FAULT_CREDIT, channel, 1u, 0u, event);
        return;
    }

    endpoint->local_credit[channel]--;
    endpoint->rx_seen_mask |= bit;
    endpoint->rx_last[channel] = sequence;
    endpoint->rx_expected[channel] = (uint16_t)(sequence + 1u);
    endpoint->ack_pending_mask |= bit;
    endpoint->stats.rx_delivered++;
    event->flags |= KLINK_EVENT_RX;
    event->rx_channel = channel;
    event->rx_type = KLINK_TYPE(cell->channel_type);
    event->rx_sequence = sequence;
}

static void process_realtime(klink_v1_endpoint_t *endpoint, const klink_v1_cell_t *cell,
                             uint8_t channel, klink_v1_event_t *event)
{
    uint8_t bit = bit_for(channel);
    if ((endpoint->rx_realtime_seen_mask & bit) != 0u) {
        uint16_t expected = (uint16_t)(endpoint->rx_realtime_last[channel] + 1u);
        if (cell->sequence != expected)
            endpoint->stats.rx_realtime_gaps += (uint16_t)(cell->sequence - expected);
    }
    endpoint->rx_realtime_seen_mask |= bit;
    endpoint->rx_realtime_last[channel] = cell->sequence;
    endpoint->stats.rx_delivered++;
    event->flags |= KLINK_EVENT_RX;
    event->rx_channel = channel;
    event->rx_type = KLINK_TYPE(cell->channel_type);
    event->rx_sequence = cell->sequence;
}

uint32_t klink_v1_process_rx(klink_v1_endpoint_t *endpoint,
                             const klink_v1_cell_t *cell,
                             klink_v1_event_t *event)
{
    klink_v1_event_t local_event;
    if (!event)
        event = &local_event;
    memset(event, 0, sizeof(*event));

    if (!endpoint || !cell) {
        event->flags = KLINK_EVENT_FAULT;
        event->fault = KLINK_FAULT_BAD_CELL;
        return event->flags;
    }

    endpoint->stats.rx_cells++;
    if (!klink_v1_cell_validate(cell)) {
        endpoint->stats.bad_cells++;
        latch_fault(endpoint, KLINK_FAULT_BAD_CELL, 0u, 0u, 0u, event);
        return event->flags;
    }

    process_status(endpoint, cell, event);
    if (endpoint->fault != KLINK_FAULT_NONE)
        return event->flags;

    uint8_t type = KLINK_TYPE(cell->channel_type);
    if (type == KLINK_T_IDLE)
        return event->flags;

    uint8_t channel = KLINK_CHANNEL(cell->channel_type);
    if ((cell->flags & KLINK_F_RELIABLE) != 0u)
        process_reliable(endpoint, cell, channel, event);
    else
        process_realtime(endpoint, cell, channel, event);
    return event->flags;
}
