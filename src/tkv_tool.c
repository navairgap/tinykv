/* tinykv — command line tool for tinykv databases. */
#include "tinykv/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *stream)
{
    fputs("tinykv " TINYKV_VERSION_STRING " — tiny embedded key-value store\n"
          "\n"
          "usage: tinykv <command> <database> [args]\n"
          "\n"
          "commands:\n"
          "  put <db> <key> <value>   store a value (NUL-free strings)\n"
          "  get <db> <key> [--raw]   print a value (--raw: no newline)\n"
          "  get <db> <key> --into=F  write the exact bytes to a new file\n"
          "  del <db> <key>           delete a key\n"
          "  exists <db> <key>        exit 0 if the key is present\n"
          "  list <db> [prefix]       print key<TAB>value, optionally filtered\n"
          "  keys <db> [prefix]       print live keys, optionally filtered\n"
          "  count <db> [prefix]      print the live key count, optionally filtered\n"
          "  stats <db> [--json]      print storage statistics\n"
          "  compact <db>             rewrite the log, dropping dead bytes\n"
          "  export <db> <file>       dump key<TAB>value lines to a new file\n"
          "  import <db> <file>       load key<TAB>value lines from a file\n"
          "  batch <db> [--sync=policy]\n"
          "                           import stdin lines of key<TAB>value;\n"
          "                           policy: none, close (default), every\n"
          "  copy <db> <new-db>       consistent snapshot into a new file\n"
          "  verify <db> [--json]     check the log for damage\n"
          "  repair <db> [--yes] [--max-key=N] [--max-value=N]\n"
          "                           repair a damaged log (confirm with --yes)\n"
          "  version                  print the version\n",
          stream);
}

static int stdout_dead;

/* Emit bytes to stdout, flipping a flag instead of exiting mid-stream
 * so the tool can finish cleanly and report the I/O failure. */
static void emit(const void *bytes, size_t length)
{
    if (length != 0 && fwrite(bytes, 1, length, stdout) != length)
        stdout_dead = 1;
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

static void read_only_options(tinykv_options *options)
{
    options->max_key_length = 4096;
    options->max_value_length = 1048576;
    options->sync = TINYKV_SYNC_EVERY_WRITE;
    options->create_if_missing = 0;
    options->read_only = 1;
    options->lock = 0;
    options->auto_compact_dead_ratio = 0.0;
    options->file_mode = 0600;
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
    int raw = 0;
    const char *into = NULL;
    FILE *stream;
    struct stat st;
    /* cmd_* helpers receive the command name in argv[0]: plain get is
     * argc 3, get --raw or get --into=FILE is argc 4. */
    if (argc == 4) {
        if (strcmp(argv[3], "--raw") == 0) raw = 1;
        else if (strncmp(argv[3], "--into=", 7) == 0) into = argv[3] + 7;
        else return usage(stderr), 1;
    } else if (argc != 3) {
        return usage(stderr), 1;
    }
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_get(db, argv[2], strlen(argv[2]), &value, &value_length);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    if (into != NULL) {
        /* Binary-safe output: the exact stored bytes, nothing added. */
        if (stat(into, &st) == 0) {
            fprintf(stderr, "tinykv: %s already exists; --into refuses to overwrite\n",
                    into);
            free(value);
            return 1;
        }
        stream = fopen(into, "wb");
        if (stream == NULL) {
            fprintf(stderr, "tinykv: %s: cannot open output file\n", into);
            free(value);
            return 6;
        }
        if (value_length != 0 &&
            fwrite(value, 1, value_length, stream) != value_length) {
            fprintf(stderr, "tinykv: %s: write failed\n", into);
            fclose(stream);
            free(value);
            return 6;
        }
        if (fflush(stream) != 0 || ferror(stream)) {
            fprintf(stderr, "tinykv: %s: write failed\n", into);
            fclose(stream);
            free(value);
            return 6;
        }
        fclose(stream);
        free(value);
        return 0;
    }
    emit(value, value_length);
    if (!raw && putchar('\n') == EOF) stdout_dead = 1;
    free(value);
    if (stdout_dead)
        return fprintf(stderr, "tinykv: write to stdout failed\n"), 6;
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
    if (argc < 2 || argc > 3) return usage(stderr), 1;
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    if (argc == 3)
        status = tinykv_iter_new_prefix(db, argv[2], strlen(argv[2]), &iter);
    else
        status = tinykv_iter_new(db, &iter);
    if (status == TINYKV_OK) {
        while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
               key != NULL) {
            emit(key, klen);
            if (putchar('\t') == EOF) stdout_dead = 1;
            emit(value, vlen);
            if (putchar('\n') == EOF) stdout_dead = 1;
            if (stdout_dead) break;
        }
        tinykv_iter_free(iter);
    }
    tinykv_close(db);
    if (stdout_dead)
        return fprintf(stderr, "tinykv: write to stdout failed\n"), 6;
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    return 0;
}

