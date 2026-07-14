#ifndef KSPI_V2_H
#define KSPI_V2_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ESP8266 HSPI slave status register (commands 1/4):
 *
 * descriptor, K210 -> ESP
 *   31..24  magic 0xa5
 *   23..20  operation
 *   19..16  memory region
 *   15..8   byte count
 *    7..0   transaction token
 *
 * result, ESP -> K210
 *   31..24  magic 0x5a
 *   23..16  first error
 *   15..8   phase
 *    7..0   echoed transaction token
 */
#define KSPI_V2_DESCRIPTOR_MAGIC 0xa5000000u
#define KSPI_V2_DESCRIPTOR_MASK  0xff000000u
#define KSPI_V2_RESULT_MAGIC     0x5a000000u
#define KSPI_V2_RESULT_MASK      0xff000000u

#define KSPI_V2_OPERATION_SHIFT 20u
#define KSPI_V2_REGION_SHIFT    16u
#define KSPI_V2_LENGTH_SHIFT     8u

#define KSPI_V2_OPERATION_MASK 0x0fu
#define KSPI_V2_REGION_MASK    0x0fu
#define KSPI_V2_LENGTH_MASK    0xffu
#define KSPI_V2_TOKEN_MASK     0xffu

#define KSPI_V2_OPERATION_EXCHANGE 1u
#define KSPI_V2_REGION_KLINK       0u
#define KSPI_V2_CELL_BYTES        64u

#define KSPI_V2_PHASE_COMPLETE 3u

/*
 * Streaming exchanges return the result of exchange N in the otherwise
 * reserved upper ten bits of the KLINK flags field carried by exchange N+1.
 * The whole KLINK cell, including these bits, is protected by its CRC32.
 * This removes a separate SPI status transaction from every 64-byte cell
 * while retaining an exact rolling transaction token and terminal result.
 */
#define KSPI_V2_CELL_APP_FLAGS_MASK   0x003fu
#define KSPI_V2_CELL_ERROR_SHIFT      6u
#define KSPI_V2_CELL_ERROR_MASK       0x00c0u
#define KSPI_V2_CELL_TOKEN_SHIFT      8u
#define KSPI_V2_CELL_TOKEN_MASK       0xff00u

#define KSPI_V2_CELL_RESULT_OK        0u
#define KSPI_V2_CELL_RESULT_FATAL     1u
#define KSPI_V2_CELL_RESULT_QUIESCE   2u

typedef enum kspi_v2_error {
    KSPI_V2_ERROR_NONE = 0,
    KSPI_V2_ERROR_DESCRIPTOR = 1,
    KSPI_V2_ERROR_READ_PHASE = 2,
    KSPI_V2_ERROR_WRITE_PHASE = 3,
    KSPI_V2_ERROR_CELL = 4,
    KSPI_V2_ERROR_RESULT_PHASE = 5,
    KSPI_V2_ERROR_QUIESCE = 0x80
} kspi_v2_error_t;

static inline uint32_t kspi_v2_descriptor(uint8_t operation, uint8_t region,
                                           uint8_t length, uint8_t token)
{
    return KSPI_V2_DESCRIPTOR_MAGIC |
           ((uint32_t)(operation & KSPI_V2_OPERATION_MASK)
            << KSPI_V2_OPERATION_SHIFT) |
           ((uint32_t)(region & KSPI_V2_REGION_MASK)
            << KSPI_V2_REGION_SHIFT) |
           ((uint32_t)length << KSPI_V2_LENGTH_SHIFT) | token;
}

static inline uint8_t kspi_v2_descriptor_operation(uint32_t descriptor)
{
    return (uint8_t)((descriptor >> KSPI_V2_OPERATION_SHIFT) &
                     KSPI_V2_OPERATION_MASK);
}

static inline uint8_t kspi_v2_descriptor_region(uint32_t descriptor)
{
    return (uint8_t)((descriptor >> KSPI_V2_REGION_SHIFT) &
                     KSPI_V2_REGION_MASK);
}

static inline uint8_t kspi_v2_descriptor_length(uint32_t descriptor)
{
    return (uint8_t)((descriptor >> KSPI_V2_LENGTH_SHIFT) &
                     KSPI_V2_LENGTH_MASK);
}

static inline uint8_t kspi_v2_descriptor_token(uint32_t descriptor)
{
    return (uint8_t)(descriptor & KSPI_V2_TOKEN_MASK);
}

static inline bool kspi_v2_descriptor_valid(uint32_t descriptor)
{
    return (descriptor & KSPI_V2_DESCRIPTOR_MASK) ==
               KSPI_V2_DESCRIPTOR_MAGIC &&
           kspi_v2_descriptor_operation(descriptor) ==
               KSPI_V2_OPERATION_EXCHANGE &&
           kspi_v2_descriptor_region(descriptor) == KSPI_V2_REGION_KLINK &&
           kspi_v2_descriptor_length(descriptor) == KSPI_V2_CELL_BYTES;
}

static inline uint32_t kspi_v2_result(uint8_t error, uint8_t phase,
                                      uint8_t token)
{
    return KSPI_V2_RESULT_MAGIC | ((uint32_t)error << 16u) |
           ((uint32_t)phase << 8u) | token;
}

static inline bool kspi_v2_result_valid(uint32_t result, uint8_t token)
{
    return (result & KSPI_V2_RESULT_MASK) == KSPI_V2_RESULT_MAGIC &&
           ((uint8_t)(result >> 16u)) == KSPI_V2_ERROR_NONE &&
           ((uint8_t)(result >> 8u)) == KSPI_V2_PHASE_COMPLETE &&
           (uint8_t)result == token;
}

static inline uint8_t kspi_v2_result_error(uint32_t result)
{
    return (uint8_t)(result >> 16u);
}

static inline bool kspi_v2_result_quiesce(uint32_t result, uint8_t token)
{
    return (result & KSPI_V2_RESULT_MASK) == KSPI_V2_RESULT_MAGIC &&
           kspi_v2_result_error(result) == KSPI_V2_ERROR_QUIESCE &&
           ((uint8_t)(result >> 8u)) == KSPI_V2_PHASE_COMPLETE &&
           (uint8_t)result == token;
}

static inline uint8_t kspi_v2_cell_result_class(uint8_t error)
{
    if (error == KSPI_V2_ERROR_NONE)
        return KSPI_V2_CELL_RESULT_OK;
    if (error == KSPI_V2_ERROR_QUIESCE)
        return KSPI_V2_CELL_RESULT_QUIESCE;
    return KSPI_V2_CELL_RESULT_FATAL;
}

static inline uint16_t kspi_v2_cell_result_flags(uint16_t application_flags,
                                                  uint8_t error,
                                                  uint8_t token)
{
    return (uint16_t)((application_flags & KSPI_V2_CELL_APP_FLAGS_MASK) |
                      ((uint16_t)kspi_v2_cell_result_class(error)
                       << KSPI_V2_CELL_ERROR_SHIFT) |
                      ((uint16_t)token << KSPI_V2_CELL_TOKEN_SHIFT));
}

static inline uint8_t kspi_v2_cell_result(uint16_t flags)
{
    return (uint8_t)((flags & KSPI_V2_CELL_ERROR_MASK) >>
                     KSPI_V2_CELL_ERROR_SHIFT);
}

static inline uint8_t kspi_v2_cell_token(uint16_t flags)
{
    return (uint8_t)((flags & KSPI_V2_CELL_TOKEN_MASK) >>
                     KSPI_V2_CELL_TOKEN_SHIFT);
}

#endif
