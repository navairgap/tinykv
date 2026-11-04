#include "tinykv/record.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_crc_vector(void)
{
    static const uint8_t input[] = "123456789";
    assert(tinykv_crc32_iso_hdlc(input, sizeof(input) - 1) == 0xCBF43926u);
}
static void test_put_round_trip(void)
{
    static const uint8_t key[] = {'k', 0, '1'};
    static const uint8_t value[] = {0, 1, 2, 0, 255};
    uint8_t *encoded = NULL;
    size_t length = 0, consumed = 0;
    tinykv_record_view decoded;
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, key, sizeof(key), value,
        sizeof(value), 64, 1024, &encoded, &length) == TINYKV_RECORD_OK);
    assert(tinykv_record_decode(encoded, length, 64, 1024,
        &decoded, &consumed) == TINYKV_RECORD_OK);
    assert(consumed == length && decoded.type == TINYKV_RECORD_PUT);
    assert(decoded.key_length == sizeof(key) && decoded.value_length == sizeof(value));
    assert(memcmp(decoded.key, key, sizeof(key)) == 0);
    assert(memcmp(decoded.value, value, sizeof(value)) == 0);
    free(encoded);
}
static void test_delete_round_trip(void)
{
    static const uint8_t key[] = "gone";
    uint8_t *encoded = NULL;
    size_t length = 0, consumed = 0;
    tinykv_record_view decoded;
    assert(tinykv_record_encode(TINYKV_RECORD_DELETE, key, sizeof(key) - 1,
        NULL, 0, 64, 1024, &encoded, &length) == TINYKV_RECORD_OK);
    assert(tinykv_record_decode(encoded, length, 64, 1024,
        &decoded, &consumed) == TINYKV_RECORD_OK);
    assert(decoded.type == TINYKV_RECORD_DELETE && decoded.value_length == 0);
    free(encoded);
}
static void test_rejects_truncation_and_checksum_corruption(void)
{
    static const uint8_t key[] = "k", value[] = "v";
    uint8_t *encoded = NULL;
    size_t length = 0, consumed = 0;
    tinykv_record_view decoded;
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, key, 1, value, 1,
        64, 64, &encoded, &length) == TINYKV_RECORD_OK);
    assert(tinykv_record_decode(encoded, length - 1, 64, 64,
        &decoded, &consumed) == TINYKV_RECORD_TRUNCATED);
    encoded[length - 1] ^= 1;
    assert(tinykv_record_decode(encoded, length, 64, 64,
        &decoded, &consumed) == TINYKV_RECORD_BAD_CHECKSUM);
    free(encoded);
}
static void test_null_input_clears_decode_outputs(void)
{
    tinykv_record_view decoded = {
        TINYKV_RECORD_DELETE, (const uint8_t *)"stale", 5,
        (const uint8_t *)"value", 5
    };
    size_t consumed = 123;
    assert(tinykv_record_decode(NULL, 0, 64, 64, &decoded, &consumed) ==
           TINYKV_RECORD_INVALID_ARGUMENT);
    assert(consumed == 0);
    assert(decoded.type == 0);
    assert(decoded.key == NULL && decoded.key_length == 0);
    assert(decoded.value == NULL && decoded.value_length == 0);
}

static void test_enforces_limits_before_payload_read(void)
{
    uint8_t header[TINYKV_RECORD_HEADER_SIZE] = {
        'T','K','V','1',TINYKV_RECORD_PUT, 0,0,0,65, 0,0,0,0, 0,0,0,0
    };
    size_t consumed = 0;
    tinykv_record_view decoded;
    assert(tinykv_record_decode(header, sizeof(header), 64, 64,
        &decoded, &consumed) == TINYKV_RECORD_LIMIT_EXCEEDED);
}
int main(void)
{
    test_crc_vector();
    test_put_round_trip();
    test_delete_round_trip();
    test_rejects_truncation_and_checksum_corruption();
    test_null_input_clears_decode_outputs();
    test_enforces_limits_before_payload_read();
    puts("tinykv record codec: all tests passed");
    return 0;
}
