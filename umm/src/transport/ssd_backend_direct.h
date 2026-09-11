#ifndef SSD_BACKEND_DIRECT_H
#define SSD_BACKEND_DIRECT_H
#include <stdint.h>

typedef struct SsdDirect SsdDirect;
/* Counters are actual successful kernel I/O calls/bytes, not logical KV rows.
 * Snapshot/reset only at a quiescent benchmark boundary. */
typedef struct {
    uint64_t read_calls, write_calls, read_bytes, write_bytes, bounce_bytes;
    uint64_t alignment, window_base;
} SsdDirectStats;
/* spec = decimal_or_hex_window_base:/absolute/path, existing target only.
 * capacity and all I/O offsets/lengths must be 4 KiB aligned. No mmap/fallback.
 * Block devices require UMM_ALLOW_BLOCK_DEVICE=1 and are opened exclusively. */
int ssd_direct_open(const char *spec, uint64_t capacity, SsdDirect **out);
void ssd_direct_close(SsdDirect *b);
int ssd_direct_read(SsdDirect *b, uint64_t off, uint64_t len, void *buf);
int ssd_direct_write(SsdDirect *b, uint64_t off, uint64_t len, const void *buf);
int ssd_direct_sync(SsdDirect *b);
int ssd_direct_stats(SsdDirect *b, SsdDirectStats *out, int reset);
#endif
