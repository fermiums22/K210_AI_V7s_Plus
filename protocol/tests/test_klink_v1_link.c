#include "klink_v1_link.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        return 0;
    }
    printf("PASS %s\n", name);
    return 1;
}

static void exchange(klink_v1_endpoint_t *a, klink_v1_endpoint_t *b,
                     klink_v1_event_t *event_a, klink_v1_event_t *event_b,
                     klink_v1_cell_t *a_cell, klink_v1_cell_t *b_cell)
{
    klink_v1_build_tx(a, a_cell);
    klink_v1_build_tx(b, b_cell);
    (void)klink_v1_process_rx(a, b_cell, event_a);
    (void)klink_v1_process_rx(b, a_cell, event_b);
}

int main(void)
{
    int ok = 1;
    klink_v1_endpoint_t a;
    klink_v1_endpoint_t b;
    klink_v1_event_t event_a;
    klink_v1_event_t event_b;
    klink_v1_cell_t a_cell;
    klink_v1_cell_t b_cell;

    klink_v1_endpoint_init(&a, 2u);
    klink_v1_endpoint_init(&b, 2u);

    for (int i = 0; i < 8; ++i)
        exchange(&a, &b, &event_a, &event_b, &a_cell, &b_cell);
    ok &= check(a.peer_credit[KLINK_CH_BULK] == 2u, "a-learns-b-credit");
    ok &= check(b.peer_credit[KLINK_CH_BULK] == 2u, "b-learns-a-credit");

    const char payload[] = "firmware-block";
    ok &= check(klink_v1_queue(&a, KLINK_CH_BULK, KLINK_T_DATA,
                               KLINK_F_RELIABLE, payload, sizeof(payload)),
                "queue-reliable");

    klink_v1_build_tx(&a, &a_cell);
    ok &= check(a.inflight, "reliable-inflight");
    ok &= check(klink_v1_process_rx(&b, &a_cell, &event_b) & KLINK_EVENT_RX,
                "reliable-delivered");
    ok &= check(b.local_credit[KLINK_CH_BULK] == 1u, "credit-consumed");
    ok &= check(memcmp(a_cell.payload, payload, sizeof(payload)) == 0, "payload-exact");

    klink_v1_build_tx(&a, &a_cell);
    ok &= check(klink_v1_process_rx(&b, &a_cell, &event_b) & KLINK_EVENT_RX_DUPLICATE,
                "duplicate-not-redelivered");
    ok &= check(b.stats.rx_delivered == 1u, "single-delivery-count");

    klink_v1_build_tx(&b, &b_cell);
    ok &= check(klink_v1_process_rx(&a, &b_cell, &event_a) & KLINK_EVENT_TX_ACKED,
                "ack-clears-inflight");
    ok &= check(!a.inflight && a.stats.tx_acked == 1u, "inflight-cleared");
    ok &= check(klink_v1_release_credit(&b, KLINK_CH_BULK, 1u), "credit-release");

    for (int i = 0; i < 8; ++i)
        exchange(&a, &b, &event_a, &event_b, &a_cell, &b_cell);
    ok &= check(a.peer_credit[KLINK_CH_BULK] == 2u, "released-credit-advertised");

    klink_v1_endpoint_t bad = b;
    klink_v1_build_tx(&a, &a_cell);
    a_cell.payload[0] ^= 0x80u;
    ok &= check(klink_v1_process_rx(&bad, &a_cell, &event_b) & KLINK_EVENT_FAULT,
                "crc-fault-immediate");
    ok &= check(bad.fault == KLINK_FAULT_BAD_CELL, "crc-fault-latched");

    if (!ok)
        return 1;
    printf("KLINK_V1_LINK_TESTS_PASS tx=%lu rx=%lu duplicates=%lu\n",
           (unsigned long)a.stats.tx_cells,
           (unsigned long)b.stats.rx_cells,
           (unsigned long)b.stats.rx_duplicates);
    return 0;
}
