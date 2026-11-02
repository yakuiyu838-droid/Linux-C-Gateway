#include "proto.h"
#include <string.h>
#include <stdio.h>

static uint8_t calc_checksum(const uint8_t *data, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

int proto_pack(uint8_t type, const char *json_payload, uint8_t *out_buf) {
    int json_len = strlen(json_payload);
    int total_len = HEADER_SIZE + json_len + 1;
    
    FrameHeader *header = (FrameHeader *)out_buf;
    header->magic[0] = 0xA5;
    header->magic[1] = 0x5A;
    header->version = 0x01;
    header->type = type;
    header->len = ((json_len & 0xFF) << 8) | ((json_len >> 8) & 0xFF); 
    
    memcpy(out_buf + HEADER_SIZE, json_payload, json_len);
    out_buf[total_len - 1] = calc_checksum(out_buf, total_len - 1);
    
    return total_len;
}

// 注意这里的参数，必须接收 out_type
int proto_unpack(uint8_t *buf, int buf_len, uint8_t *out_type, uint8_t *out_json, int *out_json_len) {
    if (buf_len < (int)HEADER_SIZE) return -1;
    
    FrameHeader *header = (FrameHeader *)buf;
    if (header->magic[0] != 0xA5 || header->magic[1] != 0x5A) {
        printf("🚨 [协议报错] 找不到魔数! 收到的头: %02X %02X\n", header->magic[0], header->magic[1]);
        return -2;
    }
    
    int json_len = ((header->len & 0xFF) << 8) | ((header->len >> 8) & 0xFF);
    int total_needed = HEADER_SIZE + json_len + 1;
    if (buf_len < total_needed) {
        printf("⏳ [半包等待] 收到 %d 字节，还差 %d 字节\n", buf_len, total_needed);
        return -1;
    }
    
    uint8_t expected_sum = calc_checksum(buf, total_needed - 1);
    if (buf[total_needed - 1] != expected_sum) {
        printf("🚨 [协议报错] 校验和不匹配! 期望: %02X, 实际: %02X\n", expected_sum, buf[total_needed - 1]);
        return -2;
    }
    
    memcpy(out_json, buf + HEADER_SIZE, json_len);
    out_json[json_len] = '\0';
    *out_json_len = json_len;
    *out_type = header->type; 
    
    return total_needed;
}