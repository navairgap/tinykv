#include "tkv_index.h"
#include <stdlib.h>
#include <string.h>

#define TKV_INDEX_INITIAL_CAPACITY 64u

/* FNV-1a, 32-bit: tiny, fast, and stable across releases. */
uint32_t tkv_index_hash(const uint8_t *key, size_t key_length)
{
    uint32_t hash = 2166136261u;
    size_t i;
    if (key != NULL)
        for (i = 0; i < key_length; ++i) {
            hash ^= key[i];
            hash *= 16777619u;
        }
    return hash;
}

int tkv_index_init(tkv_index *index)
{
    if (index == NULL) return -1;
    index->buckets = (tkv_entry *)calloc(TKV_INDEX_INITIAL_CAPACITY,
                                         sizeof(tkv_entry));
    if (index->buckets == NULL) return -1;
    index->capacity = TKV_INDEX_INITIAL_CAPACITY;
    index->live = 0;
    index->used = 0;
    return 0;
}

void tkv_index_free(tkv_index *index)
{
    size_t i;
    if (index == NULL || index->buckets == NULL) return;
    for (i = 0; i < index->capacity; ++i)
        free(index->buckets[i].key);
    free(index->buckets);
    index->buckets = NULL;
    index->capacity = index->live = index->used = 0;
}

void tkv_index_clear(tkv_index *index)
{
    size_t i;
    if (index == NULL || index->buckets == NULL) return;
    for (i = 0; i < index->capacity; ++i) {
        free(index->buckets[i].key);
        index->buckets[i].key = NULL;
    }
    memset(index->buckets, 0, index->capacity * sizeof(tkv_entry));
    index->live = index->used = 0;
}

size_t tkv_index_count(const tkv_index *index)
{
    return index == NULL ? 0 : index->live;
}

static size_t index_probe_start(const tkv_index *index, uint32_t hash)
{
    return (size_t)hash & (index->capacity - 1);
}

tkv_entry *tkv_index_find(const tkv_index *index, const uint8_t *key,
                          size_t key_length)
{
    uint32_t hash;
    size_t i;
    if (index == NULL || index->buckets == NULL ||
        (key == NULL && key_length != 0) || key_length > UINT32_MAX)
        return NULL;
    hash = tkv_index_hash(key, key_length);
    for (i = index_probe_start(index, hash);; i = (i + 1) & (index->capacity - 1)) {
        const tkv_entry *slot = &index->buckets[i];
        if (slot->state == TKV_INDEX_EMPTY) return NULL;
        if (slot->state == TKV_INDEX_LIVE && slot->hash == hash &&
            slot->key_length == key_length &&
            (key_length == 0 || memcmp(slot->key, key, key_length) == 0))
            return &index->buckets[i];
    }
}

static int index_grow(tkv_index *index)
{
    tkv_entry *larger;
    size_t larger_capacity = index->capacity * 2, i;
    larger = (tkv_entry *)calloc(larger_capacity, sizeof(tkv_entry));
    if (larger == NULL) return -1;
    for (i = 0; i < index->capacity; ++i) {
        tkv_entry *slot = &index->buckets[i];
        size_t j;
        if (slot->state != TKV_INDEX_LIVE) continue;
        j = (size_t)slot->hash & (larger_capacity - 1);
        while (larger[j].state != TKV_INDEX_EMPTY)
            j = (j + 1) & (larger_capacity - 1);
        larger[j] = *slot;
    }
    free(index->buckets);
    index->buckets = larger;
    index->capacity = larger_capacity;
    index->used = index->live;
    return 0;
}

int tkv_index_put(tkv_index *index, const uint8_t *key, size_t key_length,
                  uint64_t offset, uint32_t record_length,
                  uint32_t value_length)
{
    uint32_t hash;
    size_t i, insert_at;
    int have_insert_at = 0;

    if (index == NULL || index->buckets == NULL ||
        (key == NULL && key_length != 0) || key_length > UINT32_MAX)
        return -1;
    hash = tkv_index_hash(key, key_length);
    if ((index->used + 1) * 4 > index->capacity * 3 &&
        index_grow(index) != 0)
        return -1;

    for (i = index_probe_start(index, hash);; i = (i + 1) & (index->capacity - 1)) {
        tkv_entry *slot = &index->buckets[i];
        if (slot->state == TKV_INDEX_EMPTY) {
            if (!have_insert_at) insert_at = i;
            break;
        }
        if (slot->state == TKV_INDEX_LIVE && slot->hash == hash &&
            slot->key_length == key_length &&
            (key_length == 0 || memcmp(slot->key, key, key_length) == 0)) {
            slot->offset = offset;
            slot->record_length = record_length;
            slot->value_length = value_length;
            return 0;
        }
        if (slot->state == TKV_INDEX_TOMBSTONE && !have_insert_at) {
            insert_at = i;
            have_insert_at = 1;
        }
    }

    {
        tkv_entry *slot = &index->buckets[insert_at];
        uint8_t *copy = (uint8_t *)malloc(key_length ? key_length : 1);
        if (copy == NULL) return -1;
        if (key_length != 0) memcpy(copy, key, key_length);
        slot->key = copy;
        slot->key_length = (uint32_t)key_length;
        slot->hash = hash;
        slot->offset = offset;
        slot->record_length = record_length;
        slot->value_length = value_length;
        slot->state = TKV_INDEX_LIVE;
        if (!have_insert_at) index->used++; /* reusing a tombstone keeps used */
        index->live++;
    }
    return 0;
}

int tkv_index_remove(tkv_index *index, const uint8_t *key, size_t key_length)
{
    tkv_entry *entry = tkv_index_find(index, key, key_length);
    if (entry == NULL) return 0;
    free(entry->key);
    entry->key = NULL;
    entry->key_length = 0;
    entry->state = TKV_INDEX_TOMBSTONE;
    index->live--;
    return 1;
}

void tkv_index_foreach(const tkv_index *index, tkv_index_visitor visit,
                       void *user)
{
    size_t i;
    if (index == NULL || index->buckets == NULL || visit == NULL) return;
    for (i = 0; i < index->capacity; ++i)
        if (index->buckets[i].state == TKV_INDEX_LIVE)
            visit(&index->buckets[i], user);
}
