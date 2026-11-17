#include "tinykv/db.h"
#include "tkv_io.h"
#include "tkv_log.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[64] = "/tmp/tinykv-db-XXXXXX";

/* Mirror of the library defaults, kept local to the tests. */
static tinykv_options tinykv_options_default_for_test(void)
{
    tinykv_options options;
    options.max_key_length = 4096;
    options.max_value_length = 1048576;
    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 1;
    options.read_only = 0;
    options.lock = 1;
    return options;
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

static void db_path(char *buf, size_t size, const char *suffix)
{
    snprintf(buf, size, "%s/%s", temp_dir, suffix);
}

static void test_open_creates_private_file(void)
{
    char path[128];
    tinykv_db *db = NULL;
    struct stat st;
    db_path(path, sizeof(path), "fresh.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(db != NULL);
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    assert(tinykv_close(db) == TINYKV_OK);
    /* Reopening an existing file works and reports no creation. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    tinykv_close(db);
}

static void test_open_rejects_bad_arguments(void)
{
    tinykv_db *db = (tinykv_db *)0x1;
    tinykv_options options;
    char path[128];
    db_path(path, sizeof(path), "args.db");
    assert(tinykv_open(path, NULL, NULL) == TINYKV_ERR_INVALID);
    assert(tinykv_open(NULL, NULL, &db) == TINYKV_ERR_INVALID);
    assert(tinykv_open("", NULL, &db) == TINYKV_ERR_INVALID);
    assert(db == (tinykv_db *)0x1); /* untouched on failure */

    options = tinykv_options_default_for_test();
    options.max_key_length = 0;
    assert(tinykv_open(path, &options, &db) == TINYKV_ERR_INVALID);
    options.max_key_length = 4096;
    options.max_value_length = 0;
    assert(tinykv_open(path, &options, &db) == TINYKV_ERR_INVALID);
    options.max_value_length = 1024;
    options.sync = (tinykv_sync_policy)77;
    assert(tinykv_open(path, &options, &db) == TINYKV_ERR_INVALID);
    assert(access(path, F_OK) == -1); /* never created */

    options.sync = TINYKV_SYNC_EVERY_WRITE;
    options.create_if_missing = 0;
    assert(tinykv_open(path, &options, &db) == TINYKV_ERR_IO);
    options.read_only = 1;
    assert(tinykv_open(path, &options, &db) == TINYKV_ERR_IO);
    assert(tinykv_close(NULL) == TINYKV_OK);
}

static void test_second_writer_is_locked_out(void)
{
    char path[128];
    tinykv_db *first = NULL, *second = NULL;
    tinykv_options options;
    db_path(path, sizeof(path), "lock.db");
    assert(tinykv_open(path, NULL, &first) == TINYKV_OK);
    assert(tinykv_open(path, NULL, &second) == TINYKV_ERR_LOCKED);
    /* Opening without the lock is allowed. */
    options = tinykv_options_default_for_test();
    options.lock = 0;
    assert(tinykv_open(path, &options, &second) == TINYKV_OK);
    tinykv_close(second);
    /* Read-only handles never take the write lock. */
    options.read_only = 1;
    assert(tinykv_open(path, &options, &second) == TINYKV_OK);
    tinykv_close(second);
    tinykv_close(first);
    /* After close the lock is free again. */
    assert(tinykv_open(path, NULL, &first) == TINYKV_OK);
    tinykv_close(first);
}

static void test_corrupt_log_fails_open(void)
{
    char path[128];
    tinykv_db *db = NULL;
    int fd;
    db_path(path, sizeof(path), "corrupt.db");
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    /* A full header-sized run of junk is corruption, not a torn tail. */
    assert(tkv_write_all(fd, "JUNKJUNKJUNKJUNKJUNK", 20) == 0);
    close(fd);
    assert(tinykv_open(path, NULL, &db) == TINYKV_ERR_CORRUPT);
    assert(db == NULL);
    assert(tinykv_open(path, NULL, &db) == TINYKV_ERR_CORRUPT);
}

static void test_torn_tail_opens_cleanly(void)
{
    char path[128];
    tinykv_db *db = NULL;
    int fd;
    uint8_t partial[5] = {'T', 'K', 'V', '1', TINYKV_RECORD_PUT};
    db_path(path, sizeof(path), "torn.db");
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    /* Five valid header bytes then EOF is an incomplete record. */
    assert(tkv_write_all(fd, partial, sizeof(partial)) == 0);
    close(fd);
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(db != NULL);
    tinykv_close(db);
    /* Read-only opens tolerate the same tail without touching the file. */
    {
        tinykv_options options = tinykv_options_default_for_test();
        options.read_only = 1;
        assert(tinykv_open(path, &options, &db) == TINYKV_OK);
        tinykv_close(db);
    }
}

static void test_version_and_status_strings(void)
{
    assert(tinykv_version_string() != NULL);
    assert(strcmp(tinykv_version_string(), "1.0.0") == 0);
    assert(strcmp(tinykv_status_string(TINYKV_OK), "ok") == 0);
    assert(strcmp(tinykv_status_string((tinykv_status)999),
                  "unknown status") == 0);
}

static void test_replay_builds_index(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tkv_log_limits limits = {4096, 1048576};
    uint64_t offset, count = 0;
    size_t length;
    int fd;
    db_path(path, sizeof(path), "replay.db");
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    /* A log with duplicates, tombstones, and empty values. */
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "", 0,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_DELETE, "gone", 4, NULL, 0,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "two", 3,
                          &limits, &offset, &length) == 0);
    close(fd);

    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_count(db, &count) == TINYKV_OK);
    assert(count == 2); /* alpha and beta survive; gone was never live */
    tinykv_close(db);

    /* Deterministic replay: reopening yields the same state. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_count(db, &count) == TINYKV_OK);
    assert(count == 2);
    tinykv_close(db);
}

static void test_replay_over_limit_is_corruption(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tkv_log_limits loose = {4096, 1048576};
    uint64_t offset;
    size_t length;
    int fd;
    db_path(path, sizeof(path), "limits.db");
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "big", 3, "", 0,
                          &loose, &offset, &length) == 0);
    /* Claim a 4 GiB value. The decoder checks configured limits before the
     * checksum, so the record is rejected as an invalid length. */
    {
        uint8_t header[TINYKV_RECORD_HEADER_SIZE];
        assert(tkv_pread_all(fd, header, sizeof(header), 0) == 0);
        header[9] = 0xFF;
        header[10] = 0xFF;
        header[11] = 0xFF;
        header[12] = 0xFF;
        assert(pwrite(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    }
    close(fd);
    /* Open with a 64 KiB value limit: a record claiming 4 GiB is invalid. */
    {
        tinykv_options options = tinykv_options_default_for_test();
        options.max_value_length = 65536;
        assert(tinykv_open(path, &options, &db) == TINYKV_ERR_CORRUPT);
    }
}

static void test_put_get_delete_roundtrip(void)
{
    char path[128];
    tinykv_db *db = NULL;
    void *value = NULL;
    size_t value_length = 0;
    /* Binary key and value with embedded NULs and high bytes. */
    static const uint8_t binary_key[] = {'b', 0, 'i', 'n', 0xFF};
    static const uint8_t binary_value[] = {0, 1, 2, 0, 254, 255};
    db_path(path, sizeof(path), "roundtrip.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);

    assert(tinykv_get(db, "missing", 7, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    assert(value == NULL && value_length == 0);

    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 3 && memcmp(value, "one", 3) == 0);
    free(value);

    /* Overwrites resolve to the newest value. */
    assert(tinykv_put(db, "alpha", 5, "second-value", 12) == TINYKV_OK);
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 12 && memcmp(value, "second-value", 12) == 0);
    free(value);

    /* Empty values round-trip as a non-NULL zero-length result. */
    assert(tinykv_put(db, "empty", 5, NULL, 0) == TINYKV_OK);
    assert(tinykv_get(db, "empty", 5, &value, &value_length) == TINYKV_OK);
    assert(value != NULL && value_length == 0);
    free(value);

    /* Binary payloads survive the log verbatim. */
    assert(tinykv_put(db, binary_key, sizeof(binary_key), binary_value,
                      sizeof(binary_value)) == TINYKV_OK);
    assert(tinykv_get(db, binary_key, sizeof(binary_key), &value,
                      &value_length) == TINYKV_OK);
    assert(value_length == sizeof(binary_value));
    assert(memcmp(value, binary_value, sizeof(binary_value)) == 0);
    free(value);

    /* Deletes remove the key; deleting it again reports not found. */
    assert(tinykv_delete(db, "alpha", 5) == TINYKV_OK);
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    assert(tinykv_delete(db, "alpha", 5) == TINYKV_ERR_NOT_FOUND);
    assert(tinykv_delete(db, "never-existed", 13) == TINYKV_ERR_NOT_FOUND);

    tinykv_close(db);
}

static void test_zero_length_keys(void)
{
    char path[128];
    tinykv_db *db = NULL;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "emptykey.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, NULL, 0, "root", 4) == TINYKV_OK);
    assert(tinykv_get(db, NULL, 0, &value, &value_length) == TINYKV_OK);
    assert(value_length == 4 && memcmp(value, "root", 4) == 0);
    free(value);
    assert(tinykv_put(db, NULL, 0, "new-root", 8) == TINYKV_OK);
    assert(tinykv_get(db, NULL, 0, &value, &value_length) == TINYKV_OK);
    assert(value_length == 8);
    free(value);
    tinykv_close(db);
}

static void test_write_argument_validation(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_options options;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "validate.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);

    assert(tinykv_put(db, NULL, 3, "v", 1) == TINYKV_ERR_INVALID);
    assert(tinykv_put(db, "k", 1, NULL, 5) == TINYKV_ERR_INVALID);
    assert(tinykv_put(NULL, "k", 1, "v", 1) == TINYKV_ERR_INVALID);
    assert(tinykv_get(db, NULL, 3, &value, &value_length) ==
           TINYKV_ERR_INVALID);
    assert(tinykv_get(db, "k", 1, NULL, &value_length) ==
           TINYKV_ERR_INVALID);
    assert(value == NULL && value_length == 0);

    options = tinykv_options_default_for_test();
    options.max_key_length = 4;
    options.max_value_length = 8;
    tinykv_close(db);
    db_path(path, sizeof(path), "limits2.db");
    assert(tinykv_open(path, &options, &db) == TINYKV_OK);
    assert(tinykv_put(db, "too-long-key", 12, "v", 1) == TINYKV_ERR_LIMIT);
    assert(tinykv_put(db, "k", 1, "too-long-value", 14) == TINYKV_ERR_LIMIT);
    assert(tinykv_count(db, (uint64_t[1]){0}) == TINYKV_OK);

    /* Read-only handles cannot write. */
    tinykv_close(db);
    options.read_only = 1;
    assert(tinykv_open(path, &options, &db) == TINYKV_OK);
    assert(tinykv_put(db, "k", 1, "v", 1) == TINYKV_ERR_INVALID);
    assert(tinykv_delete(db, "k", 1) == TINYKV_ERR_INVALID);
    tinykv_close(db);
}

static void test_write_reclaims_torn_tail(void)
{
    char path[128];
    tinykv_db *db = NULL;
    uint8_t partial[5] = {'T', 'K', 'V', '1', TINYKV_RECORD_PUT};
    int fd;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "reclaim.db");
    /* Seed one valid record through a real db handle. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "keep", 4, "me", 2) == TINYKV_OK);
    tinykv_close(db);
    /* Simulate a crash mid-append: five header bytes, no payload. */
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(lseek(fd, 0, SEEK_END) >= 0);
    assert(tkv_write_all(fd, partial, sizeof(partial)) == 0);
    close(fd);

    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    /* The torn tail is only reclaimed when the first write happens. */
    assert(tinykv_put(db, "after", 5, "crash", 5) == TINYKV_OK);
    tinykv_close(db);

    /* Replay sees the valid record, the new one, and no garbage. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_get(db, "keep", 4, &value, &value_length) == TINYKV_OK);
    assert(value_length == 2);
    free(value);
    assert(tinykv_get(db, "after", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 5 && memcmp(value, "crash", 5) == 0);
    free(value);
    tinykv_close(db);
}

static void test_restart_persistence(void)
{
    char path[128];
    tinykv_db *db = NULL;
    void *value = NULL;
    size_t value_length = 0;
    int i;
    char key[16];
    db_path(path, sizeof(path), "restart.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    for (i = 0; i < 100; ++i) {
        snprintf(key, sizeof(key), "key-%03d", i);
        assert(tinykv_put(db, key, strlen(key), key, strlen(key)) == TINYKV_OK);
    }
    assert(tinykv_delete(db, "key-050", strlen("key-050")) == TINYKV_OK);
    tinykv_close(db);

    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    for (i = 0; i < 100; ++i) {
        snprintf(key, sizeof(key), "key-%03d", i);
        if (i == 50) {
            assert(tinykv_get(db, key, strlen(key), &value, &value_length) ==
                   TINYKV_ERR_NOT_FOUND);
            continue;
        }
        assert(tinykv_get(db, key, strlen(key), &value, &value_length) ==
               TINYKV_OK);
        assert(value_length == strlen(key) && memcmp(value, key, value_length) == 0);
        free(value);
        value = NULL;
    }
    tinykv_close(db);
}

static void test_stats_track_activity(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_stats stats;
    int fd;
    uint8_t partial[5] = {'T', 'K', 'V', '1', TINYKV_RECORD_PUT};
    uint64_t count = 0;
    db_path(path, sizeof(path), "stats.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.records == 0 && stats.file_size == 0 && stats.puts == 0);

    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_put(db, "beta", 4, "two", 3) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "uno", 3) == TINYKV_OK);
    assert(tinykv_delete(db, "beta", 4) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.records == 1);
    assert(stats.puts == 3 && stats.deletes == 1);
    /* Only alpha's newest record is live (25 bytes). The first alpha, the
     * beta record, and the tombstone are dead. */
    assert(stats.live_bytes == 25);
    {
        struct stat st;
        assert(stat(path, &st) == 0);
        assert(stats.file_size == (uint64_t)st.st_size);
    }
    assert(stats.dead_bytes == stats.file_size - stats.live_bytes);
    assert(stats.discarded_bytes == 0);
    tinykv_close(db);

    /* A torn tail shows up as discarded bytes until the next write. */
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(lseek(fd, 0, SEEK_END) >= 0);
    assert(tkv_write_all(fd, partial, sizeof(partial)) == 0);
    close(fd);
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.discarded_bytes == sizeof(partial));
    assert(stats.dead_bytes >= stats.discarded_bytes);
    assert(tinykv_count(db, &count) == TINYKV_OK && count == 1);
    assert(tinykv_put(db, "gamma", 5, "three", 5) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.discarded_bytes == 0);
    tinykv_close(db);
}

