#ifndef TINYKV_RECORD_H
#define TINYKV_RECORD_H
#include <stddef.h>
#include <stdint.h>
#define TINYKV_RECORD_HEADER_SIZE 17u
enum { TINYKV_RECORD_PUT = 1, TINYKV_RECORD_DELETE = 2 };
typedef enum {
    TINYKV_RECORD_OK = 0, TINYKV_RECORD_INVALID_ARGUMENT,
    TINYKV_RECORD_INVALID_TYPE, TINYKV_RECORD_INVALID_FORMAT,
    TINYKV_RECORD_LIMIT_EXCEEDED, TINYKV_RECORD_TRUNCATED,
    TINYKV_RECORD_BAD_CHECKSUM, TINYKV_RECORD_OUT_OF_MEMORY
} tinykv_record_status;
typedef struct {
    uint8_t type;
    const uint8_t *key;
    uint32_t key_length;
    const uint8_t *value;
    uint32_t value_length;
} tinykv_record_view;
uint32_t tinykv_crc32_iso_hdlc(const uint8_t *bytes, size_t length);
tinykv_record_status tinykv_record_encode(
    uint8_t type, const uint8_t *key, size_t key_length,
    const uint8_t *value, size_t value_length,
    uint32_t max_key_length, uint32_t max_value_length,
    uint8_t **out_bytes, size_t *out_length);
tinykv_record_status tinykv_record_decode(
    const uint8_t *bytes, size_t length,
    uint32_t max_key_length, uint32_t max_value_length,
    tinykv_record_view *out, size_t *consumed);
const char *tinykv_record_status_string(tinykv_record_status status);
#endif
