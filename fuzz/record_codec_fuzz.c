/* Fuzz harnesses for the record codec decoder and the recovery scanner.
 *
 * Build with libFuzzer (clang):
 *   clang -fsanitize=fuzzer,address -DTINYKV_HAVE_LIBFUZZER -Iinclude \
 *       src/record_codec.c fuzz/record_codec_fuzz.c -o record-fuzz
 *
 * Standalone build (any compiler, used by make fuzz and CI): feeds
 * generated and mutated records to the decoder, including every single-
 * byte corruption of a valid record and every prefix of it, so torn
 * writes and bit flips are covered deterministically everywhere.
 */
#include "tinykv/record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tkv_log.h"
#include "tkv_io.h"
#include <unistd.h>

int LLVMFuzzerTestScan(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        tinykv_record_view view;
        size_t consumed = 0;
        tinykv_record_status status = tinykv_record_decode(
            data + pos, size - pos, 4096, 1048576, &view, &consumed);
        if (status != TINYKV_RECORD_OK) break;
        /* The view must always stay inside the caller's buffer. */
        if (view.key + view.key_length > data + pos + consumed ||
            view.value + view.value_length > data + pos + consumed)
            abort();
        pos += consumed;
    }
    LLVMFuzzerTestScan(data, size);
    return 0;
}

/* Scanner invariant check: replay the input as if it were a log file.
 * Callback views must stay in bounds, last_good_offset must never
 * exceed the file, and a tail classification must account for every
 * discarded byte. */
static int fuzz_scanned;
static int scan_capture(const tinykv_record_view *view, uint64_t offset,
                        void *user)
{
    const uint8_t *data = (const uint8_t *)user;
    (void)offset; /* in-bounds checks read the end from fuzz_scanned */
    if (view->key + view->key_length > data + fuzz_scanned ||
        view->value + view->value_length > data + fuzz_scanned)
        return 1;
    return 0;
}

/* libFuzzer builds call both checks from the single entry point. */
int LLVMFuzzerTestScan(const uint8_t *data, size_t size)
{
    char path[] = "/tmp/tinykv-fuzz-scan-XXXXXX";
    tkv_log_limits limits = {4096, 1048576};
    tkv_log_scan_result result;
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    fuzz_scanned = (int)size;
    if (size != 0 && tkv_write_all(fd, data, size) != 0) {
        close(fd);
        unlink(path);
        return 0;
    }
    if (tkv_log_scan(fd, &limits, scan_capture, (void *)data, &result) != 0) {
        close(fd);
        unlink(path);
        return 0;
    }
    if (result.last_good_offset > size) abort();
    if (result.tail_truncated &&
        result.last_good_offset + result.discarded_bytes != size) abort();
    if (!result.tail_truncated &&
        result.stop_status != TINYKV_RECORD_OK &&
        result.stop_offset > size) abort();
    close(fd);
    unlink(path);
    return 0;
}

#ifndef TINYKV_HAVE_LIBFUZZER

static uint32_t xorshift(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void)
{
    static const uint8_t corpus[] = {
        'T', 'K', 'V', '1', 1, 0, 0, 0, 1, 0, 0, 0, 1, 0xD6, 0x14, 0x1B, 0x4D,
        'k', 'v'
    };
    uint32_t seed = 0x5EEDC0DE;
    uint8_t *buffer = (uint8_t *)malloc(4096);
    size_t iteration;
    size_t byte;
    if (buffer == NULL) return 1;

    /* Every single-byte corruption of a valid record must be survived,
     * and every prefix of it as an independent torn write. */
    for (byte = 0; byte < sizeof(corpus); ++byte) {
        uint8_t mutated[sizeof(corpus)];
        memcpy(mutated, corpus, sizeof(corpus));
        mutated[byte] ^= 0xFF;
        LLVMFuzzerTestOneInput(mutated, sizeof(mutated));
        LLVMFuzzerTestOneInput(mutated, byte);
    }
    /* Whole-log shapes through the scanner: valid, torn, and corrupt. */
    LLVMFuzzerTestScan(corpus, sizeof(corpus));
    for (byte = 0; byte <= sizeof(corpus); ++byte)
        LLVMFuzzerTestScan(corpus, byte);
    for (byte = 0; byte <= sizeof(corpus); ++byte)
        LLVMFuzzerTestOneInput(corpus, byte);

    /* Deterministic random buffers, biased toward a valid magic. */
    for (iteration = 0; iteration < 100000; ++iteration) {
        size_t size = xorshift(&seed) % 512;
        size_t i;
        for (i = 0; i < size; ++i)
            buffer[i] = (uint8_t)xorshift(&seed);
        if (iteration % 2 == 0 && size >= 4) {
            buffer[0] = 'T';
            buffer[1] = 'K';
            buffer[2] = 'V';
            buffer[3] = '1';
        }
        LLVMFuzzerTestOneInput(buffer, size);
    }
    free(buffer);
    puts("tinykv fuzz: standalone run clean");
    return 0;
}
#endif
