/* Linux host direct I/O. Works without accelerator libraries. */
#define _GNU_SOURCE
#include "ssd_backend_direct.h"
#include "../common/error_codes.h"
#include "../common/log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define IO_ALIGN 4096ULL
struct SsdDirect {
    int fd;
    uint64_t base, capacity, alignment;
    uint64_t reads, writes, read_bytes, write_bytes, bounce_bytes;
};
/* One reusable bounce buffer per host thread, shared across direct devices.
 * The benchmark supplies aligned buffers and therefore never uses this path. */
typedef struct { void *ptr; size_t size, align; } Bounce;
static pthread_key_t bounce_key;
static pthread_once_t bounce_once = PTHREAD_ONCE_INIT;
static int bounce_key_error;
static void destroy_bounce(void *p) { Bounce *b = p; if (b) { free(b->ptr); free(b); } }
static void create_bounce_key(void) { bounce_key_error = pthread_key_create(&bounce_key, destroy_bounce); }
static void *get_bounce(size_t size, size_t align)
{
    pthread_once(&bounce_once, create_bounce_key);
    if (bounce_key_error) return NULL;
    Bounce *b = pthread_getspecific(bounce_key);
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return NULL;
        if (pthread_setspecific(bounce_key, b)) { free(b); return NULL; }
    }
    if (size > b->size || align > b->align) {
        void *p = NULL;
        if (posix_memalign(&p, align, size)) return NULL;
        free(b->ptr); b->ptr = p; b->size = size; b->align = align;
    }
    return b->ptr;
}

int ssd_direct_open(const char *spec, uint64_t capacity, SsdDirect **out)
{
    if (!spec || !out || !capacity || capacity % IO_ALIGN) return UMM_E_INVALID_ARG;
    *out = NULL;
    if (spec[0] < '0' || spec[0] > '9') return UMM_E_INVALID_ARG;
    char *end;
    errno = 0;
    uint64_t base = strtoull(spec, &end, 0);
    if (errno || end == spec || *end != ':' || end[1] != '/' || base % IO_ALIGN ||
        base > INT64_MAX || capacity > (uint64_t)INT64_MAX - base)
        return UMM_E_INVALID_ARG;
    const char *path = end + 1;
    struct stat st;
    if (stat(path, &st)) return UMM_E_NOT_FOUND;
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) return UMM_E_INVALID_ARG;
    int block = S_ISBLK(st.st_mode);
    const char *allow = getenv("UMM_ALLOW_BLOCK_DEVICE");
    if (block && (!allow || strcmp(allow, "1"))) return UMM_E_UNSUPPORTED;
    int fd = open(path, O_RDWR | O_DIRECT | O_CLOEXEC | (block ? O_EXCL : 0));
    if (fd < 0) {
        umm_log_error("direct: open(%s) failed: %s (no buffered fallback)", path, strerror(errno));
        return UMM_E_IO;
    }
    int rc = UMM_E_INVALID_ARG;
    if (fstat(fd, &st) || (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) ||
        block != !!S_ISBLK(st.st_mode)) goto fail;
    uint64_t bytes = (uint64_t)st.st_size;
    if (block) {
        int sector = 0;
        if (ioctl(fd, BLKGETSIZE64, &bytes) || ioctl(fd, BLKSSZGET, &sector) ||
            sector <= 0 || IO_ALIGN % (uint64_t)sector) goto fail;
    } else {
        struct statfs fs;
        if (fstatfs(fd, &fs)) goto fail;
        /* tmpfs may accept and ignore O_DIRECT. Never label RAM as disk I/O. */
        if (fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC) {
            rc = UMM_E_UNSUPPORTED; goto fail;
        }
    }
    if (base > bytes || capacity > bytes - base) goto fail;
    SsdDirect *b = calloc(1, sizeof(*b));
    if (!b) { rc = UMM_E_NO_MEMORY; goto fail; }
    b->fd = fd; b->base = base; b->capacity = capacity;
    long page = sysconf(_SC_PAGESIZE);
    b->alignment = page > (long)IO_ALIGN ? (uint64_t)page : IO_ALIGN;
    *out = b;
    umm_log_info("direct: opened %s base=%lu capacity=%lu alignment=%lu (O_DIRECT, no mmap)",
                 path, (unsigned long)base, (unsigned long)capacity, (unsigned long)b->alignment);
    return UMM_OK;
