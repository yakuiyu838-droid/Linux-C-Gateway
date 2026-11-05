#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
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

#define MAX_CLIENTS        1024
#define PUSH_INTERVAL_SEC  2      // 订阅推送周期（秒）

// ================= 数据接收缓冲区 =================
char serial_rx_buf[4096];
int  serial_rx_len = 0;

typedef struct
{
    int  fd;             // 该槽位对应的连接 fd，-1 表示空槽
    char recv_buf[4096];
    int  buf_len;
    int  subscribed;    // 1 = 该客户端订阅了周期推送
} ClientContext;

ClientContext clients[MAX_CLIENTS];

// ================= 客户端槽位管理 =================
// 不能拿 fd 直接当数组下标：Linux 的 fd 会一直往上长，连接数一多 fd 超过
// MAX_CLIENTS 就会数组越界。所以用「槽位」存客户端，槽位里记下 fd，再按 fd 反查。
static ClientContext *find_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == fd)
            return &clients[i];
    return NULL;
}

static ClientContext *alloc_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd == -1)
        {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            return &clients[i];
        }
    }
    return NULL;   // 连接数打满
}

static void free_client(ClientContext *ctx)
{
    ctx->fd = -1;
    ctx->buf_len = 0;
    ctx->subscribed = 0;
}

// ================= 通用发送函数 =================
void send_frame(int fd, uint8_t type, const char *json_str)
{
    uint8_t frame[MAX_FRAME];
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

// 周期定时器：每 interval_sec 秒触发一次，用来给订阅客户端推最新数据
int init_timer(int interval_sec)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) { perror("timerfd_create"); return -1; }

    struct itimerspec its;
    its.it_interval.tv_sec  = interval_sec;   // 周期：每隔 interval_sec 秒触发一次
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec     = interval_sec;   // 首次触发：interval_sec 秒后
    its.it_value.tv_nsec    = 0;

    if (timerfd_settime(tfd, 0, &its, NULL) != 0)
    {
        perror("timerfd_settime");
        close(tfd);
        return -1;
    }
    return tfd;
}

// ================= 线程池任务 =================
typedef struct
{
    int     client_fd;
    uint8_t type;
    char    query[256];     // 请求里的 JSON 参数（比如历史查询的 limit）
} QueryTaskArgs;

// 后台查库：查最新 / 查历史，把结果打包发回客户端
void background_db_query(void *arg)
{
    QueryTaskArgs *args = (QueryTaskArgs *)arg;
    char reply_json[2048] = {0};

    if (args->type == TYPE_QUERY_LATEST)
    {
        double temp, hum; int ts;
        cJSON *root = cJSON_CreateObject();
        if (db_get_latest(&temp, &hum, &ts) == 0)
        {
            cJSON_AddNumberToObject(root, "temp", temp);
            cJSON_AddNumberToObject(root, "hum", hum);
            cJSON_AddNumberToObject(root, "ts", ts);
        }
        else
        {
            cJSON_AddStringToObject(root, "error", "no data");
        }
        char *str = cJSON_PrintUnformatted(root);
        strncpy(reply_json, str, sizeof(reply_json) - 1);
        cJSON_free(str);
        cJSON_Delete(root);
        send_frame(args->client_fd, TYPE_QUERY_LATEST, reply_json);
    }
    else if (args->type == TYPE_QUERY_HISTORY)
    {
        int limit = 5;
        cJSON *req = cJSON_Parse(args->query);
        if (req)
        {
            cJSON *l = cJSON_GetObjectItem(req, "limit");
            if (l && l->valueint > 0) limit = l->valueint;
            cJSON_Delete(req);
        }
        db_get_history(limit, reply_json, sizeof(reply_json));
        send_frame(args->client_fd, TYPE_QUERY_HISTORY, reply_json);
    }

    free(args);
}

// 把最新一条传感器数据推给所有「已订阅」的客户端
void push_latest_to_subscribers(void)
{
    double temp, hum; int ts;
    if (db_get_latest(&temp, &hum, &ts) != 0) return;   // 库里还没数据，跳过

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp", temp);
    cJSON_AddNumberToObject(root, "hum", hum);
    cJSON_AddNumberToObject(root, "ts", ts);
    char *str = cJSON_PrintUnformatted(root);

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 && clients[i].subscribed)
        {
            send_frame(clients[i].fd, TYPE_SENSOR_DATA, str);   // 推送用「传感器数据」帧
        }
    }

    cJSON_free(str);
    cJSON_Delete(root);
}