/* Print live keys, optionally only those under a prefix. */
static int cmd_keys(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_iter *iter = NULL;
    tinykv_options options;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    if (argc < 2 || argc > 3) return usage(stderr), 1;
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    if (argc == 3)
        status = tinykv_iter_new_prefix(db, argv[2], strlen(argv[2]), &iter);
    else
        status = tinykv_iter_new(db, &iter);
    if (status != TINYKV_OK) {
        report(argv[1], status);
        tinykv_close(db);
        return exit_code_for(status);
    }
    while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
           key != NULL) {
        emit(key, klen);
        if (putchar('\n') == EOF) stdout_dead = 1;
        if (stdout_dead) break;
    }
    tinykv_iter_free(iter);
    tinykv_close(db);
    if (stdout_dead)
        return fprintf(stderr, "tinykv: write to stdout failed\n"), 6;
    return 0;
}

static int cmd_exists(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_options options;
    if (argc != 3) return usage(stderr), 1;
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_exists(db, argv[2], strlen(argv[2]));
    tinykv_close(db);
    if (status == TINYKV_OK) {
        printf("exists\n");
        return 0;
    }
    if (status == TINYKV_ERR_NOT_FOUND) return 2;
    return report(argv[1], status), exit_code_for(status);
}

static int cmd_count(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    uint64_t count = 0;
    tinykv_options options;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    tinykv_iter *iter = NULL;
    if (argc < 2 || argc > 3) return usage(stderr), 1;
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    if (argc == 2) {
        status = tinykv_count(db, &count);
    } else {
        /* Prefix-filtered count: walk the prefix snapshot. */
        status = tinykv_iter_new_prefix(db, argv[2], strlen(argv[2]), &iter);
        if (status == TINYKV_OK) {
            count = 0;
            while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) ==
                       TINYKV_OK &&
                   key != NULL)
                count++;
            tinykv_iter_free(iter);
            status = TINYKV_OK;
        }
    }
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
    int as_json = 0;
    const char *database;
    if (argc < 2 || argc > 3) return usage(stderr), 1;
    database = argv[1];
    if (argc == 3) {
        if (strcmp(argv[2], "--json") != 0) return usage(stderr), 1;
        as_json = 1;
    }
    read_only_options(&options);
    status = tinykv_open(database, &options, &db);
    if (status != TINYKV_OK) return report(database, status), exit_code_for(status);
    status = tinykv_get_stats(db, &stats);
    tinykv_close(db);
    if (status != TINYKV_OK) return report(database, status), exit_code_for(status);
    if (as_json)
        printf("{\n"
               "  \"records\": %llu,\n"
               "  \"file_size\": %llu,\n"
               "  \"live_bytes\": %llu,\n"
               "  \"dead_bytes\": %llu,\n"
               "  \"discarded_bytes\": %llu,\n"
               "  \"puts\": %llu,\n"
               "  \"deletes\": %llu,\n"
               "  \"compactions\": %llu,\n"
               "  \"index_memory_bytes\": %llu,\n"
               "  \"version\": \"%s\"\n"
               "}\n",
               (unsigned long long)stats.records,
               (unsigned long long)stats.file_size,
               (unsigned long long)stats.live_bytes,
               (unsigned long long)stats.dead_bytes,
               (unsigned long long)stats.discarded_bytes,
               (unsigned long long)stats.puts,
               (unsigned long long)stats.deletes,
               (unsigned long long)stats.compactions,
               (unsigned long long)stats.index_memory_bytes,
               tinykv_version_string());
    else
        printf("records:         %llu\n"
               "file_size:       %llu\n"
               "live_bytes:      %llu\n"
               "dead_bytes:      %llu\n"
               "discarded_bytes: %llu\n"
               "puts:            %llu\n"
               "deletes:         %llu\n"
               "compactions:     %llu\n"
               "index_memory:    %llu\n",
               (unsigned long long)stats.records,
               (unsigned long long)stats.file_size,
               (unsigned long long)stats.live_bytes,
               (unsigned long long)stats.dead_bytes,
               (unsigned long long)stats.discarded_bytes,
               (unsigned long long)stats.puts,
               (unsigned long long)stats.deletes,
               (unsigned long long)stats.compactions,
               (unsigned long long)stats.index_memory_bytes);
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

