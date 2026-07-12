#include "kstream_v2.h"

uint32_t kstream_v2_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void kstream_v2_command_finalize(kstream_v2_command_t *command)
{
    command->crc32 = kstream_v2_crc32(command, offsetof(kstream_v2_command_t, crc32));
}

bool kstream_v2_command_valid(const kstream_v2_command_t *command)
{
    return command->magic == KSTREAM_V2_MAGIC_CMD &&
           command->version == KSTREAM_V2_VERSION &&
           command->crc32 ==
               kstream_v2_crc32(command, offsetof(kstream_v2_command_t, crc32));
}

void kstream_v2_response_finalize(kstream_v2_response_t *response)
{
    response->crc32 = kstream_v2_crc32(response, offsetof(kstream_v2_response_t, crc32));
}

bool kstream_v2_response_valid(const kstream_v2_response_t *response)
{
    return response->magic == KSTREAM_V2_MAGIC_RESPONSE &&
           response->version == KSTREAM_V2_VERSION &&
           response->crc32 ==
               kstream_v2_crc32(response, offsetof(kstream_v2_response_t, crc32));
}
