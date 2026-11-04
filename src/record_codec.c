#include "tinykv/record.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t RECORD_MAGIC[4] = {'T', 'K', 'V', '1'};

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t tinykv_crc32_iso_hdlc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    if (bytes == NULL && length != 0) return 0;
    crc = crc32_update(crc, bytes, length);
    return crc ^ 0xFFFFFFFFu;
}

static tinykv_record_status validate_fields(
    uint8_t type, const uint8_t *key, size_t key_length,
    const uint8_t *value, size_t value_length,
    uint32_t max_key_length, uint32_t max_value_length)
{
    if ((key == NULL && key_length != 0) ||
        (value == NULL && value_length != 0))
        return TINYKV_RECORD_INVALID_ARGUMENT;
    if (type != TINYKV_RECORD_PUT && type != TINYKV_RECORD_DELETE)
        return TINYKV_RECORD_INVALID_TYPE;
    if (type == TINYKV_RECORD_DELETE && value_length != 0)
        return TINYKV_RECORD_INVALID_FORMAT;
    if (key_length > UINT32_MAX || value_length > UINT32_MAX ||
        key_length > max_key_length || value_length > max_value_length)
        return TINYKV_RECORD_LIMIT_EXCEEDED;
    return TINYKV_RECORD_OK;
}

tinykv_record_status tinykv_record_encode(
    uint8_t type, const uint8_t *key, size_t key_length,
    const uint8_t *value, size_t value_length,
    uint32_t max_key_length, uint32_t max_value_length,
    uint8_t **out_bytes, size_t *out_length)
{
    tinykv_record_status status;
    uint8_t *record;
    size_t total_length;
    uint32_t crc;

    if (out_bytes == NULL || out_length == NULL)
        return TINYKV_RECORD_INVALID_ARGUMENT;
    *out_bytes = NULL;
    *out_length = 0;
    status = validate_fields(type, key, key_length, value, value_length,
                             max_key_length, max_value_length);
    if (status != TINYKV_RECORD_OK) return status;

    if (key_length > SIZE_MAX - TINYKV_RECORD_HEADER_SIZE ||
        value_length > SIZE_MAX - TINYKV_RECORD_HEADER_SIZE - key_length)
        return TINYKV_RECORD_LIMIT_EXCEEDED;
    total_length = TINYKV_RECORD_HEADER_SIZE + key_length + value_length;
    record = (uint8_t *)malloc(total_length);
    if (record == NULL) return TINYKV_RECORD_OUT_OF_MEMORY;

    memcpy(record, RECORD_MAGIC, sizeof(RECORD_MAGIC));
    record[4] = type;
    write_be32(record + 5, (uint32_t)key_length);
    write_be32(record + 9, (uint32_t)value_length);
    if (key_length != 0)
        memcpy(record + TINYKV_RECORD_HEADER_SIZE, key, key_length);
    if (value_length != 0)
        memcpy(record + TINYKV_RECORD_HEADER_SIZE + key_length, value, value_length);

    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, record + 4, 9);
    crc = crc32_update(crc, record + TINYKV_RECORD_HEADER_SIZE,
                       key_length + value_length);
    write_be32(record + 13, crc ^ 0xFFFFFFFFu);
    *out_bytes = record;
    *out_length = total_length;
    return TINYKV_RECORD_OK;
}

tinykv_record_status tinykv_record_decode(
    const uint8_t *bytes, size_t length,
    uint32_t max_key_length, uint32_t max_value_length,
    tinykv_record_view *out, size_t *consumed)
{
    uint32_t key_length, value_length, expected_crc, actual_crc;
    size_t total_length;

    if (out == NULL || consumed == NULL)
        return TINYKV_RECORD_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    *consumed = 0;
    if (bytes == NULL)
        return TINYKV_RECORD_INVALID_ARGUMENT;
    if (length < TINYKV_RECORD_HEADER_SIZE) return TINYKV_RECORD_TRUNCATED;
    if (memcmp(bytes, RECORD_MAGIC, sizeof(RECORD_MAGIC)) != 0)
        return TINYKV_RECORD_INVALID_FORMAT;
    if (bytes[4] != TINYKV_RECORD_PUT && bytes[4] != TINYKV_RECORD_DELETE)
        return TINYKV_RECORD_INVALID_TYPE;

    key_length = read_be32(bytes + 5);
    value_length = read_be32(bytes + 9);
    if (key_length > max_key_length || value_length > max_value_length)
        return TINYKV_RECORD_LIMIT_EXCEEDED;
    if (bytes[4] == TINYKV_RECORD_DELETE && value_length != 0)
        return TINYKV_RECORD_INVALID_FORMAT;
    if ((size_t)key_length > SIZE_MAX - TINYKV_RECORD_HEADER_SIZE ||
        (size_t)value_length > SIZE_MAX - TINYKV_RECORD_HEADER_SIZE - (size_t)key_length)
        return TINYKV_RECORD_LIMIT_EXCEEDED;

    total_length = TINYKV_RECORD_HEADER_SIZE + (size_t)key_length + (size_t)value_length;
    if (length < total_length) return TINYKV_RECORD_TRUNCATED;

    actual_crc = 0xFFFFFFFFu;
    actual_crc = crc32_update(actual_crc, bytes + 4, 9);
    actual_crc = crc32_update(actual_crc, bytes + TINYKV_RECORD_HEADER_SIZE,
                              (size_t)key_length + (size_t)value_length);
    actual_crc ^= 0xFFFFFFFFu;
    expected_crc = read_be32(bytes + 13);
    if (actual_crc != expected_crc) return TINYKV_RECORD_BAD_CHECKSUM;

    out->type = bytes[4];
    out->key_length = key_length;
    out->value_length = value_length;
    out->key = bytes + TINYKV_RECORD_HEADER_SIZE;
    out->value = out->key + key_length;
    *consumed = total_length;
    return TINYKV_RECORD_OK;
}

const char *tinykv_record_status_string(tinykv_record_status status)
{
    switch (status) {
    case TINYKV_RECORD_OK: return "ok";
    case TINYKV_RECORD_INVALID_ARGUMENT: return "invalid argument";
    case TINYKV_RECORD_INVALID_TYPE: return "invalid record type";
    case TINYKV_RECORD_INVALID_FORMAT: return "invalid record format";
    case TINYKV_RECORD_LIMIT_EXCEEDED: return "configured size limit exceeded";
    case TINYKV_RECORD_TRUNCATED: return "truncated record";
    case TINYKV_RECORD_BAD_CHECKSUM: return "checksum mismatch";
    case TINYKV_RECORD_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown status";
}
