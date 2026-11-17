/* Internal append-log writer and recovery scanner. Not public API. */
#ifndef TINYKV_TKV_LOG_H
#define TINYKV_TKV_LOG_H

#include "tinykv/record.h"
#include <stddef.h>
#include <stdint.h>

typedef struct tkv_log_limits {
    uint32_t max_key_length;
    uint32_t max_value_length;
} tkv_log_limits;

/* Encode and append one record at the end of fd (O_RDWR descriptor).
 * Returns 0 on success, a positive tinykv_record_status when encoding
 * rejects the record, or -1 on I/O failure. *offset_out is the record's
 * start offset and *length_out its encoded size. */
int tkv_log_append(int fd, uint8_t type, const void *key, size_t key_length,
                   const void *value, size_t value_length,
                   const tkv_log_limits *limits, uint64_t *offset_out,
                   size_t *length_out);

typedef struct tkv_log_scan_result {
    uint64_t total_bytes;      /* file size at scan start */
    uint64_t last_good_offset; /* end of the last fully validated record */
    uint64_t stop_offset;      /* where parsing stopped */
    uint64_t discarded_bytes;  /* stop_offset - last_good_offset for tails */
    int tail_truncated;        /* 1 when stopped on an incomplete tail */
    int callback_aborted;      /* 1 when the record callback stopped the scan */
    tinykv_record_status stop_status; /* OK for a clean end */
} tkv_log_scan_result;

/* Callback: return 0 to continue, 1 to abort the scan cleanly. */
typedef int (*tkv_log_record_fn)(const tinykv_record_view *view,
                                 uint64_t offset, void *user);
/* 0 = classification finished (clean end, truncated tail, or located
 * corruption); -1 = I/O failure. Corruption is reported through
 * stop_status/stop_offset, never silently skipped. */
int tkv_log_scan(int fd, const tkv_log_limits *limits,
                 tkv_log_record_fn on_record, void *user,
                 tkv_log_scan_result *result);
#endif
