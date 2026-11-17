/* Internal in-memory key index ("keydir"). Not part of the public API. */
#ifndef TINYKV_TKV_INDEX_H
#define TINYKV_TKV_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define TKV_INDEX_EMPTY 0u
#define TKV_INDEX_LIVE 1u
#define TKV_INDEX_TOMBSTONE 2u

/* One key slot. LIVE entries own their malloc'd key copy. The offset and
 * lengths describe the newest record for the key inside the append log. */
typedef struct tkv_entry {
    uint8_t *key;
    uint32_t key_length;
    uint32_t hash;
    uint64_t offset;
    uint32_t record_length;
    uint32_t value_length;
    uint8_t state;
} tkv_entry;

typedef struct tkv_index {
    tkv_entry *buckets; /* power-of-two sized, open addressing */
    size_t capacity;
    size_t live; /* LIVE entries */
    size_t used; /* LIVE + TOMBSTONE slots */
} tkv_index;

typedef void (*tkv_index_visitor)(tkv_entry *entry, void *user);

uint32_t tkv_index_hash(const uint8_t *key, size_t key_length);
int tkv_index_init(tkv_index *index);
void tkv_index_free(tkv_index *index);
void tkv_index_clear(tkv_index *index);
size_t tkv_index_count(const tkv_index *index);
tkv_entry *tkv_index_find(const tkv_index *index, const uint8_t *key,
                          size_t key_length);
/* Insert or update; returns 0 on success, -1 on allocation failure or
 * invalid arguments (NULL key with nonzero length, key beyond 32 bits). */
int tkv_index_put(tkv_index *index, const uint8_t *key, size_t key_length,
                  uint64_t offset, uint32_t record_length,
                  uint32_t value_length);
/* Returns 1 if the key was removed, 0 if it was not present. */
int tkv_index_remove(tkv_index *index, const uint8_t *key, size_t key_length);
void tkv_index_foreach(const tkv_index *index, tkv_index_visitor visit,
                       void *user);
void tkv_index_foreach_mutable(tkv_index *index, tkv_index_visitor visit,
                               void *user);
#endif
