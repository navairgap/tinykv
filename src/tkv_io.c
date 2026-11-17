#include "tkv_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

int tkv_write_all(int fd, const void *bytes, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)bytes;
    while (length != 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

int tkv_pread_all(int fd, void *bytes, size_t length, uint64_t offset)
{
    uint8_t *cursor = (uint8_t *)bytes;
    while (length != 0) {
        ssize_t got = pread(fd, cursor, length, (off_t)offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) return -1; /* unexpected EOF: short record */
        cursor += (size_t)got;
        offset += (uint64_t)got;
        length -= (size_t)got;
    }
    return 0;
}

int tkv_fsync_file(int fd)
{
    while (fsync(fd) != 0)
        if (errno != EINTR) return -1;
    return 0;
}

int tkv_fsync_path(const char *path)
{
    char dir[4096];
    const char *slash;
    size_t prefix;
    int fd, result;

    if (path == NULL || path[0] == '\0') return -1;
    slash = strrchr(path, '/');
    if (slash == NULL) {
        dir[0] = '.';
        dir[1] = '\0';
    } else {
        prefix = (size_t)(slash - path);
        if (prefix == 0) prefix = 1; /* keep the leading '/' */
        if (prefix >= sizeof(dir)) return -1;
        memcpy(dir, path, prefix);
        dir[prefix] = '\0';
    }
    do
        fd = open(dir, O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    result = tkv_fsync_file(fd);
    close(fd); /* best effort; the fsync result is what we report */
    return result;
}

off_t tkv_file_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return (off_t)-1;
    return st.st_size;
}

int tkv_open_log(const char *path, int read_only, int *created)
{
    int fd;
    if (read_only) {
        do
            fd = open(path, O_RDONLY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (created != NULL && fd >= 0) *created = 0;
        return fd;
    }
    do
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        if (created != NULL) *created = 1;
        return fd;
    }
    if (errno != EEXIST) return -1;
    do
        fd = open(path, O_RDWR | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (created != NULL && fd >= 0) *created = 0;
    return fd;
}

int tkv_lock_try(int fd, int exclusive)
{
    int operation = exclusive ? (LOCK_EX | LOCK_NB) : (LOCK_SH | LOCK_NB);
    return flock(fd, operation) == 0 ? 0 : -1;
}

void tkv_unlock(int fd)
{
    (void)flock(fd, LOCK_UN);
}

int tkv_rename_sync(const char *from, const char *to)
{
    if (rename(from, to) != 0) return -1;
    return tkv_fsync_path(to);
}

int tkv_copy_file(const char *from, const char *to)
{
    uint8_t buffer[65536];
    int src, dst = -1, result = -1;
    do
        src = open(from, O_RDONLY | O_CLOEXEC);
    while (src < 0 && errno == EINTR);
    if (src < 0) return -1;
    do
        dst = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    while (dst < 0 && errno == EINTR);
    if (dst < 0) goto out;
    for (;;) {
        ssize_t got;
        do
            got = read(src, buffer, sizeof(buffer));
        while (got < 0 && errno == EINTR);
        if (got < 0) goto out;
        if (got == 0) break;
        if (tkv_write_all(dst, buffer, (size_t)got) != 0) goto out;
    }
    /* The copy is a durability artifact (backups, repair presets), so
     * sync it before reporting success. */
    if (tkv_fsync_file(dst) != 0) goto out;
    result = 0;
out:
    if (dst >= 0) close(dst);
    close(src);
    return result;
}

int tkv_truncate_file(int fd, uint64_t length)
{
    return ftruncate(fd, (off_t)length) == 0 ? 0 : -1;
}

int tkv_sidecar_path(const char *path, const char *suffix, char *dst,
                     size_t dst_size)
{
    return snprintf(dst, dst_size, "%s%s", path, suffix) < (int)dst_size
               ? 0
               : -1;
}