/* Shared TAB-separated importer: trims CR/LF, splits on the first tab,
 * skips empty and tab-less lines. */
static tinykv_status import_lines(tinykv_db *db, FILE *stream,
                                  unsigned long long *imported,
                                  unsigned long long *skipped,
                                  unsigned long long *failed_line)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t got;
    tinykv_status status = TINYKV_OK;
    unsigned long long line_number = 0;
    while ((got = getline(&line, &capacity, stream)) >= 0) {
        char *tab, *key = line, *value;
        size_t key_length, value_length;
        line_number++;
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r'))
            line[--got] = '\0';
        if (got == 0) continue;
        tab = strchr(line, '\t');
        if (tab == NULL) {
            (*skipped)++;
            continue;
        }
        *tab = '\0';
        key_length = strlen(key);
        value = tab + 1;
        value_length = strlen(value);
        status = tinykv_put(db, key, key_length, value, value_length);
        if (status != TINYKV_OK) {
            fprintf(stderr,
                    "tinykv: line %llu: put failed for key '%s': %s\n",
                    line_number, key, tinykv_status_string(status));
            if (failed_line != NULL) *failed_line = line_number;
            break;
        }
        (*imported)++;
    }
    free(line);
    return status;
}

static tinykv_status open_for_load(const char *database,
                                   tinykv_sync_policy policy,
                                   tinykv_db **out_db)
{
    tinykv_options options;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = policy;
    options.create_if_missing = 1;
    options.read_only = 0;
    options.lock = 1;
    options.auto_compact_dead_ratio = 0.0;
    options.file_mode = 0600;
    return tinykv_open(database, &options, out_db);
}

static int finish_load(const char *database, tinykv_db *db,
                       unsigned long long imported,
                       unsigned long long skipped,
                       unsigned long long failed_line,
                       tinykv_status status)
{
    tinykv_close(db);
    if (status != TINYKV_OK) {
        if (failed_line != 0)
            fprintf(stderr, "tinykv: %s: stopped at input line %llu\n",
                    database, failed_line);
        return report(database, status), exit_code_for(status);
    }
    printf("imported %llu key(s)%s\n", imported,
           skipped ? " (some lines skipped: no TAB)" : "");
    return 0;
}

