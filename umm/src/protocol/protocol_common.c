#include "protocol_common.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Write helpers  --  little-endian, byte-by-byte                      */
/* ------------------------------------------------------------------ */

void proto_write_u8(uint8_t *buf, size_t *pos, uint8_t val)
{
    buf[(*pos)++] = val;
}

void proto_write_u16(uint8_t *buf, size_t *pos, uint16_t val)
{
    buf[(*pos)++] = (uint8_t)(val & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 8) & 0xFF);
}

void proto_write_u32(uint8_t *buf, size_t *pos, uint32_t val)
{
    buf[(*pos)++] = (uint8_t)( val        & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >>  8) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 16) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 24) & 0xFF);
}

void proto_write_u64(uint8_t *buf, size_t *pos, uint64_t val)
{
    buf[(*pos)++] = (uint8_t)( val        & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >>  8) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 16) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 24) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 32) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 40) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 48) & 0xFF);
    buf[(*pos)++] = (uint8_t)((val >> 56) & 0xFF);
}

void proto_write_i32(uint8_t *buf, size_t *pos, int32_t val)
{
    proto_write_u32(buf, pos, (uint32_t)val);
}

void proto_write_i64(uint8_t *buf, size_t *pos, int64_t val)
{
    proto_write_u64(buf, pos, (uint64_t)val);
}

void proto_write_str(uint8_t *buf, size_t *pos, const char *str, size_t max_len)
{
    if (str && *str) {
        size_t slen = strlen(str);
        size_t n = (slen < max_len) ? slen : max_len;
        memcpy(&buf[*pos], str, n);
        if (n < max_len)
            memset(&buf[*pos + n], 0, max_len - n);
        *pos += max_len;
    } else {
        memset(&buf[*pos], 0, max_len);
        *pos += max_len;
    }
}

/* ------------------------------------------------------------------ */
/* Read helpers  --  little-endian, byte-by-byte                       */
/* ------------------------------------------------------------------ */

uint8_t proto_read_u8(const uint8_t *buf, size_t *pos)
{
    return buf[(*pos)++];
}

uint16_t proto_read_u16(const uint8_t *buf, size_t *pos)
{
    uint16_t v  = buf[(*pos)++];
    v |= (uint16_t)buf[(*pos)++] << 8;
    return v;
}

uint32_t proto_read_u32(const uint8_t *buf, size_t *pos)
{
    uint32_t v  = buf[(*pos)++];
    v |= (uint32_t)buf[(*pos)++] << 8;
    v |= (uint32_t)buf[(*pos)++] << 16;
    v |= (uint32_t)buf[(*pos)++] << 24;
    return v;
}

uint64_t proto_read_u64(const uint8_t *buf, size_t *pos)
{
    uint64_t v  = buf[(*pos)++];
    v |= (uint64_t)buf[(*pos)++] << 8;
    v |= (uint64_t)buf[(*pos)++] << 16;
    v |= (uint64_t)buf[(*pos)++] << 24;
    v |= (uint64_t)buf[(*pos)++] << 32;
    v |= (uint64_t)buf[(*pos)++] << 40;
    v |= (uint64_t)buf[(*pos)++] << 48;
    v |= (uint64_t)buf[(*pos)++] << 56;
    return v;
}

int32_t proto_read_i32(const uint8_t *buf, size_t *pos)
{
    return (int32_t)proto_read_u32(buf, pos);
}

int64_t proto_read_i64(const uint8_t *buf, size_t *pos)
{
    return (int64_t)proto_read_u64(buf, pos);
}

void proto_read_str(const uint8_t *buf, size_t *pos, char *out, size_t max_len)
{
    memcpy(out, &buf[*pos], max_len);
    out[max_len] = '\0';
    *pos += max_len;
}
