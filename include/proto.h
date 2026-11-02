#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[2]; // 0xA5 0x5A
    uint8_t version;  // 0x01
    uint8_t type;     // 指令类型
    uint16_t len;     // payload 长度（大端序）
} FrameHeader;
#pragma pack(pop)

#define HEADER_SIZE sizeof(FrameHeader)

// 注意这里，必须有 5 个参数！
int proto_unpack(uint8_t *buf, int buf_len, uint8_t *out_type, uint8_t *out_json, int *out_json_len);
int proto_pack(uint8_t type, const char *json_payload, uint8_t *out_buf);

#endif