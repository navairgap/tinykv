#include "tinykv/db.h"
#include "tkv_io.h"
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

int main(void)
{
    assert(mkdtemp(temp_dir) != NULL);
    test_open_creates_private_file();
    test_open_rejects_bad_arguments();
    test_second_writer_is_locked_out();
    test_corrupt_log_fails_open();
    test_torn_tail_opens_cleanly();
    test_version_and_status_strings();
    remove_temp_dir();
    puts("tinykv db: all tests passed");
    return 0;
}
