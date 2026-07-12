#include "klink_v1.h"

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

int main(void)
{
    int ok = 1;
    klink_v1_cell_t cell;

    ok &= check(sizeof(cell) == 64u, "cell-size-64");
    ok &= check(klink_v1_crc32("123456789", 9) == 0xcbf43926u, "crc32-known-vector");

    klink_v1_cell_clear(&cell, KLINK_CH_CONTROL, KLINK_T_CAPABILITIES);
    cell.sequence = 0x1234u;
    cell.acknowledgement = 0x00feu;
    cell.ack_channel_credit = KLINK_ACK_CREDIT(KLINK_CH_BULK, 7u);
    cell.flags = KLINK_F_ACK_VALID | KLINK_F_CREDIT_VALID | KLINK_F_RELIABLE |
                 KLINK_F_FIRST | KLINK_F_LAST;
    ok &= check(klink_v1_cell_set_payload(&cell, "abc", 3), "payload-set");
    klink_v1_cell_finalize(&cell);

    ok &= check(KLINK_CHANNEL(cell.channel_type) == KLINK_CH_CONTROL, "channel-decode");
    ok &= check(KLINK_TYPE(cell.channel_type) == KLINK_T_CAPABILITIES, "type-decode");
    ok &= check(klink_v1_cell_validate(&cell), "cell-valid");
    ok &= check(KLINK_ACK_CHANNEL(cell.ack_channel_credit) == KLINK_CH_BULK,
                "ack-channel-decode");
    ok &= check(KLINK_CREDIT(cell.ack_channel_credit) == 7u, "credit-decode");
    ok &= check(cell.crc32 == 0x8b8eb547u, "cell-fixed-vector");

    cell.payload[2] ^= 0x01u;
    ok &= check(!klink_v1_cell_validate(&cell), "corruption-detected");
    cell.payload[2] ^= 0x01u;
    ok &= check(klink_v1_cell_validate(&cell), "corruption-restored");

    ok &= check(!klink_v1_cell_set_payload(&cell, &cell, KLINK_V1_PAYLOAD_BYTES + 1u),
                "oversize-rejected");

    if (!ok)
        return 1;
    printf("KLINK_V1_TESTS_PASS crc=0x%08lx\n", (unsigned long)cell.crc32);
    return 0;
}
