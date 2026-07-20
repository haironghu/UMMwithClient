#ifndef UMM_TYPES_H
#define UMM_TYPES_H

/* All public types are defined in include/umm.h */
#include "../../include/umm.h"

/* Internal wire protocol types */

#define UMM_PROTO_MAGIC     "UMMR"
#define UMM_PROTO_VERSION   1
#define UMM_PROTO_MAX_BODY  4096

typedef struct {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  opcode;
    uint8_t  flags;
    uint8_t  reserved0;
    uint16_t body_len;
    uint8_t  reserved[6];
} UmmProtoHeader;

_Static_assert(sizeof(UmmProtoHeader) == 16, "UmmProtoHeader must be 16 bytes");

typedef struct {
    uint8_t  data[UMM_PROTO_MAX_BODY];
    uint16_t len;
} UmmProtoBody;

#define UMM_FLAG_REQUEST  0x00
#define UMM_FLAG_RESPONSE 0x01

#endif /* UMM_TYPES_H */
