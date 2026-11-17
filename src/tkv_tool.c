/* tinykv — command line tool for tinykv databases. */
#include "tinykv/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    fputs("tinykv " TINYKV_VERSION_STRING " — tiny embedded key-value store\n"
          "\n"
          "usage: tinykv <command> <database> [args]\n"
          "\n"
          "commands:\n"
          "  put <db> <key> <value>   store a value (NUL-free strings)\n"
          "  get <db> <key>           print a value\n"
          "  del <db> <key>           delete a key\n"
          "  list <db>                print key<TAB>value for every key\n"
          "  count <db>               print the number of live keys\n"
          "  stats <db>               print storage statistics\n"
          "  compact <db>             rewrite the log, dropping dead bytes\n"
          "  verify <db>              check the log for damage\n"
          "  repair <db> [--yes]      repair a damaged log (confirm with --yes)\n"
          "  version                  print the version\n",
          stream);
}

static int exit_code_for(tinykv_status status)
{
    switch (status) {
    case TINYKV_OK: return 0;
    case TINYKV_ERR_NOT_FOUND: return 2;
    case TINYKV_ERR_TORN_TAIL: return 3;
    case TINYKV_ERR_CORRUPT: return 4;
    case TINYKV_ERR_LOCKED: return 5;
    case TINYKV_ERR_IO: return 6;
    default: return 1;
    }
}

static void report(const char *database, tinykv_status status)
{
    fprintf(stderr, "tinykv: %s: %s\n", database,
            tinykv_status_string(status));
}

static int cmd_put(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    if (argc != 4) return usage(stderr), 1;
    status = tinykv_open(argv[1], NULL, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_put(db, argv[2], strlen(argv[2]), argv[3], strlen(argv[3]));
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_options options;
    void *value = NULL;
    size_t value_length = 0;
    if (argc != 3) return usage(stderr), 1;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 0;
    options.read_only = 1;
    options.lock = 0;
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_get(db, argv[2], strlen(argv[2]), &value, &value_length);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    if (value_length != 0 && fwrite(value, 1, value_length, stdout) != value_length)
        return fprintf(stderr, "tinykv: write to stdout failed\n"), 6;
    putchar('\n');
    free(value);
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    if (argc != 3) return usage(stderr), 1;
    status = tinykv_open(argv[1], NULL, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_delete(db, argv[2], strlen(argv[2]));
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_iter *iter = NULL;
    tinykv_options options;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    if (argc != 2) return usage(stderr), 1;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 0;
    options.read_only = 1;
    options.lock = 0;
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_iter_new(db, &iter);
    if (status == TINYKV_OK) {
        while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
               key != NULL) {
            fwrite(key, 1, klen, stdout);
            putchar('\t');
            fwrite(value, 1, vlen, stdout);
            putchar('\n');
        }
        tinykv_iter_free(iter);
    }
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    return 0;
}

static int cmd_count(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    uint64_t count = 0;
    tinykv_options options;
    if (argc != 2) return usage(stderr), 1;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 0;
    options.read_only = 1;
    options.lock = 0;
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_count(db, &count);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    printf("%llu\n", (unsigned long long)count);
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_stats stats;
    tinykv_options options;
    if (argc != 2) return usage(stderr), 1;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 0;
    options.read_only = 1;
    options.lock = 0;
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_get_stats(db, &stats);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    printf("records:         %llu\n"
           "file_size:       %llu\n"
           "live_bytes:      %llu\n"
           "dead_bytes:      %llu\n"
           "discarded_bytes: %llu\n"
           "puts:            %llu\n"
           "deletes:         %llu\n"
           "compactions:     %llu\n",
           (unsigned long long)stats.records,
           (unsigned long long)stats.file_size,
           (unsigned long long)stats.live_bytes,
           (unsigned long long)stats.dead_bytes,
           (unsigned long long)stats.discarded_bytes,
           (unsigned long long)stats.puts,
           (unsigned long long)stats.deletes,
           (unsigned long long)stats.compactions);
    return 0;
}

static int cmd_compact(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    if (argc != 2) return usage(stderr), 1;
    status = tinykv_open(argv[1], NULL, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_compact(db);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    printf("compacted %s\n", argv[1]);
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    uint64_t last_good = 0;
    tinykv_status status;
    if (argc != 2) return usage(stderr), 1;
    status = tinykv_verify(argv[1], 4096, 1048576, &last_good);
    if (status == TINYKV_OK)
        printf("ok: log replays cleanly (%llu bytes)\n",
               (unsigned long long)last_good);
    else if (status == TINYKV_ERR_TORN_TAIL)
        printf("torn tail: last verified offset %llu (repair available)\n",
               (unsigned long long)last_good);
    else if (status == TINYKV_ERR_CORRUPT)
        printf("corruption: last verified offset %llu\n",
               (unsigned long long)last_good);
    else
        report(argv[1], status);
    return exit_code_for(status);
}

static int cmd_repair(int argc, char **argv)
{
    uint64_t last_good = 0;
    tinykv_status status;
    int confirm = 0;
    const char *database;
    if (argc < 2 || argc > 3) return usage(stderr), 1;
    database = argv[1];
    if (argc == 3) {
        if (strcmp(argv[2], "--yes") != 0) return usage(stderr), 1;
        confirm = 1;
    }
    status = tinykv_verify(database, 4096, 1048576, &last_good);
    if (status == TINYKV_OK) {
        printf("%s: clean, nothing to repair\n", database);
        return 0;
    }
    printf("database: %s\nproblem:  %s\n",
           database, tinykv_status_string(status));
    if (status == TINYKV_ERR_CORRUPT)
        printf("plan:     keep records before offset %llu, drop the rest\n",
               (unsigned long long)last_good);
    else
        printf("plan:     truncate to last verified offset %llu\n",
               (unsigned long long)last_good);
    printf("backup:   %s.bak keeps the original bytes\n", database);
    if (!confirm) {
        printf("dry run:  rerun with --yes to apply\n");
        return exit_code_for(status);
    }
    status = tinykv_repair(database, 4096, 1048576, 0, &last_good);
    if (status != TINYKV_OK) return report(database, status), exit_code_for(status);
    printf("repaired: %s now replays to offset %llu\n",
           database, (unsigned long long)last_good);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage(stderr), 1;
    if (strcmp(argv[1], "version") == 0) {
        printf("tinykv %s\n", tinykv_version_string());
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0)
        return usage(stdout), 0;
    if (strcmp(argv[1], "put") == 0) return cmd_put(argc - 1, argv + 1);
    if (strcmp(argv[1], "get") == 0) return cmd_get(argc - 1, argv + 1);
    if (strcmp(argv[1], "del") == 0 || strcmp(argv[1], "delete") == 0)
        return cmd_delete(argc - 1, argv + 1);
    if (strcmp(argv[1], "list") == 0) return cmd_list(argc - 1, argv + 1);
    if (strcmp(argv[1], "count") == 0) return cmd_count(argc - 1, argv + 1);
    if (strcmp(argv[1], "stats") == 0) return cmd_stats(argc - 1, argv + 1);
    if (strcmp(argv[1], "compact") == 0) return cmd_compact(argc - 1, argv + 1);
    if (strcmp(argv[1], "verify") == 0) return cmd_verify(argc - 1, argv + 1);
    if (strcmp(argv[1], "repair") == 0) return cmd_repair(argc - 1, argv + 1);
    fprintf(stderr, "tinykv: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return 1;
}
