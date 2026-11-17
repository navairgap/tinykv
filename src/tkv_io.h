/* Internal POSIX file helpers. Not part of the public API. */
#ifndef TINYKV_TKV_IO_H
#define TINYKV_TKV_IO_H
#if defined(_WIN32)
#error "tinykv v1 requires a POSIX platform for file I/O"
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* All functions return 0 on success and -1 on failure unless noted. */

int tkv_write_all(int fd, const void *bytes, size_t length);
/* 0 = full read, -1 = I/O error or fewer than length bytes available. */
int tkv_pread_all(int fd, void *bytes, size_t length, uint64_t offset);
int tkv_fsync_file(int fd);
/* fsync the directory that contains path, so creates/renames persist. */
int tkv_fsync_path(const char *path);
off_t tkv_file_size(int fd);
/* Open the log read-only or read-write, creating it with mode 0600.
 * *created (optional) reports whether this call created the file. */
int tkv_open_log(const char *path, int read_only, int *created);
int tkv_lock_try(int fd, int exclusive);
void tkv_unlock(int fd);
/* rename(2) followed by an fsync of the destination directory. */
int tkv_rename_sync(const char *from, const char *to);
int tkv_copy_file(const char *from, const char *to);
int tkv_truncate_file(int fd, uint64_t length);
/* dst must hold strlen(path) + strlen(suffix) + 1 bytes. */
int tkv_sidecar_path(const char *path, const char *suffix, char *dst,
                     size_t dst_size);
#endif
