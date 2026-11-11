#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

// ================= 自定义帧协议（定长头 + 变长体 + 校验）=================
// 帧结构（字节流）：
//   magic(2B) | version(1B) | type(1B) | len(2B 大端) | payload(len B) | checksum(1B)
//   0xA5 0x5A | 0x01        | 指令类型  | payload 长度   | JSON 字符串    | 前面所有字节累加%256
//
// 这是解决 TCP/串口「粘包」的核心：先看头，读出长度，收齐一整帧再拆。

#define PROTO_MAGIC_0  0xA5
#define PROTO_MAGIC_1  0x5A
#define PROTO_VERSION  0x01

// 指令类型
#define TYPE_SENSOR_DATA   0x01  // 传感器数据上报（模拟器 -> 网关；网关 -> 订阅客户端）
#define TYPE_QUERY_LATEST  0x02  // 查询最新值（客户端 -> 网关，网关响应也用这个类型）
#define TYPE_QUERY_HISTORY 0x03  // 查询历史
#define TYPE_CONTROL       0x04  // 控制指令（Phase 3 用）
#define TYPE_HEARTBEAT     0x05  // 心跳 / 应答
#define TYPE_SUBSCRIBE     0x06  // 订阅周期推送（客户端 -> 网关）
#define TYPE_SUBSCRIBE_STOP 0x07 // 取消订阅（客户端 -> 网关）

// payload 上限（超出视为异常帧，防止缓冲区溢出）
#define MAX_PAYLOAD 1024

#pragma pack(push, 1)
typedef struct 
{
    uint8_t  magic[2];   // 魔数 0xA5 0x5A
    uint8_t  version;    // 协议版本 0x01
    uint8_t  type;       // 指令类型
    uint16_t len;        // payload 长度，网络字节序（大端）
} FrameHeader;
#pragma pack(pop)

#define HEADER_SIZE sizeof(FrameHeader)                // 6 字节
#define MAX_FRAME    (HEADER_SIZE + MAX_PAYLOAD + 1)   // 一整帧的最大字节数

// 组帧：type + JSON payload -> 二进制帧，返回帧总长；payload 超长返回 -1
int proto_pack(uint8_t type, const char *json_payload, uint8_t *out_buf);

// 拆帧：从 buf 解析一帧
//   返回值 >0 = 完整一帧的字节数（out_type / out_json / out_json_len 被填充）
//   返回值 -1 = 数据还不够（半包，等更多数据再拆）
//   返回值 -2 = 帧错误（魔数 / 校验 / 超长），调用方应丢弃或重置缓冲区
int proto_unpack(uint8_t *buf, int buf_len, uint8_t *out_type, uint8_t *out_json, int *out_json_len);

#endif