static void test_compaction_reclaims_dead_bytes(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_stats stats;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "compact.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_put(db, "beta", 4, "two", 3) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "uno", 3) == TINYKV_OK);
    assert(tinykv_put(db, "gamma", 5, "three", 5) == TINYKV_OK);
    assert(tinykv_delete(db, "beta", 4) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.dead_bytes > 0);

    assert(tinykv_compact(db) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.records == 2);
    assert(stats.live_bytes == (17 + 5 + 3) + (17 + 5 + 5));
    assert(stats.dead_bytes == 0);
    assert(stats.file_size == stats.live_bytes);
    assert(stats.compactions == 1);
    /* Values must still read correctly through the new offsets. */
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 3 && memcmp(value, "uno", 3) == 0);
    free(value);
    assert(tinykv_get(db, "gamma", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 5 && memcmp(value, "three", 5) == 0);
    free(value);
    assert(tinykv_get(db, "beta", 4, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    /* Writes keep working after the swap. */
    assert(tinykv_put(db, "delta", 5, "four", 4) == TINYKV_OK);
    tinykv_close(db);

    /* The compacted file replays to the same state. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 3);
    free(value);
    assert(tinykv_get(db, "delta", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 4);
    free(value);
    tinykv_close(db);
}

static void test_compaction_with_torn_tail(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_stats stats;
    int fd;
    uint8_t partial[5] = {'T', 'K', 'V', '1', TINYKV_RECORD_PUT};
    db_path(path, sizeof(path), "compact-torn.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    tinykv_close(db);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(lseek(fd, 0, SEEK_END) >= 0);
    assert(tkv_write_all(fd, partial, sizeof(partial)) == 0);
    close(fd);

    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    /* Compaction is a write operation, so it reclaims the tail too. */
    assert(tinykv_compact(db) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.discarded_bytes == 0 && stats.dead_bytes == 0);
    assert(stats.file_size == 25);
    tinykv_close(db);
}

static void test_compaction_empty_database(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_stats stats;
    db_path(path, sizeof(path), "compact-empty.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_compact(db) == TINYKV_OK);
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.records == 0 && stats.file_size == 0 && stats.dead_bytes == 0);
    tinykv_close(db);
}

static uint64_t file_size_of(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return (uint64_t)st.st_size;
}

static void test_verify_classifies_logs(void)
{
    char path[128];
    tinykv_db *db = NULL;
    int fd;
    uint64_t good = 0;
    db_path(path, sizeof(path), "verify.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    tinykv_close(db);
    assert(tinykv_verify(path, 4096, 1048576, &good) == TINYKV_OK);
    assert(good == 25 && good == file_size_of(path));

    /* Torn tail: reported, not corruption. */
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(lseek(fd, 0, SEEK_END) >= 0);
    assert(tkv_write_all(fd, (const uint8_t[]){'T', 'K', 'V', '1', 1}, 5) == 0);
    close(fd);
    assert(tinykv_verify(path, 4096, 1048576, &good) == TINYKV_ERR_TORN_TAIL);
    assert(good == 25);

    /* Mid-file corruption: reported with the offending offset. */
    {
        int rw = tkv_open_log(path, 0, NULL);
        uint8_t scratch = 0x7F;
        assert(rw >= 0);
        assert(pwrite(rw, &scratch, 1, 20) == 1);
        close(rw);
    }
    assert(tinykv_verify(path, 4096, 1048576, &good) == TINYKV_ERR_CORRUPT);
    assert(good == 0);
    assert(tinykv_verify(NULL, 4096, 1048576, &good) == TINYKV_ERR_INVALID);
    assert(tinykv_verify(path, 0, 1048576, &good) == TINYKV_ERR_INVALID);
    assert(tinykv_verify("/does/not/exist.db", 4096, 1048576, &good) ==
           TINYKV_ERR_IO);
}

static void test_repair_is_non_destructive(void)
{
    char path[128], bak[160], repair[160];
    tinykv_db *db = NULL;
    uint64_t good = 0;
    int fd;
    db_path(path, sizeof(path), "repair.db");
    snprintf(bak, sizeof(bak), "%s.bak", path);
    snprintf(repair, sizeof(repair), "%s.repair", path);
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_put(db, "beta", 4, "two", 3) == TINYKV_OK);
    tinykv_close(db);

    /* A torn tail: 8 bytes of an interrupted record. */
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(lseek(fd, 0, SEEK_END) >= 0);
    assert(tkv_write_all(fd, (const uint8_t[]){'T', 'K', 'V', '1', 1, 0, 0, 0},
                         8) == 0);
    close(fd);

    /* Dry run changes nothing and creates no sidecars. */
    assert(tinykv_repair(path, 4096, 1048576, 1, &good) ==
           TINYKV_ERR_TORN_TAIL);
    assert(good == 49);
    assert(file_size_of(path) == 57);
    assert(access(bak, F_OK) == -1);
    assert(access(repair, F_OK) == -1);

    /* Real repair truncates to the last verified offset. */
    assert(tinykv_repair(path, 4096, 1048576, 0, &good) == TINYKV_OK);
    assert(good == 49);
    assert(file_size_of(path) == 49);
    assert(access(repair, F_OK) == -1);
    assert(file_size_of(bak) == 57); /* original preserved */
    /* The repaired file replays and both keys survive. */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    {
        void *value = NULL;
        size_t value_length = 0;
        assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
        assert(value_length == 3);
        free(value);
        assert(tinykv_get(db, "beta", 4, &value, &value_length) == TINYKV_OK);
        assert(value_length == 3);
        free(value);
    }
    tinykv_close(db);
    assert(unlink(bak) == 0);
}

static void test_repair_stops_at_midfile_corruption(void)
{
    char path[128], bak[132];
    tinykv_db *db = NULL;
    uint64_t good = 0;
    int fd;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "repair-corrupt.db");
    snprintf(bak, sizeof(bak), "%s.bak", path);
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_put(db, "beta", 4, "two", 3) == TINYKV_OK);
    assert(tinykv_put(db, "gamma", 5, "three", 5) == TINYKV_OK);
    tinykv_close(db);
    /* Corrupt the second record's payload (offset 25+17 lands in beta). */
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(pwrite(fd, (const uint8_t[]){0x7F}, 1, 42) == 1);
    close(fd);

    assert(tinykv_repair(path, 4096, 1048576, 0, &good) == TINYKV_OK);
    assert(good == 25); /* everything after the corruption is dropped */
    assert(file_size_of(bak) == 25 + 24 + 27); /* original preserved */
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_get(db, "alpha", 5, &value, &value_length) == TINYKV_OK);
    assert(value_length == 3);
    free(value);
    assert(tinykv_get(db, "beta", 4, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    assert(tinykv_get(db, "gamma", 5, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    tinykv_close(db);
    assert(unlink(bak) == 0);
}

static void test_repair_clean_log_and_lock(void)
{
    char path[128], bak[132];
    tinykv_db *db = NULL;
    uint64_t good = 0;
    db_path(path, sizeof(path), "repair-clean.db");
    snprintf(bak, sizeof(bak), "%s.bak", path);
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    /* Repair must refuse while a writer holds the lock. */
    assert(tinykv_repair(path, 4096, 1048576, 0, &good) == TINYKV_ERR_LOCKED);
    tinykv_close(db);
    /* A clean log repairs to a no-op without sidecars. */
    assert(tinykv_repair(path, 4096, 1048576, 0, &good) == TINYKV_OK);
    assert(good == 25);
    assert(access(bak, F_OK) == -1);
}

static void test_get_into_copies_without_allocation(void)
{
    char path[128];
    tinykv_db *db = NULL;
    char buffer[64];
    size_t value_length = 0;
    db_path(path, sizeof(path), "getinto.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "alpha", 5, "payload-bytes", 13) == TINYKV_OK);
    assert(tinykv_put(db, "empty", 5, NULL, 0) == TINYKV_OK);

    assert(tinykv_get_into(db, "alpha", 5, buffer, sizeof(buffer),
                           &value_length) == TINYKV_OK);
    assert(value_length == 13 && memcmp(buffer, "payload-bytes", 13) == 0);
    /* Exact-capacity read works. */
    assert(tinykv_get_into(db, "alpha", 5, buffer, 13, &value_length) ==
           TINYKV_OK);
    /* A short buffer reports the needed length. */
    assert(tinykv_get_into(db, "alpha", 5, buffer, 5, &value_length) ==
           TINYKV_ERR_LIMIT);
    assert(value_length == 13);
    /* Zero-length values need no buffer space. */
    assert(tinykv_get_into(db, "empty", 5, NULL, 0, &value_length) ==
           TINYKV_OK);
    assert(value_length == 0);
    /* Missing keys still report not found. */
    assert(tinykv_get_into(db, "missing", 7, buffer, sizeof(buffer),
                           &value_length) == TINYKV_ERR_NOT_FOUND);
    assert(tinykv_get_into(db, "alpha", 5, NULL, 10, &value_length) ==
           TINYKV_ERR_INVALID);
    tinykv_close(db);
}

static void test_iteration_is_sorted_and_skips_deletes(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_iter *iter = NULL;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    int i;
    const char *expected[] = {"key-01", "key-03", "key-04", "key-05"};
    db_path(path, sizeof(path), "iter.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_put(db, "key-03", 6, "v3", 2) == TINYKV_OK);
    assert(tinykv_put(db, "key-01", 6, "v1", 2) == TINYKV_OK);
    assert(tinykv_put(db, "key-05", 6, "v5", 2) == TINYKV_OK);
    assert(tinykv_put(db, "key-02", 6, "v2", 2) == TINYKV_OK);
    assert(tinykv_put(db, "key-04", 6, "v4", 2) == TINYKV_OK);
    assert(tinykv_put(db, "key-01", 6, "v1-new", 6) == TINYKV_OK);
    assert(tinykv_delete(db, "key-02", 6) == TINYKV_OK);

    assert(tinykv_iter_new(db, &iter) == TINYKV_OK);
    i = 0;
    while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
           key != NULL) {
        assert(i < 4);
        assert(klen == 6 && memcmp(key, expected[i], 6) == 0);
        if (i == 0)
            assert(vlen == 6 && memcmp(value, "v1-new", 6) == 0);
        else
            assert(vlen == 2);
        i++;
    }
    assert(i == 4); /* key-02 was deleted; no tombstone appears */
    tinykv_iter_free(iter);
    tinykv_iter_free(NULL);
    tinykv_close(db);
}

static void test_iteration_empty_and_fresh_values(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_iter *iter = NULL;
    const void *key = NULL, *value = NULL;
    size_t klen = 0, vlen = 0;
    db_path(path, sizeof(path), "iter-empty.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_iter_new(db, &iter) == TINYKV_OK);
    assert(tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK);
    assert(key == NULL && value == NULL);
    tinykv_iter_free(iter);

    /* Values are re-read on each step: a delete before the iterator
     * reaches a key makes the iterator skip it. */
    assert(tinykv_put(db, "a", 1, "1", 1) == TINYKV_OK);
    assert(tinykv_put(db, "b", 1, "2", 1) == TINYKV_OK);
    assert(tinykv_put(db, "c", 1, "3", 1) == TINYKV_OK);
    assert(tinykv_iter_new(db, &iter) == TINYKV_OK);
    assert(tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK);
    assert(klen == 1 && *(const char *)key == 'a');
    assert(tinykv_delete(db, "b", 1) == TINYKV_OK);
    assert(tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK);
    assert(klen == 1 && *(const char *)key == 'c'); /* b skipped */
    assert(vlen == 1 && *(const char *)value == '3');
    assert(tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK);
    assert(key == NULL);
    tinykv_iter_free(iter);
    tinykv_close(db);
}

static void test_accessors_report_path_and_size(void)
{
    char path[128];
    tinykv_db *db = NULL;
    const char *reported = NULL;
    uint64_t size = 0;
    db_path(path, sizeof(path), "accessors.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    assert(tinykv_path(db, &reported) == TINYKV_OK);
    assert(strcmp(reported, path) == 0);
    assert(tinykv_file_size(db, &size) == TINYKV_OK);
    assert(size == 0);
    assert(tinykv_put(db, "alpha", 5, "one", 3) == TINYKV_OK);
    assert(tinykv_file_size(db, &size) == TINYKV_OK);
    assert(size == TINYKV_RECORD_HEADER_SIZE + 8);
    assert(tinykv_compact(db) == TINYKV_OK);
    assert(tinykv_file_size(db, &size) == TINYKV_OK);
    assert(size == TINYKV_RECORD_HEADER_SIZE + 8);
    assert(tinykv_path(db, NULL) == TINYKV_ERR_INVALID);
    assert(tinykv_file_size(NULL, &size) == TINYKV_ERR_INVALID);
    tinykv_close(db);
}

static void test_max_length_boundaries(void)
{
    char path[128];
    tinykv_db *db = NULL;
    tinykv_options options;
    static uint8_t max_key[64], max_value[8192];
    /* Oversized probes need their own storage: a one-past-the-limit
     * length must never read beyond a limit-sized buffer. */
    static uint8_t oversized_key[65], oversized_value[8193];
    void *value = NULL;
    size_t value_length = 0;
    unsigned i;

    for (i = 0; i < sizeof(max_key); ++i) max_key[i] = (uint8_t)(i * 7 + 1);
    for (i = 0; i < sizeof(max_value); ++i) max_value[i] = (uint8_t)(i ^ 0xA5);
    for (i = 0; i < sizeof(oversized_key); ++i)
        oversized_key[i] = (uint8_t)(i * 7 + 1);
    for (i = 0; i < sizeof(oversized_value); ++i)
        oversized_value[i] = (uint8_t)(i ^ 0xA5);

    options = tinykv_options_default_for_test();
    options.max_key_length = sizeof(max_key);
    options.max_value_length = sizeof(max_value);
    db_path(path, sizeof(path), "boundary.db");
    assert(tinykv_open(path, &options, &db) == TINYKV_OK);
    /* Exactly at the limit is accepted. */
    assert(tinykv_put(db, max_key, sizeof(max_key), max_value,
                      sizeof(max_value)) == TINYKV_OK);
    assert(tinykv_get(db, max_key, sizeof(max_key), &value,
                      &value_length) == TINYKV_OK);
    assert(value_length == sizeof(max_value));
    assert(memcmp(value, max_value, sizeof(max_value)) == 0);
    free(value);
    /* One byte over is refused. */
    assert(tinykv_put(db, oversized_key, sizeof(oversized_key), "v", 1) ==
           TINYKV_ERR_LIMIT);
    assert(tinykv_put(db, "k", 1, oversized_value, sizeof(oversized_value)) ==
           TINYKV_ERR_LIMIT);
    /* The oversized key never entered the index. */
    assert(tinykv_get(db, oversized_key, sizeof(oversized_key), &value,
                      &value_length) == TINYKV_ERR_NOT_FOUND);
    /* Compaction must survive a max-size record verbatim. */
    assert(tinykv_compact(db) == TINYKV_OK);
    assert(tinykv_get_into(db, max_key, sizeof(max_key), max_value,
                           sizeof(max_value), &value_length) == TINYKV_OK);
    assert(value_length == sizeof(max_value));
    tinykv_close(db);
    /* And replays back after a restart. */
    assert(tinykv_open(path, &options, &db) == TINYKV_OK);
    assert(tinykv_get(db, max_key, sizeof(max_key), &value,
                      &value_length) == TINYKV_OK);
    assert(value_length == sizeof(max_value));
    free(value);
    tinykv_close(db);
}

static void test_randomized_operations_match_reference(void)
{
    /* Deterministic model check: a fixed table of slots mirrors what the
     * database should contain; every put/delete/get is verified against
     * it, with a restart in the middle to exercise replay. */
    char path[128];
    tinykv_db *db = NULL;
    enum { SLOTS = 40, OPS = 4000, MAX_VALUE = 64 };
    char keys[SLOTS][24];
    char values[SLOTS][MAX_VALUE];
    size_t key_lengths[SLOTS], value_lengths[SLOTS];
    int present[SLOTS];
    uint32_t seed = 0xC0FFEE11;
    void *got = NULL;
    size_t got_length = 0;
    int op, i;
    db_path(path, sizeof(path), "model.db");
    for (i = 0; i < SLOTS; ++i) {
        snprintf(keys[i], sizeof(keys[i]), "slot-%02d", i);
        key_lengths[i] = strlen(keys[i]);
        present[i] = 0;
    }
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    for (op = 0; op < OPS; ++op) {
        seed = seed * 1664525u + 1013904223u;
        i = (int)((seed >> 8) % SLOTS);
        if (seed & 1) { /* put: new random value */
            size_t vlen = (seed >> 16) % (MAX_VALUE + 1);
            size_t j;
            for (j = 0; j < vlen; ++j)
                values[i][j] = (char)(seed >> (j % 17));
            assert(tinykv_put(db, keys[i], key_lengths[i], values[i],
                              vlen) == TINYKV_OK);
            value_lengths[i] = vlen;
            present[i] = 1;
        } else if (present[i]) {
            assert(tinykv_delete(db, keys[i], key_lengths[i]) == TINYKV_OK);
            present[i] = 0;
        } else {
            assert(tinykv_delete(db, keys[i], key_lengths[i]) ==
                   TINYKV_ERR_NOT_FOUND);
        }
        if (op % 7 == 0) { /* spot-check one random slot */
            int probe = (int)((seed >> 20) % SLOTS);
            if (present[probe]) {
                assert(tinykv_get(db, keys[probe], key_lengths[probe], &got,
                                  &got_length) == TINYKV_OK);
                assert(got_length == value_lengths[probe]);
                assert(got_length == 0 ||
                       memcmp(got, values[probe], got_length) == 0);
                free(got);
                got = NULL;
            } else {
                assert(tinykv_get(db, keys[probe], key_lengths[probe], &got,
                                  &got_length) == TINYKV_ERR_NOT_FOUND);
            }
        }
        if (op == OPS / 2) { /* restart: replay must reproduce the model */
            tinykv_close(db);
            assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
            for (i = 0; i < SLOTS; ++i) {
                if (present[i]) {
                    assert(tinykv_get(db, keys[i], key_lengths[i], &got,
                                      &got_length) == TINYKV_OK);
                    assert(got_length == value_lengths[i]);
                    assert(got_length == 0 ||
                           memcmp(got, values[i], got_length) == 0);
                    free(got);
                    got = NULL;
                } else {
                    assert(tinykv_get(db, keys[i], key_lengths[i], &got,
                                      &got_length) == TINYKV_ERR_NOT_FOUND);
                }
            }
        }
    }
    /* Full comparison at the end, then compaction must preserve it. */
    assert(tinykv_compact(db) == TINYKV_OK);
    for (i = 0; i < SLOTS; ++i) {
        if (present[i]) {
            assert(tinykv_get(db, keys[i], key_lengths[i], &got,
                              &got_length) == TINYKV_OK);
            assert(got_length == value_lengths[i]);
            assert(got_length == 0 || memcmp(got, values[i], got_length) == 0);
            free(got);
            got = NULL;
        } else {
            assert(tinykv_get(db, keys[i], key_lengths[i], &got,
                              &got_length) == TINYKV_ERR_NOT_FOUND);
        }
    }
    tinykv_close(db);
}

static void test_compaction_after_heavy_overwrites(void)
{
    char path[128], key[24], value[48];
    tinykv_db *db = NULL;
    tinykv_stats stats;
    void *got = NULL;
    size_t got_length = 0;
    int round, i;
    db_path(path, sizeof(path), "compact-heavy.db");
    assert(tinykv_open(path, NULL, &db) == TINYKV_OK);
    /* Three full overwrite rounds plus a delete of every fifth key. */
    for (round = 0; round < 3; ++round) {
        for (i = 0; i < 1000; ++i) {
            snprintf(key, sizeof(key), "key-%04d", i);
            snprintf(value, sizeof(value), "round-%d-value-%04d", round, i);
            assert(tinykv_put(db, key, strlen(key), value,
                              strlen(value)) == TINYKV_OK);
        }
    }
    for (i = 0; i < 1000; i += 5) {
        snprintf(key, sizeof(key), "key-%04d", i);
        assert(tinykv_delete(db, key, strlen(key)) == TINYKV_OK);
    }
    assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
    assert(stats.records == 800);
    {
        uint64_t before = stats.file_size;
        assert(before > stats.live_bytes * 2); /* mostly garbage */
        assert(tinykv_compact(db) == TINYKV_OK);
        assert(tinykv_get_stats(db, &stats) == TINYKV_OK);
        assert(stats.dead_bytes == 0);
        assert(stats.file_size == stats.live_bytes);
        assert(stats.file_size < before / 2);
        assert(stats.compactions == 1);
    }
    for (i = 0; i < 1000; ++i) {
        snprintf(key, sizeof(key), "key-%04d", i);
        snprintf(value, sizeof(value), "round-2-value-%04d", i);
        if (i % 5 == 0) {
            assert(tinykv_get(db, key, strlen(key), &got, &got_length) ==
                   TINYKV_ERR_NOT_FOUND);
            continue;
        }
        assert(tinykv_get(db, key, strlen(key), &got, &got_length) ==
               TINYKV_OK);
        assert(got_length == strlen(value));
        assert(memcmp(got, value, got_length) == 0);
        free(got);
        got = NULL;
    }
    tinykv_close(db);
}

static void test_reader_sees_writer_state_without_lock(void)
{
    char path[128];
    tinykv_db *writer = NULL, *reader = NULL;
    tinykv_options options;
    void *value = NULL;
    size_t value_length = 0;
    db_path(path, sizeof(path), "reader.db");
    assert(tinykv_open(path, NULL, &writer) == TINYKV_OK);
    assert(tinykv_put(writer, "committed", 9, "yes", 3) == TINYKV_OK);

    /* A reader may open while the writer holds the lock; it replays the
     * log as it exists at open time. */
    options = tinykv_options_default_for_test();
    options.read_only = 1;
    assert(tinykv_open(path, &options, &reader) == TINYKV_OK);
    assert(tinykv_get(reader, "committed", 9, &value, &value_length) ==
           TINYKV_OK);
    assert(value_length == 3);
    free(value);
    /* Writes after the reader opened are invisible to its snapshot. */
    assert(tinykv_put(writer, "later", 5, "value", 5) == TINYKV_OK);
    assert(tinykv_get(reader, "later", 5, &value, &value_length) ==
           TINYKV_ERR_NOT_FOUND);
    /* Delete handling stays consistent for the reader. */
    assert(tinykv_delete(reader, "committed", 9) == TINYKV_ERR_INVALID);
    assert(tinykv_get(writer, "committed", 9, &value, &value_length) ==
           TINYKV_OK);
    free(value);
    tinykv_close(reader);
    tinykv_close(writer);
}

int main(void)
{
    assert(mkdtemp(temp_dir) != NULL);
    test_open_creates_private_file();
    test_open_rejects_bad_arguments();
    test_second_writer_is_locked_out();
    test_corrupt_log_fails_open();
    test_torn_tail_opens_cleanly();
    test_version_and_status_strings();
    test_replay_builds_index();
    test_replay_over_limit_is_corruption();
    test_put_get_delete_roundtrip();
    test_zero_length_keys();
    test_write_argument_validation();
    test_write_reclaims_torn_tail();
    test_restart_persistence();
    test_stats_track_activity();
    test_compaction_reclaims_dead_bytes();
    test_compaction_with_torn_tail();
    test_compaction_empty_database();
    test_verify_classifies_logs();
    test_repair_is_non_destructive();
    test_repair_stops_at_midfile_corruption();
    test_repair_clean_log_and_lock();
    test_get_into_copies_without_allocation();
    test_iteration_is_sorted_and_skips_deletes();
    test_iteration_empty_and_fresh_values();
    test_accessors_report_path_and_size();
    test_max_length_boundaries();
    test_randomized_operations_match_reference();
    test_compaction_after_heavy_overwrites();
    test_reader_sees_writer_state_without_lock();
    remove_temp_dir();
    puts("tinykv db: all tests passed");
    return 0;
}
