#include "proto.h"
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>   // htons / ntohs

// 校验和：把 data 前 len 字节累加，取低 8 位
static uint8_t calc_checksum(const uint8_t *data, int len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++)
    {
        sum += data[i];
    }
    return sum;
}

// 发包函数 proto_pack（打包）
int proto_pack(uint8_t type, const char *json_payload, uint8_t *out_buf)
{
    // 算一下 json 字符串有多长，过长的话返回，防止缓冲区溢出
    int json_len = (int)strlen(json_payload);
    if (json_len > MAX_PAYLOAD)
    {
        fprintf(stderr, "proto_pack: payload 超长 %d > %d\n", json_len, MAX_PAYLOAD);
        return -1;
    }

    // 总长度 = 快递单重量（HEADER_SIZE，6 字节）+ 货物重量（json_len）+ 封条重量（+1 是末尾的校验和）
    int total_len = HEADER_SIZE + json_len + 1;

    // 贴快递单
    FrameHeader *header = (FrameHeader *)out_buf;
    header->magic[0]    =   PROTO_MAGIC_0;  // 贴上防伪标识 A5
    header->magic[1]    =   PROTO_MAGIC_1;  // 贴上防伪标识 5A
    header->version     =   PROTO_VERSION;  // 填上版本号
    header->type        =   type;           // 填上货物号
    // 网络字节序（大端），小端变大端
    header->len         =   htons((uint16_t)json_len);
    // out_buf + HEADER_SIZE 意思是跳过已经被占用的 6 个字节，把 JSON 稳稳地拷贝到快递单的后面。
    memcpy(out_buf + HEADER_SIZE, json_payload, json_len);
    // 贴防伪封条
    out_buf[total_len - 1] = calc_checksum(out_buf, total_len - 1);
    return total_len;
}

// 拆包函数 proto_unpack，解决半包
int proto_unpack(uint8_t *buf, int buf_len, uint8_t *out_type, uint8_t *out_json, int *out_json_len)
{
    // 1. 连帧头都不够 -> 半包
    if (buf_len < (int)HEADER_SIZE)
    {
        return -1;
    }
    FrameHeader *header = (FrameHeader *)buf; // 强行把字节流当成头
    // 2. 校验魔数，检查开头是不是 0xA5 0x5A，如果不是，说明是坏包或者被黑客攻击了。
    if (header->magic[0] != PROTO_MAGIC_0 || header->magic[1] != PROTO_MAGIC_1)
    {
        return -2; // -2 表示坏包
    }
    // 3. 读出 payload 长度（网络字节序转主机序）
    int json_len = ntohs(header->len);
    // ntohs 是 htons 的逆向操作（Network to Host）

    // 4. 长度合法性检查（防止恶意帧导致溢出）
    if (json_len > MAX_PAYLOAD)
    {
        return -2;
    }
    // 5. 判断整帧是否收齐
    int total_needed = HEADER_SIZE + json_len + 1;
    if (buf_len < total_needed)
    {
        return -1;
    }
    // 6. 校验和
    uint8_t expected_sum = calc_checksum(buf, total_needed - 1);
    if (buf[total_needed - 1] != expected_sum)
    {
        return -2;
    }
    // 7. 抽出 payload
    memcpy(out_json, buf + HEADER_SIZE, json_len);
    out_json[json_len] = '\0';
    *out_json_len = json_len;
    *out_type = header->type;

    return total_needed;
}