static int cmd_batch(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_options options;
    unsigned long long imported = 0, skipped = 0;
    const char *database;
    if (argc < 2 || argc > 4) return usage(stderr), 1;
    database = argv[1];
    /* Bulk loading prefers throughput; one sync at close. */
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_ON_CLOSE;
    options.create_if_missing = 1;
    options.read_only = 0;
    options.lock = 1;
    options.auto_compact_dead_ratio = 0.0;
    options.file_mode = 0600;
    if (argc == 3) {
        if (strncmp(argv[2], "--sync=", 7) != 0) return usage(stderr), 1;
        if (strcmp(argv[2] + 7, "none") == 0)
            options.sync = TINYKV_SYNC_NONE;
        else if (strcmp(argv[2] + 7, "close") == 0)
            options.sync = TINYKV_SYNC_ON_CLOSE;
        else if (strcmp(argv[2] + 7, "every") == 0)
            options.sync = TINYKV_SYNC_EVERY_WRITE;
        else
            return usage(stderr), 1;
    } else if (argc > 3) {
        return usage(stderr), 1;
    }
    unsigned long long failed_line = 0;
    status = open_for_load(database, options.sync, &db);
    if (status != TINYKV_OK) return report(database, status), exit_code_for(status);
    status = import_lines(db, stdin, &imported, &skipped, &failed_line);
    return finish_load(database, db, imported, skipped, failed_line, status);
}

/* Import from a TAB-separated file. */
static int cmd_import(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    unsigned long long imported = 0, skipped = 0, failed_line = 0;
    FILE *stream;
    if (argc != 3) return usage(stderr), 1;
    status = open_for_load(argv[1], TINYKV_SYNC_ON_CLOSE, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    stream = fopen(argv[2], "r");
    if (stream == NULL) {
        fprintf(stderr, "tinykv: %s: cannot open input file\n", argv[2]);
        tinykv_close(db);
        return 6;
    }
    status = import_lines(db, stream, &imported, &skipped, &failed_line);
    fclose(stream);
    return finish_load(argv[1], db, imported, skipped, failed_line, status);
}

/* Dump every live key/value as TAB-separated lines (batch-compatible). */
static int cmd_export(int argc, char **argv)
{
    tinykv_db *db = NULL;
    tinykv_status status;
    tinykv_iter *iter = NULL;
    tinykv_options options;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    FILE *stream;
    struct stat st;
    if (argc != 3) return usage(stderr), 1;
    if (stat(argv[2], &st) == 0) {
        fprintf(stderr, "tinykv: %s already exists; export refuses to overwrite\n",
                argv[2]);
        return 1;
    }
    read_only_options(&options);
    status = tinykv_open(argv[1], &options, &db);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_iter_new(db, &iter);
    if (status != TINYKV_OK) {
        report(argv[1], status);
        tinykv_close(db);
        return exit_code_for(status);
    }
    stream = fopen(argv[2], "w");
    if (stream == NULL) {
        fprintf(stderr, "tinykv: %s: cannot open output file\n", argv[2]);
        tinykv_iter_free(iter);
        tinykv_close(db);
        return 6;
    }
    while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
           key != NULL) {
        fwrite(key, 1, klen, stream);
        fputc('\t', stream);
        fwrite(value, 1, vlen, stream);
        fputc('\n', stream);
    }
    tinykv_iter_free(iter);
    tinykv_close(db);
    if (fflush(stream) != 0 || ferror(stream)) {
        fprintf(stderr, "tinykv: %s: write failed\n", argv[2]);
        fclose(stream);
        return 6;
    }
    fclose(stream);
    return 0;
}

/* Copy every live key/value into a brand-new file: a consistent
 * snapshot taken through the verified read path. */