// ================= 主函数 =================
int main()
{
    signal(SIGPIPE, SIG_IGN);

    if (serial_init("/tmp/gw_tty_read") != 0) return -1;
    if (db_init("gateway.db") != 0) return -1;

    int listen_fd = init_server(8888);
    int timer_fd  = init_timer(PUSH_INTERVAL_SEC);
    int epfd = epoll_create(1);
    struct epoll_event ev, events[16];

    // 1. 注册串口
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = serial_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serial_fd, &ev);

    // 2. 注册网络监听
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    // 3. 注册周期推送定时器
    if (timer_fd >= 0)
    {
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = timer_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &ev);
    }

    printf("=== 网关启动（Phase 2：协议 + 查询 + 订阅推送） ===\n");
    ThreadPool *pool = threadpool_create(3);

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;   // 初始化客户端槽位

    while (1)
    {
        int nfds = epoll_wait(epfd, events, 16, -1);
        if (nfds < 0)
        {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // 【分支 A】串口有动静：读传感器帧 -> 落库
            if (fd == serial_fd)
            {
                for (;;)
                {
                    char buf[256];
                    int len = serial_read(buf, sizeof(buf));
                    if (len < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读干了，ET 下必须 break，否则死循环 CPU 100%
                        perror("串口读出错");
                        break;
                    }
                    if (len == 0) break;

                    if (serial_rx_len + len > (int)sizeof(serial_rx_buf))
                    {
                        serial_rx_len = 0;   // 防溢出
                    }
                    memcpy(serial_rx_buf + serial_rx_len, buf, len);
                    serial_rx_len += len;

                    int offset = 0;
                    while (serial_rx_len - offset > 0)
                    {
                        uint8_t type;
                        char json_payload[MAX_PAYLOAD + 1];
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
                    }
                    else if (offset == serial_rx_len)
                    {
                        serial_rx_len = 0;
                    }
                }
            }

            // 【分支 B】新客户端连进来
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

                    ClientContext *ctx = alloc_client(c_fd);
                    if (ctx == NULL)
                    {
                        fprintf(stderr, "【网络】连接数已满，拒绝 fd=%d\n", c_fd);
                        close(c_fd);
                        continue;
                    }

                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }

            // 【分支 C】周期推送定时器到点：给订阅客户端推最新数据
            else if (fd == timer_fd)
            {
                uint64_t exp;
                while (read(timer_fd, &exp, sizeof(exp)) > 0) {}   // 排空（ET 下要读到 EAGAIN）
                push_latest_to_subscribers();
            }

            // 【分支 D】已连接的客户端发消息
            else
            {
                ClientContext *ctx = find_client(fd);
                if (ctx == NULL)
                {
                    // 防御性兜底：fd 既然在 epoll 里，就该有对应槽位
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                for (;;)
                {
                    char buf[256];
                    int len = recv(fd, buf, sizeof(buf) - 1, 0);

                    if (len > 0)
                    {

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
                            char json_payload[MAX_PAYLOAD + 1];
                            int json_len;

                            int res = proto_unpack((uint8_t *)(ctx->recv_buf + offset), ctx->buf_len - offset, &type, (uint8_t *)json_payload, &json_len);

                            if (res > 0)
                            {
                                printf("【网络拆包】fd=%d Type: %d, JSON: %s\n", fd, type, json_payload);

                                if (type == TYPE_SUBSCRIBE)
                                {
                                    ctx->subscribed = 1;
                                    send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"subscribed\"}");
                                    printf("【订阅】fd=%d 订阅了周期推送\n", fd);
                                }
                                else if (type == TYPE_SUBSCRIBE_STOP)
                                {
                                    ctx->subscribed = 0;
                                    send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"unsubscribed\"}");
                                    printf("【订阅】fd=%d 取消了订阅\n", fd);
                                }
                                else
                                {
                                    // 查询类：丢线程池异步执行（耗时的解析/查库不阻塞 epoll 主循环）
                                    QueryTaskArgs *args = (QueryTaskArgs *)malloc(sizeof(QueryTaskArgs));
                                    if (args != NULL)
                                    {
                                        args->client_fd = fd;
                                        args->type = type;
                                        strncpy(args->query, json_payload, sizeof(args->query) - 1);
                                        args->query[sizeof(args->query) - 1] = '\0';
                                        threadpool_add_task(pool, background_db_query, args);
                                    }
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
                        free_client(ctx);
                        printf("【网络】客户端 fd=%d 断开\n", fd);
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        free_client(ctx);
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
    if (timer_fd >= 0) close(timer_fd);
    close(epfd);
    return 0;
}
