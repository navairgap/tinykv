/* tinykv benchmark: measures the throughput of the public API.
 *
 * make bench                          (100k keys, 64-byte values, 1 thread)
 * ./build/bench_tinykv 50000 256 4    (50k keys, 256-byte values, 4 threads)
 *
 * Writes run with TINYKV_SYNC_NONE because per-write fsync measures the
 * disk, not tinykv; add your own policy numbers for real durability.
 * With threads > 1 the put and delete phases run on N threads over
 * disjoint key ranges through one shared handle; the handle lock
 * serializes engine work, so those numbers show contention, not
 * scaling. The get and reopen phases are single-threaded sweeps.
 */
#include "tinykv/db.h"
#include <pthread.h>
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

enum { MAX_THREADS = 16 };

enum bench_op { BENCH_PUT, BENCH_DELETE };

struct bench_args {
    tinykv_db *db;
    long start, end; /* key range [start, end) */
    const char *value;
    size_t value_size;
    enum bench_op op;
    int failed;
};

static void *bench_worker(void *argument)
{
    struct bench_args *args = (struct bench_args *)argument;
    char key[32];
    long i;
    for (i = args->start; i < args->end; ++i) {
        int len = snprintf(key, sizeof(key), "key-%08ld", i);
        if (args->op == BENCH_PUT) {
            if (tinykv_put(args->db, key, (size_t)len, args->value,
                           args->value_size) != TINYKV_OK) {
                args->failed = 1;
                return NULL;
            }
        } else {
            if (tinykv_delete(args->db, key, (size_t)len) != TINYKV_OK) {
                args->failed = 1;
                return NULL;
            }
        }
    }
    return NULL;
}

/* Run one phase over [0, count) on `threads` workers; returns 0 on
 * success. */
static int run_phase(tinykv_db *db, enum bench_op op, long count,
                     int threads, const char *value, size_t value_size,
                     pthread_t *workers, struct bench_args *args)
{
    long slice = count / threads, remainder = count % threads;
    long cursor = 0;
    int t;
    for (t = 0; t < threads; ++t) {
        args[t].db = db;
        args[t].start = cursor;
        args[t].end = cursor + slice + (t < remainder ? 1 : 0);
        cursor = args[t].end;
        args[t].value = value;
        args[t].value_size = value_size;
        args[t].op = op;
        args[t].failed = 0;
        if (threads > 1 &&
            pthread_create(&workers[t], NULL, bench_worker, &args[t]) != 0)
            return -1;
    }
    if (threads == 1)
        bench_worker(&args[0]);
    else
        for (t = 0; t < threads; ++t)
            pthread_join(workers[t], NULL);
    for (t = 0; t < threads; ++t)
        if (args[t].failed) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? strtol(argv[1], NULL, 10) : 100000;
    size_t value_size = argc > 2 ? (size_t)strtol(argv[2], NULL, 10) : 64;
    int threads = argc > 3 ? (int)strtol(argv[3], NULL, 10) : 1;
    tinykv_options options;
    tinykv_db *db = NULL;
    struct bench_args args[MAX_THREADS];
    pthread_t workers[MAX_THREADS];
    char *value = NULL;
    char key[32];
    double start;
    long i;
    tinykv_stats stats;
    int result = 0;

    if (count <= 0 || value_size == 0 || threads < 1 ||
        threads > MAX_THREADS) {
        fprintf(stderr, "usage: bench [count] [value_size] [threads 1-16]\n");
        return 1;
    }
    value = (char *)malloc(value_size);
    if (value == NULL) return 1;
    memset(value, 'x', value_size);
    remove("/tmp/tinykv-bench.tkv");

    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_NONE;
    options.create_if_missing = 1;
    options.read_only = 0;
    options.lock = 1;
    options.auto_compact_dead_ratio = 0.0;

    if (tinykv_open("/tmp/tinykv-bench.tkv", &options, &db) != TINYKV_OK) {
        fprintf(stderr, "bench: open failed\n");
        result = 1;
        goto out;
    }

    start = now_seconds();
    if (run_phase(db, BENCH_PUT, count, threads, value, value_size, workers,
                  args) != 0) {
        fprintf(stderr, "bench: put failed\n");
        result = 1;
        goto out_close;
    }
    report("put", count, now_seconds() - start);

    start = now_seconds();
    for (i = 0; i < count; ++i) {
        int len = snprintf(key, sizeof(key), "key-%08ld", i);
        void *got = NULL;
        size_t got_length = 0;
        if (tinykv_get(db, key, (size_t)len, &got, &got_length) != TINYKV_OK ||
            got_length != value_size) {
            fprintf(stderr, "bench: get failed\n");
            result = 1;
            goto out_close;
        }
        free(got);
    }
    report("get (malloc)", count, now_seconds() - start);

    tinykv_close(db);
    start = now_seconds();
    if (tinykv_open("/tmp/tinykv-bench.tkv", &options, &db) != TINYKV_OK) {
        fprintf(stderr, "bench: reopen failed\n");
        result = 1;
        goto out;
    }
    report("reopen (replay)", count, now_seconds() - start);

    start = now_seconds();
    if (run_phase(db, BENCH_DELETE, count, threads, NULL, 0, workers,
                  args) != 0) {
        fprintf(stderr, "bench: delete failed\n");
        result = 1;
        goto out_close;
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
    free(value);
    remove("/tmp/tinykv-bench.tkv");
    return result;
}