static int cmd_copy(int argc, char **argv)
{
    tinykv_db *source = NULL, *destination = NULL;
    tinykv_status status;
    tinykv_iter *iter = NULL;
    tinykv_options source_options, destination_options;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    unsigned long long copied = 0;
    if (argc != 3) return usage(stderr), 1;
    read_only_options(&source_options);
    status = tinykv_open(argv[1], &source_options, &source);
    if (status != TINYKV_OK) return report(argv[1], status), exit_code_for(status);
    status = tinykv_iter_new(source, &iter);
    if (status != TINYKV_OK) {
        report(argv[1], status);
        tinykv_close(source);
        return exit_code_for(status);
    }
    destination_options = source_options;
    destination_options.create_if_missing = 1;
    destination_options.read_only = 0;
    destination_options.lock = 1;
    destination_options.sync = TINYKV_SYNC_ON_CLOSE;
    {
        struct stat st;
        if (stat(argv[2], &st) == 0) {
            fprintf(stderr, "tinykv: %s already exists; copy refuses to overwrite\n",
                    argv[2]);
            tinykv_iter_free(iter);
            tinykv_close(source);
            return 1;
        }
    }
    status = tinykv_open(argv[2], &destination_options, &destination);
    if (status != TINYKV_OK) {
        report(argv[2], status);
        tinykv_iter_free(iter);
        tinykv_close(source);
        return exit_code_for(status);
    }
    while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
           key != NULL) {
        status = tinykv_put(destination, key, klen, value, vlen);
        if (status != TINYKV_OK) {
            report(argv[2], status);
            tinykv_iter_free(iter);
            tinykv_close(destination);
            tinykv_close(source);
            return exit_code_for(status);
        }
        copied++;
    }
    tinykv_iter_free(iter);
    tinykv_close(destination);
    tinykv_close(source);
    printf("copied %llu key(s) from %s to %s\n", copied, argv[1], argv[2]);
    return 0;
}

/* Parse repeated --max-key=N / --max-value=N options for verify and
 * repair, so databases opened with custom limits can be maintained. */
static int parse_limit_flags(int argc, char **argv, int start,
                             uint32_t *max_key, uint32_t *max_value)
{
    int i;
    *max_key = 4096;
    *max_value = 1048576;
    for (i = start; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "--yes") == 0)
            continue; /* handled by the caller */
        if (strncmp(argv[i], "--max-key=", 10) == 0) {
            char *end;
            unsigned long parsed = strtoul(argv[i] + 10, &end, 10);
            if (*end != '\0' || parsed == 0 || parsed > UINT32_MAX)
                return -1;
            *max_key = (uint32_t)parsed;
        } else if (strncmp(argv[i], "--max-value=", 12) == 0) {
            char *end;
            unsigned long parsed = strtoul(argv[i] + 12, &end, 10);
            if (*end != '\0' || parsed == 0 || parsed > UINT32_MAX)
                return -1;
            *max_value = (uint32_t)parsed;
        } else {
            return -1;
        }
    }
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    uint64_t last_good = 0;
    tinykv_status status;
    int as_json = 0;
    uint32_t max_key, max_value;
    const char *database;
    if (argc < 2 || argc > 4) return usage(stderr), 1;
    database = argv[1];
    if (parse_limit_flags(argc, argv, 2, &max_key, &max_value) != 0)
        return usage(stderr), 1;
    if (strcmp(argv[argc - 1], "--json") == 0) as_json = 1;
    if (argc >= 3 && strcmp(argv[2], "--json") == 0) as_json = 1;
    status = tinykv_verify(database, max_key, max_value, &last_good);
    {
        /* Torn tails leave reclaimable bytes; report the gap so
         * operators can size the repair. */
        struct stat st;
        uint64_t discarded = 0;
        if (status == TINYKV_ERR_TORN_TAIL && stat(database, &st) == 0 &&
            (uint64_t)st.st_size > last_good)
            discarded = (uint64_t)st.st_size - last_good;
        if (as_json) {
            printf("{\n"
                   "  \"status\": \"%s\",\n"
                   "  \"last_good_offset\": %llu,\n"
                   "  \"discarded_bytes\": %llu\n"
                   "}\n",
                   tinykv_status_string(status),
                   (unsigned long long)last_good,
                   (unsigned long long)discarded);
            return exit_code_for(status);
        }
        if (status == TINYKV_OK)
            printf("ok: log replays cleanly (%llu bytes)\n",
                   (unsigned long long)last_good);
        else if (status == TINYKV_ERR_TORN_TAIL)
            printf("torn tail: last verified offset %llu, %llu bytes "
                   "reclaimable (repair available)\n",
                   (unsigned long long)last_good,
                   (unsigned long long)discarded);
        else if (status == TINYKV_ERR_CORRUPT)
            printf("corruption: last verified offset %llu\n",
                   (unsigned long long)last_good);
        else
            report(argv[1], status);
        return exit_code_for(status);
    }
}

