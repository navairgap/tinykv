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

static void test_rejects_invalid_delete_payload_and_record_type(void)
{
    static const uint8_t key[] = "k", value[] = "v";
    uint8_t *encoded = (uint8_t *)1;
    size_t length = 99;

    assert(tinykv_record_encode(TINYKV_RECORD_DELETE, key, 1, value, 1,
        64, 64, &encoded, &length) == TINYKV_RECORD_INVALID_FORMAT);
    assert(encoded == NULL && length == 0);

    encoded = (uint8_t *)1;
    length = 99;
    assert(tinykv_record_encode(255, key, 1, NULL, 0,
        64, 64, &encoded, &length) == TINYKV_RECORD_INVALID_TYPE);
    assert(encoded == NULL && length == 0);
}

static void test_exact_limit_boundaries(void)
{
    enum { KEY_LIMIT = 8, VALUE_LIMIT = 16 };
    static uint8_t max_key[KEY_LIMIT], max_value[VALUE_LIMIT];
    /* Oversized inputs need real storage of their own: a length one
     * past the limit must never read past a limit-sized buffer. */
    static uint8_t oversized_key[KEY_LIMIT + 1];
    static uint8_t oversized_value[VALUE_LIMIT + 1];
    uint8_t *encoded = NULL;
    size_t length = 0, consumed = 0;
    tinykv_record_view decoded;
    unsigned i;

    for (i = 0; i < KEY_LIMIT; ++i) max_key[i] = (uint8_t)(0x80 + i);
    for (i = 0; i <= KEY_LIMIT; ++i) oversized_key[i] = (uint8_t)(0x80 + i);
    for (i = 0; i < VALUE_LIMIT; ++i) max_value[i] = (uint8_t)(0xFF - i);
    for (i = 0; i <= VALUE_LIMIT; ++i) oversized_value[i] = (uint8_t)(0xFF - i);

    /* Exactly at the limit must encode and decode. */
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, max_key, KEY_LIMIT,
                                max_value, VALUE_LIMIT, KEY_LIMIT,
                                VALUE_LIMIT, &encoded, &length) ==
           TINYKV_RECORD_OK);
    assert(length == TINYKV_RECORD_HEADER_SIZE + KEY_LIMIT + VALUE_LIMIT);
    assert(tinykv_record_decode(encoded, length, KEY_LIMIT, VALUE_LIMIT,
                                &decoded, &consumed) == TINYKV_RECORD_OK);
    assert(consumed == length && decoded.key_length == KEY_LIMIT &&
           decoded.value_length == VALUE_LIMIT);
    assert(memcmp(decoded.key, max_key, KEY_LIMIT) == 0);
    assert(memcmp(decoded.value, max_value, VALUE_LIMIT) == 0);
    free(encoded);

    /* One byte beyond the limit is refused at encode. */
    encoded = (uint8_t *)1;
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, oversized_key,
                                KEY_LIMIT + 1, max_value, VALUE_LIMIT,
                                KEY_LIMIT, VALUE_LIMIT, &encoded, &length) ==
           TINYKV_RECORD_LIMIT_EXCEEDED);
    assert(encoded == NULL && length == 0);
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, max_key, KEY_LIMIT,
                                oversized_value, VALUE_LIMIT + 1, KEY_LIMIT,
                                VALUE_LIMIT, &encoded, &length) ==
           TINYKV_RECORD_LIMIT_EXCEEDED);

    /* A decoder with tighter limits rejects an oversized record. */
    assert(tinykv_record_encode(TINYKV_RECORD_PUT, oversized_key,
                                KEY_LIMIT + 1, max_value, VALUE_LIMIT,
                                1024, 1024, &encoded, &length) ==
           TINYKV_RECORD_OK);
    assert(tinykv_record_decode(encoded, length, KEY_LIMIT, VALUE_LIMIT,
                                &decoded, &consumed) ==
           TINYKV_RECORD_LIMIT_EXCEEDED);
    assert(consumed == 0);
    free(encoded);
}

int main(void)
{
    test_crc_vector();
    test_put_round_trip();
    test_delete_round_trip();
    test_rejects_truncation_and_checksum_corruption();
    test_null_input_clears_decode_outputs();
    test_enforces_limits_before_payload_read();
    test_rejects_invalid_delete_payload_and_record_type();
    test_exact_limit_boundaries();
    puts("tinykv record codec: all tests passed");
    return 0;
}
