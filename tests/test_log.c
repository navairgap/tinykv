#include "tkv_log.h"
#include "tkv_io.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char temp_dir[64] = "/tmp/tinykv-log-XXXXXX";

struct captured {
    uint8_t type[16];
    char key[16][64];
    size_t klen[16];
    char value[16][64];
    size_t vlen[16];
    uint64_t offset[16];
    size_t rlen[16];
    int count;
};

static int capture(const tinykv_record_view *view, uint64_t offset, void *user)
{
    struct captured *c = (struct captured *)user;
    size_t kcopy = view->key_length < sizeof(c->key[0])
                       ? view->key_length
                       : sizeof(c->key[0]) - 1;
    size_t vcopy = view->value_length < sizeof(c->value[0])
                       ? view->value_length
                       : sizeof(c->value[0]) - 1;
    assert(c->count < 16);
    c->type[c->count] = view->type;
    c->klen[c->count] = view->key_length;
    c->vlen[c->count] = view->value_length;
    memcpy(c->key[c->count], view->key, kcopy);
    memcpy(c->value[c->count], view->value, vcopy);
    c->offset[c->count] = offset;
    c->count++;
    return 0;
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

static void test_append_and_scan_roundtrip(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    uint64_t offset;
    size_t length;
    int fd;
    memset(&got, 0, sizeof(got));
    snprintf(path, sizeof(path), "%s/kv.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);

    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(offset == 0 && length == TINYKV_RECORD_HEADER_SIZE + 5 + 3);
    assert(tkv_log_append(fd, TINYKV_RECORD_DELETE, "gone", 4, NULL, 0,
                          &limits, &offset, &length) == 0);
    assert(offset == 25 && length == TINYKV_RECORD_HEADER_SIZE + 4);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "", 0,
                          &limits, &offset, &length) == 0);
    assert(offset == 25 + TINYKV_RECORD_HEADER_SIZE + 4);

    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 3 && result.stop_status == TINYKV_RECORD_OK);
    assert(!result.tail_truncated && !result.callback_aborted);
    assert(result.last_good_offset == result.total_bytes);
    assert(result.last_good_offset == (uint64_t)tkv_file_size(fd));
    assert(got.offset[0] == 0 && got.type[0] == TINYKV_RECORD_PUT &&
           got.klen[0] == 5 && got.vlen[0] == 3);
    assert(memcmp(got.key[0], "alpha", 5) == 0 &&
           memcmp(got.value[0], "one", 3) == 0);
    assert(got.offset[1] == 25 && got.type[1] == TINYKV_RECORD_DELETE &&
           got.vlen[1] == 0);
    assert(got.offset[2] == 25 + 21 && got.type[2] == TINYKV_RECORD_PUT &&
           got.vlen[2] == 0 && got.klen[2] == 4);
    close(fd);
}

static void test_scan_detects_truncated_tail(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    uint64_t offset, full_size;
    size_t length;
    int fd;
    memset(&got, 0, sizeof(got));
    snprintf(path, sizeof(path), "%s/tail.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "two", 3,
                          &limits, &offset, &length) == 0);
    full_size = offset + length;
    /* A crash mid-append leaves 3 bytes of the second record unwritten. */
    assert(tkv_truncate_file(fd, full_size - 3) == 0);

    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 1);
    assert(result.tail_truncated == 1);
    assert(result.stop_status == TINYKV_RECORD_TRUNCATED);
    assert(result.last_good_offset == TINYKV_RECORD_HEADER_SIZE + 5 + 3);
    assert(result.stop_offset == result.last_good_offset);
    /* The partial record still has its on-disk bytes counted as discarded. */
    assert(result.total_bytes == full_size - 3);
    assert(result.discarded_bytes == (full_size - 3) - result.last_good_offset);
    close(fd);
}

static void test_scan_reports_midfile_corruption(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    uint64_t offset;
    size_t length;
    int fd;
    memset(&got, 0, sizeof(got));
    snprintf(path, sizeof(path), "%s/corrupt.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "two", 3,
                          &limits, &offset, &length) == 0);
    /* Flip one payload byte of the first record. */
    {
        uint8_t scratch = 0xFF;
        assert(pwrite(fd, &scratch, 1, (off_t)(TINYKV_RECORD_HEADER_SIZE)) ==
               1);
    }
    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 0);
    assert(result.tail_truncated == 0);
    assert(result.stop_status == TINYKV_RECORD_BAD_CHECKSUM);
    assert(result.stop_offset == 0);
    close(fd);
}

static void test_scan_reports_invalid_magic(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    uint64_t offset;
    size_t length;
    int fd;
    memset(&got, 0, sizeof(got));
    snprintf(path, sizeof(path), "%s/magic.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    {
        const uint8_t junk[4] = {'J', 'U', 'N', 'K'};
        assert(pwrite(fd, junk, 4, 0) == 4);
    }
    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 0 && result.tail_truncated == 0);
    assert(result.stop_status == TINYKV_RECORD_INVALID_FORMAT);
    assert(result.stop_offset == 0);
    close(fd);
}

