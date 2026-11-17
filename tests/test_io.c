#include "tkv_io.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[64] = "/tmp/tinykv-io-XXXXXX";

static void make_temp_dir(void)
{
    assert(mkdtemp(temp_dir) != NULL);
}

static void remove_temp_dir(void)
{
    /* Tests only ever touch the mkdtemp directory they created. */
    DIR *dir = opendir(temp_dir);
    struct dirent *item;
    char entry[384]; /* NAME_MAX + dir path + separator stays bounded */
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

static void test_write_pread_size_roundtrip(void)
{
    char path[128];
    const uint8_t payload[] = {0, 1, 2, 3, 4, 250, 251, 252, 253, 254, 255};
    uint8_t out[3];
    int fd, created = -1;
    snprintf(path, sizeof(path), "%s/kv.log", temp_dir);
    fd = tkv_open_log(path, 0, &created);
    assert(fd >= 0 && created == 1);
    assert(tkv_write_all(fd, payload, sizeof(payload)) == 0);
    assert(tkv_file_size(fd) == (off_t)sizeof(payload));
    assert(tkv_pread_all(fd, out, sizeof(out), 8) == 0);
    assert(out[0] == 253 && out[1] == 254 && out[2] == 255);
    /* Short read past EOF must fail. */
    assert(tkv_pread_all(fd, out, 2, sizeof(payload)) == -1);
    /* Reopening an existing file must not report creation. */
    {
        int second = tkv_open_log(path, 0, &created);
        assert(second >= 0 && created == 0);
        close(second);
    }
    {
        int readonly = tkv_open_log(path, 1, NULL);
        assert(readonly >= 0);
        close(readonly);
    }
    assert(tkv_fsync_file(fd) == 0);
    close(fd);
    /* New files are private to the owner. */
    {
        struct stat st;
        assert(stat(path, &st) == 0);
        assert((st.st_mode & 0777) == 0600);
    }
}

static void test_lock_is_exclusive(void)
{
    char path[128];
    int first, second;
    snprintf(path, sizeof(path), "%s/lockfile", temp_dir);
    first = tkv_open_log(path, 0, NULL);
    assert(first >= 0);
    second = open(path, O_RDWR | O_CLOEXEC);
    assert(second >= 0);
    assert(tkv_lock_try(first, 1) == 0);
    assert(tkv_lock_try(second, 1) == -1);
    tkv_unlock(first);
    assert(tkv_lock_try(second, 1) == 0);
    tkv_unlock(second);
    close(first);
    close(second);
}

static void test_rename_sync_and_copy(void)
{
    char from[128], to[128], backup[128];
    int fd;
    snprintf(from, sizeof(from), "%s/orig.log", temp_dir);
    snprintf(to, sizeof(to), "%s/moved.log", temp_dir);
    snprintf(backup, sizeof(backup), "%s/orig.log.bak", temp_dir);
    fd = tkv_open_log(from, 0, NULL);
    assert(fd >= 0);
    assert(tkv_write_all(fd, "hello", 5) == 0);
    close(fd);
    assert(tkv_rename_sync(from, to) == 0);
    assert(access(from, F_OK) == -1 && errno == ENOENT);
    assert(access(to, F_OK) == 0);
    assert(tkv_copy_file(to, backup) == 0);
    {
        int check = tkv_open_log(backup, 1, NULL);
        uint8_t out[6];
        assert(check >= 0);
        assert(tkv_file_size(check) == 5);
        assert(tkv_pread_all(check, out, 5, 0) == 0);
        assert(memcmp(out, "hello", 5) == 0);
        close(check);
        /* Truncation needs a writable descriptor. */
        check = tkv_open_log(backup, 0, NULL);
        assert(check >= 0);
        assert(tkv_truncate_file(check, 3) == 0);
        assert(tkv_file_size(check) == 3);
        close(check);
    }
}

static void test_fsync_path_and_sidecar(void)
{
    char sidecar[160], missing[128];
    assert(tkv_fsync_path(temp_dir) == 0);
    snprintf(missing, sizeof(missing), "%s/nope/file", temp_dir);
    assert(tkv_fsync_path(missing) == -1);
    assert(tkv_sidecar_path("/data/kv.log", ".lock", sidecar,
                            sizeof(sidecar)) == 0);
    assert(strcmp(sidecar, "/data/kv.log.lock") == 0);
    assert(tkv_sidecar_path("/data/kv.log", ".very-long-suffix", sidecar, 4) ==
           -1);
}

int main(void)
{
    make_temp_dir();
    test_write_pread_size_roundtrip();
    test_lock_is_exclusive();
    test_rename_sync_and_copy();
    test_fsync_path_and_sidecar();
    remove_temp_dir();
    puts("tinykv io: all tests passed");
    return 0;
}
