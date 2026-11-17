#include "tkv_log.h"
#include "tkv_io.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TKV_LOG_CHUNK 65536u

int tkv_log_append(int fd, uint8_t type, const void *key, size_t key_length,
                   const void *value, size_t value_length,
                   const tkv_log_limits *limits, uint64_t *offset_out,
                   size_t *length_out)
{
    uint8_t *encoded = NULL;
    size_t encoded_length = 0;
    uint64_t offset;
    tinykv_record_status status;

    if (limits == NULL || offset_out == NULL || length_out == NULL)
        return -1;
    status = tinykv_record_encode(type, (const uint8_t *)key, key_length,
                                  (const uint8_t *)value, value_length,
                                  limits->max_key_length,
                                  limits->max_value_length, &encoded,
                                  &encoded_length);
    if (status != TINYKV_RECORD_OK) return -1;

    offset = (uint64_t)lseek(fd, 0, SEEK_END);
    if (offset == (uint64_t)-1 || tkv_write_all(fd, encoded, encoded_length) != 0) {
        free(encoded);
        return -1;
    }
    free(encoded);
    *offset_out = offset;
    *length_out = encoded_length;
    return 0;
}

/* Sequential scan with a buffer that grows to fit any permitted record. */
int tkv_log_scan(int fd, const tkv_log_limits *limits,
                 tkv_log_record_fn on_record, void *user,
                 tkv_log_scan_result *result)
{
    uint8_t *buffer = NULL;
    size_t capacity = 0, fill = 0, pos = 0;
    uint64_t buffer_start = 0; /* file offset of buffer[0] */
    int at_eof = 0, io_failed = 0;

    if (limits == NULL || result == NULL) return -1;
    memset(result, 0, sizeof(*result));
    result->total_bytes = (uint64_t)tkv_file_size(fd);
    if (result->total_bytes == (uint64_t)-1) {
        result->stop_status = TINYKV_RECORD_INVALID_ARGUMENT;
        return -1;
    }
    result->last_good_offset = 0;
    result->stop_offset = 0;

    capacity = TKV_LOG_CHUNK;
    buffer = (uint8_t *)malloc(capacity);
    if (buffer == NULL) return -1;

    for (;;) {
        tinykv_record_view view;
        size_t consumed = 0;
        tinykv_record_status status;

        for (;;) {
            status = tinykv_record_decode(buffer + pos, fill - pos,
                                          limits->max_key_length,
                                          limits->max_value_length, &view,
                                          &consumed);
            if (status != TINYKV_RECORD_TRUNCATED || at_eof) break;
            /* Need more bytes: compact and refill once, growing if the
             * decoded header demands a record larger than the buffer. */
            if (pos != 0) {
                memmove(buffer, buffer + pos, fill - pos);
                buffer_start += pos;
                fill -= pos;
                pos = 0;
            }
            {
                size_t need = 0;
                if (fill >= 5 && fill - pos >= TINYKV_RECORD_HEADER_SIZE) {
                    /* The header is fully present, so any truncation is in
                     * the payload: size the buffer for the whole record. */
                    uint32_t klen = ((uint32_t)buffer[5] << 24) |
                                    ((uint32_t)buffer[6] << 16) |
                                    ((uint32_t)buffer[7] << 8) |
                                    (uint32_t)buffer[8];
                    uint32_t vlen = ((uint32_t)buffer[9] << 24) |
                                    ((uint32_t)buffer[10] << 16) |
                                    ((uint32_t)buffer[11] << 8) |
                                    (uint32_t)buffer[12];
                    need = TINYKV_RECORD_HEADER_SIZE + (size_t)klen + (size_t)vlen;
                } else {
                    need = TINYKV_RECORD_HEADER_SIZE;
                }
                if (need > capacity) {
                    uint8_t *larger = (uint8_t *)realloc(buffer, need);
                    if (larger == NULL) {
                        io_failed = 1;
                        break;
                    }
                    buffer = larger;
                    capacity = need;
                }
            }
            {
                ssize_t got;
                /* pread: the descriptor's position belongs to the appender */
                do
                    got = pread(fd, buffer + fill, capacity - fill,
                                (off_t)(buffer_start + fill));
                while (got < 0 && errno == EINTR);
                if (got < 0) {
                    io_failed = 1;
                    break;
                }
                if (got == 0)
                    at_eof = 1;
                else
                    fill += (size_t)got;
            }
        }
        if (io_failed) break;

        if (status == TINYKV_RECORD_OK) {
            if (on_record != NULL &&
                on_record(&view, buffer_start + pos, user) != 0) {
                result->callback_aborted = 1;
                result->stop_offset = buffer_start + pos;
                result->last_good_offset = buffer_start + pos;
                break;
            }
            pos += consumed;
            result->last_good_offset = buffer_start + pos;
            continue;
        }
        if (status == TINYKV_RECORD_TRUNCATED) {
            if (fill == pos) {
                /* Exactly at EOF with no leftover bytes: clean end. */
                result->stop_status = TINYKV_RECORD_OK;
                break;
            }
            result->tail_truncated = 1;
            result->stop_status = TINYKV_RECORD_TRUNCATED;
            result->stop_offset = buffer_start + pos;
            result->discarded_bytes =
                result->total_bytes - result->last_good_offset;
            break;
        }
        /* Complete bytes that cannot decode: corruption, not a tail. */
        result->stop_status = status;
        result->stop_offset = buffer_start + pos;
        break;
    }

    free(buffer);
    return io_failed ? -1 : 0;
}