fail:
    close(fd);
    return rc;
}

static int direct_io(SsdDirect *b, uint64_t off, uint64_t len, void *buf, int write_op)
{
    if (!b || (!buf && len) || off > b->capacity || len > b->capacity - off ||
        off % IO_ALIGN || len % IO_ALIGN || len > SIZE_MAX) return UMM_E_INVALID_ARG;
    if (!len) return UMM_OK;
    void *io_buf = buf;
    if ((uintptr_t)buf % b->alignment) {
        io_buf = get_bounce((size_t)len, (size_t)b->alignment);
        if (!io_buf) return UMM_E_NO_MEMORY;
        if (write_op) memcpy(io_buf, buf, (size_t)len);
        __atomic_fetch_add(&b->bounce_bytes, len, __ATOMIC_RELAXED);
    }
    uint64_t done = 0;
    while (done < len) {
        /* Linux caps one transfer at MAX_RW_COUNT; preserve alignment. */
        size_t nbytes = (size_t)(len - done);
        size_t limit = (size_t)0x7ffff000 & ~((size_t)b->alignment - 1);
        if (nbytes > limit) nbytes = limit;
        ssize_t n = write_op ? pwrite(b->fd, (char *)io_buf + done, nbytes, b->base + off + done)
                             : pread(b->fd, (char *)io_buf + done, nbytes, b->base + off + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            umm_log_error("direct: %s off=%lu len=%lu failed: %s",
                write_op ? "pwrite" : "pread", (unsigned long)(b->base + off + done),
                (unsigned long)nbytes, n == 0 ? "zero/EOF" : strerror(errno));
            return UMM_E_IO;
        }
        __atomic_fetch_add(write_op ? &b->writes : &b->reads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(write_op ? &b->write_bytes : &b->read_bytes, (uint64_t)n, __ATOMIC_RELAXED);
        done += (uint64_t)n;
        /* A short transfer may no longer leave an aligned buffer/offset. */
        if (done < len && ((uint64_t)n % b->alignment)) return UMM_E_IO;
    }
    if (io_buf != buf && !write_op) memcpy(buf, io_buf, (size_t)len);
    return UMM_OK;
}
int ssd_direct_read(SsdDirect *b, uint64_t off, uint64_t len, void *buf)
{ return direct_io(b, off, len, buf, 0); }
int ssd_direct_write(SsdDirect *b, uint64_t off, uint64_t len, const void *buf)
{ return direct_io(b, off, len, (void *)buf, 1); }
int ssd_direct_sync(SsdDirect *b)
{
    if (!b) return UMM_E_INVALID_ARG;
    int rc;
    do { rc = fdatasync(b->fd); } while (rc && errno == EINTR);
    return rc ? UMM_E_IO : UMM_OK;
}
void ssd_direct_close(SsdDirect *b)
{ if (b) { close(b->fd); free(b); } }
int ssd_direct_stats(SsdDirect *b, SsdDirectStats *out, int reset)
{
    if (!b || !out) return UMM_E_INVALID_ARG;
#define SNAP(field, member) out->field = reset ? __atomic_exchange_n(&b->member, 0, __ATOMIC_RELAXED) : __atomic_load_n(&b->member, __ATOMIC_RELAXED)
    SNAP(read_calls, reads); SNAP(write_calls, writes);
    SNAP(read_bytes, read_bytes); SNAP(write_bytes, write_bytes); SNAP(bounce_bytes, bounce_bytes);
#undef SNAP
    out->alignment = b->alignment; out->window_base = b->base;
    return UMM_OK;
}
