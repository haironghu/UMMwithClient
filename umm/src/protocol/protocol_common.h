#ifndef UMM_PROTOCOL_COMMON_H
#define UMM_PROTOCOL_COMMON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Little-endian write helpers                                         */
/* ------------------------------------------------------------------ */
void proto_write_u8 (uint8_t *buf, size_t *pos, uint8_t  val);
void proto_write_u16(uint8_t *buf, size_t *pos, uint16_t val);
void proto_write_u32(uint8_t *buf, size_t *pos, uint32_t val);
void proto_write_u64(uint8_t *buf, size_t *pos, uint64_t val);
void proto_write_i32(uint8_t *buf, size_t *pos, int32_t  val);
void proto_write_i64(uint8_t *buf, size_t *pos, int64_t  val);

/* Write exactly max_len bytes (zero-padded), str may be shorter or NULL */
void proto_write_str(uint8_t *buf, size_t *pos, const char *str, size_t max_len);

/* ------------------------------------------------------------------ */
/* Little-endian read helpers                                          */
/* ------------------------------------------------------------------ */
uint8_t  proto_read_u8 (const uint8_t *buf, size_t *pos);
uint16_t proto_read_u16(const uint8_t *buf, size_t *pos);
uint32_t proto_read_u32(const uint8_t *buf, size_t *pos);
uint64_t proto_read_u64(const uint8_t *buf, size_t *pos);
int32_t  proto_read_i32(const uint8_t *buf, size_t *pos);
int64_t  proto_read_i64(const uint8_t *buf, size_t *pos);

/* Read max_len bytes, always NUL-terminate out (max_len chars + '\0') */
void proto_read_str(const uint8_t *buf, size_t *pos, char *out, size_t max_len);

/* ------------------------------------------------------------------ */
/* Token digest（Phase 1 鉴权）                                         */
/* FNV-1a 64-bit 摘要的低 6 字节，填入 UmmProtoHeader.reserved[6]。     */
/* token 为 NULL/空串 时输出全 0（= 未启用鉴权的旧客户端线格式）。        */
/* ------------------------------------------------------------------ */
void umm_token_digest(const char *token, uint8_t out6[6]);

#ifdef __cplusplus
}
#endif

#endif /* UMM_PROTOCOL_COMMON_H */
