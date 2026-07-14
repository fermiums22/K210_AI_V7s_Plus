#ifndef KSTREAM_V2_H
#define KSTREAM_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KSTREAM_V2_MAGIC_CMD       0x324d434bu
#define KSTREAM_V2_MAGIC_RESPONSE  0x3253524bu
#define KSTREAM_V2_VERSION         2u
#define KSTREAM_V2_FRAME_BYTES     64u
#define KSTREAM_V2_INLINE_BYTES    24u
#define KSTREAM_V2_BURST_BYTES     4096u

#define KSTREAM_V2_PORT_DOWNLINK   21010u
#define KSTREAM_V2_PORT_UPLINK     21011u
#define KSTREAM_V2_PORT_CONSOLE    21012u

typedef enum kstream_v2_stream {
    KSTREAM_V2_STREAM_NONE = 0,
    KSTREAM_V2_STREAM_DOWNLINK = 1,
    KSTREAM_V2_STREAM_UPLINK = 2,
    KSTREAM_V2_STREAM_CONSOLE_RX = 3,
    KSTREAM_V2_STREAM_CONSOLE_TX = 4,
} kstream_v2_stream_t;

typedef enum kstream_v2_opcode {
    KSTREAM_V2_OP_HELLO = 1,
    KSTREAM_V2_OP_STATUS = 2,
    KSTREAM_V2_OP_PUSH = 3,
    KSTREAM_V2_OP_PULL = 4,
    KSTREAM_V2_OP_CONTROL = 5,
    KSTREAM_V2_OP_ACTIVATE_INT = 6,
} kstream_v2_opcode_t;

/* ACTIVATE_INT is the only transaction sent before phase signalling starts.
 * The ESP master selects this single supported wire contract explicitly. */
#define KSTREAM_V2_INT_MODE_TOGGLE       0x01u
#define KSTREAM_V2_INT_EVENT_PHASE_ARMED 0x00000001u
#define KSTREAM_V2_INT_MODE_LEVEL        0x02u
#define KSTREAM_V2_INT_EVENT_DMA_READY   0x00000002u
#define KSTREAM_V2_INT_BOOT_LEVEL_HIGH   0x00000001u

typedef enum kstream_v2_result {
    KSTREAM_V2_RESULT_OK = 0,
    KSTREAM_V2_RESULT_BAD_MAGIC = 1,
    KSTREAM_V2_RESULT_BAD_CRC = 2,
    KSTREAM_V2_RESULT_BAD_SEQUENCE = 3,
    KSTREAM_V2_RESULT_BAD_OPCODE = 4,
    KSTREAM_V2_RESULT_BAD_STREAM = 5,
    KSTREAM_V2_RESULT_BAD_LENGTH = 6,
    KSTREAM_V2_RESULT_NO_CREDIT = 7,
    KSTREAM_V2_RESULT_BUSY = 8,
    KSTREAM_V2_RESULT_IO = 9,
} kstream_v2_result_t;

typedef struct __attribute__((packed, aligned(4))) kstream_v2_command {
    uint32_t magic;
    uint8_t version;
    uint8_t opcode;
    uint8_t stream;
    uint8_t flags;
    uint32_t sequence;
    uint32_t length;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
    uint8_t inline_data[KSTREAM_V2_INLINE_BYTES];
    uint32_t crc32;
} kstream_v2_command_t;

typedef struct __attribute__((packed, aligned(4))) kstream_v2_response {
    uint32_t magic;
    uint8_t version;
    uint8_t result;
    uint8_t state;
    uint8_t reserved;
    uint32_t sequence;
    uint32_t downlink_free;
    uint32_t uplink_used;
    uint32_t console_tx_used;
    uint32_t console_rx_free;
    uint32_t actual_length;
    uint32_t faults;
    uint8_t message[24];
    uint32_t crc32;
} kstream_v2_response_t;

_Static_assert(sizeof(kstream_v2_command_t) == KSTREAM_V2_FRAME_BYTES,
               "kstream command must be 64 bytes");
_Static_assert(sizeof(kstream_v2_response_t) == KSTREAM_V2_FRAME_BYTES,
               "kstream response must be 64 bytes");

uint32_t kstream_v2_crc32(const void *data, size_t length);
void kstream_v2_command_finalize(kstream_v2_command_t *command);
bool kstream_v2_command_valid(const kstream_v2_command_t *command);
void kstream_v2_response_finalize(kstream_v2_response_t *response);
bool kstream_v2_response_valid(const kstream_v2_response_t *response);

#endif
