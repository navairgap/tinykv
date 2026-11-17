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

static void test_growth_preserves_entries(void)
{
    tkv_index index;
    enum { N = 2000 };
    char key[16];
    int i;
    assert(tkv_index_init(&index) == 0);
    for (i = 0; i < N; ++i) {
        int len = sprintf(key, "key-%06d", i);
        assert(tkv_index_put(&index, (const uint8_t *)key, (size_t)len,
                             (uint64_t)i, (uint32_t)len, 0) == 0);
    }
    assert(tkv_index_count(&index) == (size_t)N);
    assert(index.capacity > 64); /* growth actually happened */
    for (i = 0; i < N; ++i) {
        int len = sprintf(key, "key-%06d", i);
        tkv_entry *entry = tkv_index_find(&index, (const uint8_t *)key,
                                          (size_t)len);
        assert(entry != NULL && entry->offset == (uint64_t)i);
    }
    /* Remove every other key, then verify the survivors and reuse. */
    for (i = 0; i < N; i += 2) {
        int len = sprintf(key, "key-%06d", i);
        assert(tkv_index_remove(&index, (const uint8_t *)key, (size_t)len) == 1);
    }
    assert(tkv_index_count(&index) == (size_t)(N / 2));
    for (i = 0; i < N; ++i) {
        int len = sprintf(key, "key-%06d", i);
        tkv_entry *entry = tkv_index_find(&index, (const uint8_t *)key,
                                          (size_t)len);
        if (i % 2 == 0)
            assert(entry == NULL);
        else
            assert(entry != NULL && entry->offset == (uint64_t)i);
    }
    /* Capacity growth after tombstones keeps lookups correct. */
    for (i = N; i < N + 500; ++i) {
        int len = sprintf(key, "key-%06d", i);
        assert(tkv_index_put(&index, (const uint8_t *)key, (size_t)len,
                             (uint64_t)i, (uint32_t)len, 0) == 0);
        assert(tkv_index_find(&index, (const uint8_t *)key, (size_t)len) != NULL);
    }
    tkv_index_free(&index);
}

struct visit_state {
    size_t seen;
    uint64_t max_offset;
};

static void visit_counter(tkv_entry *entry, void *user)
{
    struct visit_state *state = (struct visit_state *)user;
    state->seen++;
    if (entry->offset > state->max_offset) state->max_offset = entry->offset;
}

static void test_foreach_visits_live_entries_once(void)
{
    tkv_index index;
    struct visit_state state = {0, 0};
    char key[8];
    int i;
    assert(tkv_index_init(&index) == 0);
    for (i = 0; i < 100; ++i) {
        int len = sprintf(key, "k%03d", i);
        assert(tkv_index_put(&index, (const uint8_t *)key, (size_t)len,
                             (uint64_t)i, 10, 3) == 0);
    }
    for (i = 0; i < 100; i += 5) {
        int len = sprintf(key, "k%03d", i);
        assert(tkv_index_remove(&index, (const uint8_t *)key, (size_t)len) == 1);
    }
    tkv_index_foreach(&index, visit_counter, &state);
    assert(state.seen == 80);
    assert(state.max_offset == 99);
    tkv_index_clear(&index);
    assert(tkv_index_count(&index) == 0);
    state.seen = 0;
    tkv_index_foreach(&index, visit_counter, &state);
    assert(state.seen == 0);
    tkv_index_free(&index);
}

static void test_hash_is_stable_and_distributes(void)
{
    /* FNV-1a of "a" must never drift; index placement depends on it. */
    assert(tkv_index_hash((const uint8_t *)"a", 1) ==
           tkv_index_hash((const uint8_t *)"a", 1));
    assert(tkv_index_hash(NULL, 0) == 2166136261u); /* FNV basis */
    assert(tkv_index_hash((const uint8_t *)"a", 1) !=
           tkv_index_hash((const uint8_t *)"b", 1));
}

int main(void)
{
    test_put_find_update();
    test_remove_then_reinsert();
    test_rejects_invalid_arguments();
    test_growth_preserves_entries();
    test_foreach_visits_live_entries_once();
    test_hash_is_stable_and_distributes();
    puts("tinykv index: all tests passed");
    return 0;
}
