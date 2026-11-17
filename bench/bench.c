/* tinykv benchmark: measures the throughput of the public API.
 *
 * make bench                 (defaults: 100000 keys, 64-byte values)
 * ./build/bench_tinykv 50000 256
 *
 * Writes run with TINYKV_SYNC_NONE because per-write fsync measures the
 * disk, not tinykv; add your own policy numbers for real durability.
 */
#include "tinykv/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void report(const char *what, long count, double seconds)
{
    if (seconds > 0)
        printf("%-18s %10ld ops  %8.1f ops/s  %7.3f s\n", what, count,
               (double)count / seconds, seconds);
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? strtol(argv[1], NULL, 10) : 100000;
    size_t value_size = argc > 2 ? (size_t)strtol(argv[2], NULL, 10) : 64;
    tinykv_options options;
    tinykv_db *db = NULL;
    char *key = NULL, *value = NULL;
    void *got = NULL;
    size_t got_length = 0;
    double start;
    long i;
    tinykv_stats stats;
    int result = 0;

    if (count <= 0 || value_size == 0) {
        fprintf(stderr, "usage: bench [count] [value_size]\n");
        return 1;
    }
    key = (char *)malloc(32);
    value = (char *)malloc(value_size);
    if (key == NULL || value == NULL) return 1;
    memset(value, 'x', value_size);
    remove("/tmp/tinykv-bench.tkv");

    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_NONE;
    options.create_if_missing = 1;
    options.read_only = 0;
    options.lock = 1;

    if (tinykv_open("/tmp/tinykv-bench.tkv", &options, &db) != TINYKV_OK) {
        fprintf(stderr, "bench: open failed\n");
        result = 1;
        goto out;
    }

    start = now_seconds();
    for (i = 0; i < count; ++i) {
        int len = snprintf(key, 32, "key-%08ld", i);
        if (tinykv_put(db, key, (size_t)len, value, value_size) != TINYKV_OK) {
            fprintf(stderr, "bench: put failed\n");
            result = 1;
            goto out_close;
        }
    }
    report("put", count, now_seconds() - start);

    start = now_seconds();
    for (i = 0; i < count; ++i) {
        int len = snprintf(key, 32, "key-%08ld", i);
        if (tinykv_get(db, key, (size_t)len, &got, &got_length) != TINYKV_OK ||
            got_length != value_size) {
            fprintf(stderr, "bench: get failed\n");
            result = 1;
            goto out_close;
        }
        free(got);
        got = NULL;
    }
    report("get (malloc)", count, now_seconds() - start);

    start = now_seconds();
    for (i = 0; i < count; ++i) {
        int len = snprintf(key, 32, "key-%08ld", i);
        if (tinykv_get_into(db, key, (size_t)len, value, value_size,
                            &got_length) != TINYKV_OK ||
            got_length != value_size) {
            fprintf(stderr, "bench: get_into failed\n");
            result = 1;
            goto out_close;
        }
    }
    report("get_into", count, now_seconds() - start);

    tinykv_close(db);
    start = now_seconds();
    if (tinykv_open("/tmp/tinykv-bench.tkv", &options, &db) != TINYKV_OK) {
        fprintf(stderr, "bench: reopen failed\n");
        result = 1;
        goto out;
    }
    report("reopen (replay)", count, now_seconds() - start);

    start = now_seconds();
    for (i = 0; i < count; ++i) {
        int len = snprintf(key, 32, "key-%08ld", i);
        if (tinykv_delete(db, key, (size_t)len) != TINYKV_OK) {
            fprintf(stderr, "bench: delete failed\n");
            result = 1;
            goto out_close;
        }
    }
    report("delete", count, now_seconds() - start);

    start = now_seconds();
    if (tinykv_compact(db) != TINYKV_OK) {
        fprintf(stderr, "bench: compact failed\n");
        result = 1;
        goto out_close;
    }
    report("compact", 1, now_seconds() - start);

    if (tinykv_get_stats(db, &stats) == TINYKV_OK && stats.records != 0) {
        fprintf(stderr, "bench: compact left %llu records\n",
                (unsigned long long)stats.records);
        result = 1;
    }
    printf("log size after compact: %llu bytes\n",
           (unsigned long long)stats.file_size);

out_close:
    tinykv_close(db);
out:
    free(key);
    free(value);
    remove("/tmp/tinykv-bench.tkv");
    return result;
}
