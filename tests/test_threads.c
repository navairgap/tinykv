/* Concurrency smoke test: one handle shared by several threads. */
#include "tinykv/db.h"
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(TINYKV_NO_THREADS)
int main(void)
{
    puts("tinykv threads: disabled (built with TINYKV_NO_THREADS)");
    return 0;
}
#else

static char temp_dir[64] = "/tmp/tinykv-threads-XXXXXX";

enum { THREADS = 4, KEYS_PER_THREAD = 2000 };

struct worker_state {
    tinykv_db *db;
    int id;
    unsigned long long puts, deletes, gets_ok, gets_missing;
};

static void *worker(void *argument)
{
    struct worker_state *state = (struct worker_state *)argument;
    char key[32], value[32];
    int i;
    for (i = 0; i < KEYS_PER_THREAD; ++i) {
        void *got = NULL;
        size_t got_length = 0;
        snprintf(key, sizeof(key), "t%d-key-%05d", state->id, i);
        snprintf(value, sizeof(value), "t%d-val-%05d", state->id, i);
        assert(tinykv_put(state->db, key, strlen(key), value,
                          strlen(value)) == TINYKV_OK);
        state->puts++;
        if (i % 3 == 0) {
            assert(tinykv_delete(state->db, key, strlen(key)) == TINYKV_OK);
            state->deletes++;
            assert(tinykv_get(state->db, key, strlen(key), &got,
                              &got_length) == TINYKV_ERR_NOT_FOUND);
            state->gets_missing++;
        } else {
            assert(tinykv_get(state->db, key, strlen(key), &got,
                              &got_length) == TINYKV_OK);
            assert(got_length == strlen(value));
            assert(memcmp(got, value, got_length) == 0);
            free(got);
            state->gets_ok++;
        }
    }
    return NULL;
}

static void remove_temp_dir(void)
{
    DIR *dir = opendir(temp_dir);
    struct dirent *item;
    char entry[384];
    assert(dir != NULL);
    while ((item = readdir(dir)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        snprintf(entry, sizeof(entry), "%s/%s", temp_dir, item->d_name);
        assert(unlink(entry) == 0);
    }
    closedir(dir);
    assert(rmdir(temp_dir) == 0);
}

int main(void)
{
    tinykv_db *db = NULL;
#ifndef TINYKV_NO_THREADS
    pthread_t threads[THREADS];
#endif
    struct worker_state states[THREADS];
    uint64_t count = 0, expected = 0;
    int t;
    assert(mkdtemp(temp_dir) != NULL);
    {
        char path[128];
        tinykv_options options;
        snprintf(path, sizeof(path), "%s/concurrent.db", temp_dir);
        memset(&options, 0, sizeof(options)); /* every field set explicitly */
        options.max_key_length = 4096;
        options.max_value_length = 1048576;
        options.sync = TINYKV_SYNC_NONE;
        options.create_if_missing = 1;
        options.read_only = 0;
        options.lock = 1;
        assert(tinykv_open(path, &options, &db) == TINYKV_OK);
    }
    for (t = 0; t < THREADS; ++t) {
        states[t].db = db;
        states[t].id = t;
        states[t].puts = states[t].deletes = 0;
        states[t].gets_ok = states[t].gets_missing = 0;
#if defined(TINYKV_NO_THREADS)
        /* No internal locking: the caller serializes, so run the
         * workers one at a time — same operations, same expectations. */
        worker(&states[t]);
#else
        assert(pthread_create(&threads[t], NULL, worker, &states[t]) == 0);
#endif
    }
#if !defined(TINYKV_NO_THREADS)
    for (t = 0; t < THREADS; ++t)
        assert(pthread_join(threads[t], NULL) == 0);
#endif

    for (t = 0; t < THREADS; ++t) {
        assert(states[t].puts == KEYS_PER_THREAD);
        /* i % 3 == 0 deletes ceil(N / 3) keys per thread. */
        expected += KEYS_PER_THREAD - (KEYS_PER_THREAD + 2) / 3;
    }
    assert(tinykv_count(db, &count) == TINYKV_OK);
    assert(count == expected);

    /* Every surviving value must still read correctly. */
    for (t = 0; t < THREADS; ++t) {
        int i;
        for (i = 0; i < KEYS_PER_THREAD; ++i) {
            char key[32], value[32];
            void *got = NULL;
            size_t got_length = 0;
            snprintf(key, sizeof(key), "t%d-key-%05d", t, i);
            snprintf(value, sizeof(value), "t%d-val-%05d", t, i);
            if (i % 3 == 0)
                continue;
            assert(tinykv_get(db, key, strlen(key), &got, &got_length) ==
                   TINYKV_OK);
            assert(got_length == strlen(value) &&
                   memcmp(got, value, got_length) == 0);
            free(got);
        }
    }
    /* Prefix iteration after the concurrency storm: every surviving
     * key in a per-thread namespace must be visible and correct. */
    for (t = 0; t < THREADS; ++t) {
        char prefix[16];
        tinykv_iter *iter = NULL;
        const void *key = NULL, *value = NULL;
        size_t klen = 0, vlen = 0;
        int seen = 0, expected = 0;
        int i;
        snprintf(prefix, sizeof(prefix), "t%d-", t);
        assert(tinykv_iter_new_prefix(db, prefix, strlen(prefix), &iter) ==
               TINYKV_OK);
        while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) ==
                   TINYKV_OK &&
               key != NULL) {
            assert(klen == 12); /* "tN-key-NNNNN" */
            assert(memcmp(key, prefix, strlen(prefix)) == 0);
            seen++;
        }
        for (i = 0; i < KEYS_PER_THREAD; ++i)
            if (i % 3 != 0) expected++;
        assert(seen == expected);
        tinykv_iter_free(iter);
    }
    tinykv_close(db);
    remove_temp_dir();
    puts("tinykv threads: all tests passed");
    return 0;
}
#endif
