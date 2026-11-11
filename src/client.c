#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "proto.h"
#include "cJSON.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8888

static int sock;

// 阻塞地等待一个完整帧，拆出 type 和 json payload（自带缓冲区，自动处理粘包/半包）
// 返回 >0 = 帧长度；<0 = 出错或对端断开
static int recv_frame(uint8_t *type, char *json_payload, int *json_len)
{
    static uint8_t buf[4096]; 
    static int buf_len = 0;

    while (1)
    {
        int res = proto_unpack(buf, buf_len, type, (uint8_t *)json_payload, json_len);
        if (res > 0)
        {
            // 一帧拆完了，把用掉的字节从缓冲区里挪走
            memmove(buf, buf + res, buf_len - res);
            buf_len -= res;
            return res;
        }
        if (res == -2)   // 坏包，丢弃缓冲区重新同步
        {
            buf_len = 0;
        }

        int n = recv(sock, buf + buf_len, (int)sizeof(buf) - buf_len, 0);
        if (n <= 0) return -1;
        buf_len += n;
    }
}

// 发一个请求帧
static void send_cmd(uint8_t type, const char *json_str)
{
    uint8_t frame[MAX_FRAME];
    int total_len = proto_pack(type, json_str, frame);
    if (total_len > 0) send(sock, frame, total_len, 0);
}

int main()
{
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("连接网关失败");
        return -1;
    }
    printf("✅ 成功连接到网关 %s:%d\n", SERVER_IP, PORT);
    printf("指令：\n  latest       查最新一条\n  history [N]  查最近 N 条\n  subscribe    订阅周期推送（Ctrl+C 退出）\n  exit         退出\n");

    char input[256];
    while (1)
    {
        printf("\n> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\r\n")] = '\0';   // 去掉回车

        if (strcmp(input, "exit") == 0) break;

        uint8_t type;
        char json_payload[MAX_PAYLOAD + 1];
        int json_len;

        if (strcmp(input, "latest") == 0)
        {
            send_cmd(TYPE_QUERY_LATEST, "{}");
            if (recv_frame(&type, (char *)json_payload, &json_len) > 0)
            {
                printf("📥 最新数据：\n%s\n", json_payload);
            }
            else printf("❌ 接收响应失败\n");
        }
        else if (strncmp(input, "history", 7) == 0)
        {
            int limit = 5;
            sscanf(input, "history %d", &limit);
            if (limit <= 0) limit = 5;

            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "limit", limit);
            char *json_str = cJSON_PrintUnformatted(root);
            send_cmd(TYPE_QUERY_HISTORY, json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);

            if (recv_frame(&type, (char *)json_payload, &json_len) > 0)
            {
                printf("📥 历史数据：\n%s\n", json_payload);
            }
            else printf("❌ 接收响应失败\n");
        }
        else if (strcmp(input, "subscribe") == 0)
        {
            send_cmd(TYPE_SUBSCRIBE, "{}");

            // 先收一条 ack
            if (recv_frame(&type, (char *)json_payload, &json_len) > 0)
            {
                printf("✅ 订阅成功，开始接收周期推送（Ctrl+C 退出）...\n");
            }

            // 持续接收推送，直到对端断开或 Ctrl+C
            while (1)
            {
                if (recv_frame(&type, (char *)json_payload, &json_len) > 0)
                {
                    printf("📡 [推送] %s\n", json_payload);
                }
                else
                {
                    printf("❌ 与网关的连接断开\n");
                    close(sock);
                    return 0;
                }
            }
        }
        else
        {
            printf("❌ 未知指令，请输入 latest / history [N] / subscribe / exit\n");
        }
    }

    close(sock);
    printf("已断开连接。\n");
    return 0;
}