static void test_scan_empty_file_is_clean(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    int fd;
    memset(&got, 0, sizeof(got));
    snprintf(path, sizeof(path), "%s/empty.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 0 && result.stop_status == TINYKV_RECORD_OK);
    assert(!result.tail_truncated && result.last_good_offset == 0);
    close(fd);
}

static void test_scan_handles_records_larger_than_chunk(void)
{
    char path[128];
    tkv_log_limits limits = {64, 200000};
    struct captured got;
    tkv_log_scan_result result;
    static char big[100000];
    uint64_t offset;
    size_t length;
    int fd;
    memset(&got, 0, sizeof(got));
    memset(big, 'A', sizeof(big));
    snprintf(path, sizeof(path), "%s/big.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "big", 3, big, sizeof(big),
                          &limits, &offset, &length) == 0);
    assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
    assert(got.count == 1 && got.vlen[0] == sizeof(big));
    assert(result.stop_status == TINYKV_RECORD_OK &&
           result.last_good_offset == result.total_bytes);
    close(fd);
}

static void test_append_rejects_bad_input(void)
{
    tkv_log_limits limits = {64, 1024};
    uint64_t offset;
    size_t length;
    /* I/O failure: encoding succeeds, lseek on a closed fd does not. */
    assert(tkv_log_append(-1, TINYKV_RECORD_PUT, "k", 1, "v", 1, &limits,
                          &offset, &length) == -1);
    /* Encoding rejections surface the codec status. */
    assert(tkv_log_append(0, TINYKV_RECORD_DELETE, "k", 1, "v", 1, &limits,
                          &offset, &length) ==
           (int)TINYKV_RECORD_INVALID_FORMAT);
    assert(tkv_log_append(0, TINYKV_RECORD_PUT, "k", 1, "v", 1, NULL,
                          &offset, &length) == -1);
    {
        tkv_log_limits tiny = {2, 2};
        assert(tkv_log_append(0, TINYKV_RECORD_PUT, "toolong", 7, "v", 1,
                              &tiny, &offset, &length) ==
               (int)TINYKV_RECORD_LIMIT_EXCEEDED);
    }
}

static void test_scan_survives_every_truncation(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    struct captured got;
    tkv_log_scan_result result;
    uint64_t offset, full_size, cut;
    size_t length;
    int fd, iterations = 0;
    snprintf(path, sizeof(path), "%s/matrix.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "two", 3,
                          &limits, &offset, &length) == 0);
    full_size = offset + length;

    /* Cutting at any byte position can only produce a clean scan (cuts
     * at record boundaries) or a truncated tail; never corruption. The
     * log is rebuilt for every cut because a prior truncation destroys
     * bytes that ftruncate cannot restore when growing back. */
    for (cut = 0; cut <= full_size; ++cut) {
        assert(tkv_truncate_file(fd, 0) == 0);
        assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                              &limits, &offset, &length) == 0);
        assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "two", 3,
                              &limits, &offset, &length) == 0);
        assert(tkv_truncate_file(fd, cut) == 0);
        memset(&got, 0, sizeof(got));
        assert(tkv_log_scan(fd, &limits, capture, &got, &result) == 0);
        iterations++;
        assert(result.stop_status == TINYKV_RECORD_OK ||
               result.stop_status == TINYKV_RECORD_TRUNCATED);
        assert(result.last_good_offset <= cut);
        if (cut == 0 || cut == TINYKV_RECORD_HEADER_SIZE + 8 ||
            cut == full_size)
            assert(result.tail_truncated == 0); /* exact boundaries */
        else if (cut < TINYKV_RECORD_HEADER_SIZE + 8)
            assert(got.count == 0 && result.tail_truncated == 1);
        else if (cut > TINYKV_RECORD_HEADER_SIZE + 8)
            assert(got.count == 1);
        if (result.tail_truncated)
            assert(result.discarded_bytes == cut - result.last_good_offset);
    }
    assert(iterations == (int)full_size + 1);
    close(fd);
}

static int abort_after_first(const tinykv_record_view *view, uint64_t offset,
                             void *user)
{
    int *seen = (int *)user;
    (void)view;
    (void)offset;
    (*seen)++;
    return 1; /* stop the scan cleanly after the first record */
}

static void test_scan_callback_can_abort(void)
{
    char path[128];
    tkv_log_limits limits = {64, 1024};
    tkv_log_scan_result result;
    int seen = 0;
    uint64_t offset;
    size_t length;
    int fd;
    snprintf(path, sizeof(path), "%s/abort.log", temp_dir);
    fd = tkv_open_log(path, 0, NULL);
    assert(fd >= 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "alpha", 5, "one", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_append(fd, TINYKV_RECORD_PUT, "beta", 4, "two", 3,
                          &limits, &offset, &length) == 0);
    assert(tkv_log_scan(fd, &limits, abort_after_first, &seen, &result) == 0);
    assert(seen == 1);
    assert(result.callback_aborted == 1);
    assert(result.stop_offset == 0); /* aborted at the first record */
    assert(result.last_good_offset == 0);
    assert(result.tail_truncated == 0);
    close(fd);
}

int main(void)
{
    assert(mkdtemp(temp_dir) != NULL);
    test_append_and_scan_roundtrip();
    test_scan_detects_truncated_tail();
    test_scan_reports_midfile_corruption();
    test_scan_reports_invalid_magic();
    test_scan_empty_file_is_clean();
    test_scan_handles_records_larger_than_chunk();
    test_append_rejects_bad_input();
    test_scan_survives_every_truncation();
    test_scan_callback_can_abort();
    remove_temp_dir();
    puts("tinykv log: all tests passed");
    return 0;
}