static int cmd_repair(int argc, char **argv)
{
    uint64_t last_good = 0;
    tinykv_status status;
    int confirm = 0;
    uint32_t max_key, max_value;
    const char *database;
    int i;
    if (argc < 2 || argc > 4) return usage(stderr), 1;
    database = argv[1];
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--yes") == 0) confirm = 1;
        else if (strncmp(argv[i], "--max-key=", 10) == 0 ||
                 strncmp(argv[i], "--max-value=", 12) == 0)
            continue;
        else
            return usage(stderr), 1;
    }
    if (parse_limit_flags(argc, argv, 2, &max_key, &max_value) != 0)
        return usage(stderr), 1;
    status = tinykv_verify(database, max_key, max_value, &last_good);
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
    status = tinykv_repair(database, max_key, max_value, 0, &last_good);
    if (status != TINYKV_OK) return report(database, status), exit_code_for(status);
    printf("repaired: %s now replays to offset %llu\n",
           database, (unsigned long long)last_good);
    return 0;
}

int main(int argc, char **argv)
{
    int result;
    if (argc < 2) return usage(stderr), 1;
    if (strcmp(argv[1], "version") == 0) {
        printf("tinykv %s\n", tinykv_version_string());
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0)
        return usage(stdout), 0;
    if (strcmp(argv[1], "put") == 0) result = cmd_put(argc - 1, argv + 1);
    else if (strcmp(argv[1], "get") == 0) result = cmd_get(argc - 1, argv + 1);
    else if (strcmp(argv[1], "del") == 0 || strcmp(argv[1], "delete") == 0)
        result = cmd_delete(argc - 1, argv + 1);
    else if (strcmp(argv[1], "exists") == 0) result = cmd_exists(argc - 1, argv + 1);
    else if (strcmp(argv[1], "list") == 0) result = cmd_list(argc - 1, argv + 1);
    else if (strcmp(argv[1], "keys") == 0) result = cmd_keys(argc - 1, argv + 1);
    else if (strcmp(argv[1], "export") == 0) result = cmd_export(argc - 1, argv + 1);
    else if (strcmp(argv[1], "import") == 0) result = cmd_import(argc - 1, argv + 1);
    else if (strcmp(argv[1], "count") == 0) result = cmd_count(argc - 1, argv + 1);
    else if (strcmp(argv[1], "stats") == 0) result = cmd_stats(argc - 1, argv + 1);
    else if (strcmp(argv[1], "compact") == 0) result = cmd_compact(argc - 1, argv + 1);
    else if (strcmp(argv[1], "batch") == 0) result = cmd_batch(argc - 1, argv + 1);
    else if (strcmp(argv[1], "copy") == 0) result = cmd_copy(argc - 1, argv + 1);
    else if (strcmp(argv[1], "verify") == 0) result = cmd_verify(argc - 1, argv + 1);
    else if (strcmp(argv[1], "repair") == 0) result = cmd_repair(argc - 1, argv + 1);
    else {
        fprintf(stderr, "tinykv: unknown command '%s'\n", argv[1]);
        usage(stderr);
        return 1;
    }
    /* Buffered output can fail only at the exit flush (ENOSPC, closed
     * consumer), so check the stream once before reporting success. */
    if (fflush(stdout) != 0 || ferror(stdout)) {
        if (!stdout_dead)
            fprintf(stderr, "tinykv: write to stdout failed\n");
        return 6;
    }
    return result;
}
