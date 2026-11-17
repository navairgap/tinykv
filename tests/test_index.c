#include "tkv_index.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_put_find_update(void)
{
    tkv_index index;
    tkv_entry *entry;
    assert(tkv_index_init(&index) == 0);
    assert(tkv_index_count(&index) == 0);
    assert(tkv_index_find(&index, (const uint8_t *)"a", 1) == NULL);

    assert(tkv_index_put(&index, (const uint8_t *)"alpha", 5, 17, 22, 0) == 0);
    entry = tkv_index_find(&index, (const uint8_t *)"alpha", 5);
    assert(entry != NULL && entry->offset == 17 && entry->record_length == 22);
    assert(entry->key_length == 5 && memcmp(entry->key, "alpha", 5) == 0);

    /* Updating an existing key replaces the location without growing live. */
    assert(tkv_index_put(&index, (const uint8_t *)"alpha", 5, 99, 30, 5) == 0);
    assert(tkv_index_count(&index) == 1);
    entry = tkv_index_find(&index, (const uint8_t *)"alpha", 5);
    assert(entry != NULL && entry->offset == 99 && entry->value_length == 5);
    tkv_index_free(&index);
}

static void test_remove_then_reinsert(void)
{
    tkv_index index;
    assert(tkv_index_init(&index) == 0);
    assert(tkv_index_put(&index, (const uint8_t *)"gone", 4, 10, 10, 2) == 0);
    assert(tkv_index_remove(&index, (const uint8_t *)"gone", 4) == 1);
    assert(tkv_index_remove(&index, (const uint8_t *)"gone", 4) == 0);
    assert(tkv_index_count(&index) == 0);
    assert(tkv_index_find(&index, (const uint8_t *)"gone", 4) == NULL);

    /* Reinsertion must reuse the tombstone slot and stay findable. */
    assert(tkv_index_put(&index, (const uint8_t *)"gone", 4, 40, 9, 1) == 0);
    assert(tkv_index_count(&index) == 1);
    assert(tkv_index_find(&index, (const uint8_t *)"gone", 4) != NULL);
    tkv_index_free(&index);
}

static void test_rejects_invalid_arguments(void)
{
    tkv_index index;
    assert(tkv_index_init(&index) == 0);
    assert(tkv_index_put(&index, NULL, 3, 0, 0, 0) == -1);
    assert(tkv_index_put(NULL, (const uint8_t *)"k", 1, 0, 0, 0) == -1);
    assert(tkv_index_find(&index, NULL, 1) == NULL);
    assert(tkv_index_init(NULL) == -1);
    assert(tkv_index_remove(&index, (const uint8_t *)"missing", 7) == 0);
    tkv_index_free(&index);
    tkv_index_free(NULL);
}

int main(void)
{
    test_put_find_update();
    test_remove_then_reinsert();
    test_rejects_invalid_arguments();
    puts("tinykv index: all tests passed");
    return 0;
}
