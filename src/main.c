#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>

#include "db.h"
#include "serial.h"
#include "threadpool.h"
#include "cJSON.h" 
#include "proto.h" 

// ================= 数据接收缓冲区 =================
char serial_rx_buf[4096];
int serial_rx_len = 0;

typedef struct 
{
    char recv_buf[4096];
    int buf_len;
} ClientContext;

ClientContext clients[1024]; 

// ================= 通用发送函数 =================
void send_frame(int fd, uint8_t type, const char *json_str) 
{
    uint8_t frame[1024];
    int total_len = proto_pack(type, json_str, frame);
    if (total_len > 0) 
    {
        send(fd, frame, total_len, 0);
    }
}

// ================= 辅助函数 =================
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int init_server(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);

    if (set_nonblocking(listen_fd) != 0) 
    {
        perror("set_nonblocking");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

typedef struct 
{
    int client_fd;
    char query[256];
} QueryTaskArgs;

// ================= 线程池任务 =================
void background_db_query(void *arg)
{
    QueryTaskArgs *args = (QueryTaskArgs *)arg;
    double temp, hum;
    int ts;
    
    cJSON *root = cJSON_CreateObject();

    if (db_get_latest(&temp, &hum, &ts) == 0) 
    {
        cJSON_AddNumberToObject(root, "temp", temp);
        cJSON_AddNumberToObject(root, "hum", hum);
        cJSON_AddNumberToObject(root, "timestamp", ts);
    } 
    else 
    {
        cJSON_AddStringToObject(root, "error", "no data");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);

    // 发送类型 1：代表查询响应
    send_frame(args->client_fd, 1, json_str); 

    cJSON_free(json_str);
    cJSON_Delete(root);
    free(args);
}

// ================= 主函数 =================
int main()
{
    signal(SIGPIPE, SIG_IGN);

    if (serial_init("/tmp/gw_tty_read") != 0) return -1;
    if (db_init("gateway.db") != 0) return -1;

    int listen_fd = init_server(8888);
    int epfd = epoll_create(1);
    struct epoll_event ev, events[10];

    // 1. 注册串口
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = serial_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serial_fd, &ev);

    // 2. 注册网络监听
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("=== 网关终极版启动 (统一协议) ===\n");
    ThreadPool *pool = threadpool_create(3);

    while (1)
    {
        int nfds = epoll_wait(epfd, events, 10, -1);
        if (nfds < 0) 
        {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // 【分支 A】：串口有动静
            if (fd == serial_fd) 
            {
                for (;;) 
                {
                    char buf[256];
                    int len = serial_read(buf, sizeof(buf));
                    if (len < 0) 
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("串口读出错");
                        break;
                    }
                    if (len == 0) break;

                    if (serial_rx_len + len > (int)sizeof(serial_rx_buf)) 
                    {
                        serial_rx_len = 0; 
                    }
                    memcpy(serial_rx_buf + serial_rx_len, buf, len);
                    serial_rx_len += len;

                    int offset = 0;
                    while (serial_rx_len - offset > 0) 
                    {
                        uint8_t type;
                        char json_payload[1024];
                        int json_len;
                        
                        int res = proto_unpack((uint8_t *)(serial_rx_buf + offset), serial_rx_len - offset, &type, (uint8_t *)json_payload, &json_len);
                        
                        if (res > 0) 
                        {
                            printf("【串口拆包】Type: %d, JSON: %s\n", type, json_payload);
                            
                            cJSON *root = cJSON_Parse(json_payload);
                            if (root) 
                            {
                                cJSON *temp_node = cJSON_GetObjectItem(root, "temp");
                                cJSON *hum_node = cJSON_GetObjectItem(root, "hum");
                                if (temp_node && hum_node) 
                                {
                                    double temp = temp_node->valuedouble;
                                    double hum = hum_node->valuedouble;
                                    db_insert_sensor(temp, hum, (int)time(NULL));
                                    printf("  -> 已存入数据库\n");
                                }
                                cJSON_Delete(root);
                            }
                            offset += res;
                        } 
                        else if (res == -1) { break; } 
                        else if (res == -2) { serial_rx_len = 0; break; }
                    }

                    if (offset > 0 && offset < serial_rx_len) 
                    {
                        memmove(serial_rx_buf, serial_rx_buf + offset, serial_rx_len - offset);
                        serial_rx_len -= offset;
                    } else if (offset == serial_rx_len) {
                        serial_rx_len = 0; 
                    }
                }
            }
            
            // 【分支 B】：新客户端连进来
            else if (fd == listen_fd)
            {
                for (;;)
                {
                    int c_fd = accept(listen_fd, NULL, NULL);
                    if (c_fd < 0) 
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; 
                        perror("accept");
                        break;
                    }

                    set_nonblocking(c_fd);
                    printf("【网络】客户端接入 fd=%d\n", c_fd);

                    clients[c_fd].buf_len = 0;
                    memset(clients[c_fd].recv_buf, 0, sizeof(clients[c_fd].recv_buf));

                    ev.events = EPOLLIN ;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }

            // 【分支 C】：已连接的客户端发消息
            else
            {
                for (;;)
                {
                    char buf[256];
                    int len = recv(fd, buf, sizeof(buf) - 1, 0);

                    if (len > 0)
                    {
                        // 👈 这里已经把多余的那个 if 删了，恢复正常逻辑
                        printf("📥 [网络调试] recv 实际收到 %d 字节\n", len); 
                        
                        ClientContext *ctx = &clients[fd];
                        
                        if (ctx->buf_len + len > (int)sizeof(ctx->recv_buf))
                        {
                            ctx->buf_len = 0; 
                        }
                        memcpy(ctx->recv_buf + ctx->buf_len, buf, len);
                        ctx->buf_len += len;

                        int offset = 0;
                        while (ctx->buf_len - offset > 0) 
                        {
                            uint8_t type;
                            char json_payload[1024];
                            int json_len;
                            
                            int res = proto_unpack((uint8_t *)(ctx->recv_buf + offset), ctx->buf_len - offset, &type, (uint8_t *)json_payload, &json_len);
                            
                            if (res > 0) 
                            {
                                printf("【网络拆包】Type: %d, JSON: %s\n", type, json_payload);
                                
                                QueryTaskArgs *args = (QueryTaskArgs *)malloc(sizeof(QueryTaskArgs));
                                if (args != NULL) 
                                {
                                    args->client_fd = fd;
                                    strcpy(args->query, json_payload);
                                    threadpool_add_task(pool, background_db_query, args);
                                }
                                offset += res;
                            } 
                            else if (res == -1) { break; } 
                            else if (res == -2) { ctx->buf_len = 0; break; }
                        }

                        if (offset > 0 && offset < ctx->buf_len) 
                        {
                            memmove(ctx->recv_buf, ctx->recv_buf + offset, ctx->buf_len - offset);
                            ctx->buf_len -= offset;
                        } 
                        else if (offset == ctx->buf_len) 
                        {
                            ctx->buf_len = 0;
                        }
                    }
                    else if (len == 0)
                    {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        clients[fd].buf_len = 0;
                        printf("【网络】客户端 fd=%d 断开\n", fd);
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        clients[fd].buf_len = 0;
                        printf("【网络】客户端 fd=%d 出错断开\n", fd);
                        break;
                    }
                }
            }
        }
    }

    threadpool_destroy(pool);
    db_close();
    serial_close();
    return 0;
}