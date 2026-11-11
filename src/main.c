#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>

#include "db.h"
#include "serial.h"
#include "threadpool.h"
#include "control.h"
#include "cJSON.h"
#include "proto.h"

#define MAX_CLIENTS        1024
#define PUSH_INTERVAL_SEC  2        // 订阅推送周期（秒）
#define MAX_HISTORY_LIMIT  100      // 单次历史查询条数上限，防恶意超大 limit 拖垮内存

// ================= 优雅退出 =================
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;   // 信号处理函数里只「置标志」，不碰任何非异步信号安全的东西
}

// ================= 客户端会话唯一 ID =================
// fd 会被复用，光用 fd 分不清「旧连接没跑完的任务」和「新连接」，所以每次 accept 发一个自增 ID
static uint64_t g_next_session_id = 1;

// ================= 数据接收缓冲区 =================
char serial_rx_buf[4096];
int  serial_rx_len = 0;

typedef struct
{
    int      fd;           // 该槽位对应的连接 fd，-1 表示空槽
    uint64_t session_id;   // 本次连接的唯一 ID（防 fd 复用串台）
    char     recv_buf[4096];
    int      buf_len;
    int      subscribed;   // 1 = 该客户端订阅了周期推送
    char     send_buf[8192];  // 发送缓冲：非阻塞 send 可能发不完，先缓存
    int      send_len;        // send_buf 里待发送的字节数
    int      want_write;      // 1 = 已注册 EPOLLOUT（缓冲没发完，等 socket 可写再发）
} ClientContext;

ClientContext clients[MAX_CLIENTS];

// ================= worker → 主线程 的结果队列 =================
// worker 线程只做耗时 DB 查询 + 组 JSON，真正的 send 交给主线程（socket 只由主线程写，无并发写）
typedef struct Result
{
    int      client_fd;
    uint64_t session_id;
    uint8_t  type;
    char     json[MAX_PAYLOAD + 1];
    struct Result *next;
} Result;

static Result          *g_result_head = NULL;
static Result          *g_result_tail = NULL;
static pthread_mutex_t  g_result_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_result_eventfd = -1;  // 有结果时写 8 字节唤醒主线程

// ================= 最新传感器数据的内存缓存 =================
// 订阅推送直接用缓存，避免主线程为推送去碰数据库（主线程保持只做 I/O）
static double g_latest_temp = 0.0, g_latest_hum = 0.0;
static int    g_latest_ts = 0;
static int    g_has_latest = 0;

// ================= 客户端槽位管理 =================
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
            clients[i].session_id = g_next_session_id++;
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
    ctx->send_len = 0;
    ctx->want_write = 0;
}

// ================= 发送：缓冲 + EPOLLOUT =================
// 非阻塞 send 有三种结果：全发出 / 发一部分 / EAGAIN（发不出去）。
// 只 send 一次的话后两种情况会丢帧、甚至把协议流搞乱。所以先缓存、再尽力 flush。

// 尽力把 ctx 的发送缓冲写进 socket，直到写完或 EAGAIN；写不完登记 EPOLLOUT
static int flush_send(ClientContext *ctx, int epfd)
{
    while (ctx->send_len > 0)
    {
        ssize_t n = send(ctx->fd, ctx->send_buf, ctx->send_len, 0);
        if (n > 0)
        {
            memmove(ctx->send_buf, ctx->send_buf + n, ctx->send_len - n);
            ctx->send_len -= n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!ctx->want_write)
            {
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
                ev.data.fd = ctx->fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, ctx->fd, &ev);
                ctx->want_write = 1;
            }
            return -1;
        }
        return -1;   // 其它错误（对端关闭等），由断开逻辑去收拾
    }

    if (ctx->want_write)
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = ctx->fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, ctx->fd, &ev);
        ctx->want_write = 0;
    }
    return 0;
}

// 组帧 → 塞进发送缓冲 → 立即尝试 flush（只由主线程调用）
static int send_frame(int fd, uint8_t type, const char *json_str, int epfd)
{
    ClientContext *ctx = find_client(fd);
    if (ctx == NULL) return -1;

    uint8_t frame[MAX_FRAME];
    int total_len = proto_pack(type, json_str, frame);
    if (total_len <= 0) return -1;

    // 缓冲装不下就丢弃旧缓冲（推送场景可接受，避免无限增长）
    if (ctx->send_len + total_len > (int)sizeof(ctx->send_buf))
        ctx->send_len = 0;

    memcpy(ctx->send_buf + ctx->send_len, frame, total_len);
    ctx->send_len += total_len;

    return flush_send(ctx, epfd);
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
    if (listen_fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 128) < 0)   // backlog 给足，扛得住压测时的连接突发
    {
        perror("listen");
        close(listen_fd);
        return -1;
    }
    if (set_nonblocking(listen_fd) != 0)
    {
        perror("set_nonblocking");
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

// 周期定时器
int init_timer(int interval_sec)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) { perror("timerfd_create"); return -1; }

    struct itimerspec its;
    its.it_interval.tv_sec  = interval_sec;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec     = interval_sec;
    its.it_value.tv_nsec    = 0;

    if (timerfd_settime(tfd, 0, &its, NULL) != 0)
    {
        perror("timerfd_settime");
        close(tfd);
        return -1;
    }
    return tfd;
}

// 遇到坏帧时，向后扫描下一个魔数 0xA5 0x5A，返回需要跳过的坏字节数。
// 保证每次至少前进 1 字节，避免死循环。
static int resync_frame(const uint8_t *buf, int len)
{
    int step = 1;
    for (int i = 1; i < len - 1; i++)
    {
        if (buf[i] == PROTO_MAGIC_0 && buf[i + 1] == PROTO_MAGIC_1)
        {
            step = i;
            break;
        }
    }
    return step;
}

// ================= 线程池任务 =================
typedef struct
{
    int      client_fd;
    uint64_t session_id;
    uint8_t  type;
    char     query[256];
} QueryTaskArgs;

// worker 线程把组装好的响应入队，并通过 eventfd 唤醒主线程去真正发送
static void enqueue_result(int client_fd, uint64_t session_id, uint8_t type, const char *json)
{
    Result *r = (Result *)malloc(sizeof(Result));
    if (r == NULL) return;   // 内存不足，丢弃这条响应

    r->client_fd = client_fd;
    r->session_id = session_id;
    r->type = type;
    strncpy(r->json, json, sizeof(r->json) - 1);
    r->json[sizeof(r->json) - 1] = '\0';
    r->next = NULL;

    pthread_mutex_lock(&g_result_lock);
    if (g_result_tail == NULL)
        g_result_head = g_result_tail = r;
    else
    {
        g_result_tail->next = r;
        g_result_tail = r;
    }
    pthread_mutex_unlock(&g_result_lock);

    // 写一个 8 字节数唤醒主线程（写失败也无所谓，主线程每次醒来都会整体 drain）
    uint64_t one = 1;
    if (g_result_eventfd >= 0)
        write(g_result_eventfd, &one, sizeof(one));
}

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
        if (str)
        {
            strncpy(reply_json, str, sizeof(reply_json) - 1);
            cJSON_free(str);
        }
        cJSON_Delete(root);
        enqueue_result(args->client_fd, args->session_id, TYPE_QUERY_LATEST, reply_json);
    }
    else if (args->type == TYPE_QUERY_HISTORY)
    {
        int limit = 5;
        cJSON *req = cJSON_Parse(args->query);
        if (req)
        {
            cJSON *l = cJSON_GetObjectItem(req, "limit");
            if (l && l->valueint > 0) limit = l->valueint;
            if (limit > MAX_HISTORY_LIMIT) limit = MAX_HISTORY_LIMIT;
            cJSON_Delete(req);
        }
        db_get_history(limit, reply_json, sizeof(reply_json));
        enqueue_result(args->client_fd, args->session_id, TYPE_QUERY_HISTORY, reply_json);
    }

    free(args);
}

// 主线程：把 worker 产生的结果真正发出去（此刻能准确判断客户端是否还在，且无并发写 socket）
static void drain_results(int epfd)
{
    uint64_t dummy;
    while (read(g_result_eventfd, &dummy, sizeof(dummy)) > 0) {}   // 清空 eventfd 计数器

    for (;;)
    {
        Result *r = NULL;
        pthread_mutex_lock(&g_result_lock);
        r = g_result_head;
        if (r) g_result_head = r->next;
        if (g_result_head == NULL) g_result_tail = NULL;
        pthread_mutex_unlock(&g_result_lock);

        if (r == NULL) break;

        ClientContext *ctx = find_client(r->client_fd);
        // 校验会话 ID：防止客户端已断开、fd 被新连接复用，导致「响应发错人」
        if (ctx != NULL && ctx->session_id == r->session_id)
            send_frame(r->client_fd, r->type, r->json, epfd);
        // 否则客户端已不在，直接丢弃这条过期响应

        free(r);
    }
}

// 把最新一条传感器数据推给所有「已订阅」的客户端（数据来自内存缓存，不碰数据库）
void push_latest_to_subscribers(int epfd)
{
    if (!g_has_latest) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp", g_latest_temp);
    cJSON_AddNumberToObject(root, "hum", g_latest_hum);
    cJSON_AddNumberToObject(root, "ts", g_latest_ts);
    char *str = cJSON_PrintUnformatted(root);
    if (str == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1 && clients[i].subscribed)
        {
            send_frame(clients[i].fd, TYPE_SENSOR_DATA, str, epfd);
        }
    }

    cJSON_free(str);
    cJSON_Delete(root);
}

// ================= 主函数 =================
int main()
{
    signal(SIGPIPE, SIG_IGN);       // 对已断开连接写会触发 SIGPIPE，忽略之，改靠 send 返回值判断
    signal(SIGINT,  handle_signal); // Ctrl+C
    signal(SIGTERM, handle_signal); // kill 默认信号

    if (serial_init("/tmp/gw_tty_read") != 0) return -1;
    if (db_init("gateway.db") != 0) return -1;

    int listen_fd = init_server(8888);
    if (listen_fd < 0) return -1;

    int timer_fd = init_timer(PUSH_INTERVAL_SEC);
    if (timer_fd < 0) return -1;

    int epfd = epoll_create(1);
    if (epfd < 0) { perror("epoll_create"); return -1; }

    g_result_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_result_eventfd < 0) { perror("eventfd"); return -1; }

    struct epoll_event ev, events[64];

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = serial_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serial_fd, &ev);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &ev);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = g_result_eventfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_result_eventfd, &ev);

    printf("=== 网关启动（epoll + 线程池 + 查询/订阅推送 + 优雅退出） ===\n");
    ThreadPool *pool = threadpool_create(3);
    if (pool == NULL)
    {
        fprintf(stderr, "线程池创建失败\n");
        return -1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    while (g_running)
    {
        int nfds = epoll_wait(epfd, events, 64, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                // 被信号打断：若收到退出信号则跳出循环，否则继续等
                if (!g_running) break;
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // 【分支 A】串口有动静
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
                                    // 同步刷新内存缓存，供订阅推送用（主线程不再为推送查库）
                                    g_latest_temp = temp;
                                    g_latest_hum  = hum;
                                    g_latest_ts   = (int)time(NULL);
                                    g_has_latest  = 1;
                                    printf("  -> 已存入数据库\n");
                                }
                                cJSON_Delete(root);
                            }
                            offset += res;
                        }
                        else if (res == -1) { break; }
                        else if (res == -2)
                        {
                            // 坏帧：跳过坏字节、向后找魔数重新同步，别整段清空
                            int bad = resync_frame((const uint8_t *)(serial_rx_buf + offset), serial_rx_len - offset);
                            offset += bad;
                        }
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
            // 【分支 C】周期推送定时器到点
            else if (fd == timer_fd)
            {
                uint64_t exp;
                while (read(timer_fd, &exp, sizeof(exp)) > 0) {}
                push_latest_to_subscribers(epfd);
            }
            // 【分支 D】worker 结果队列有响应要发
            else if (fd == g_result_eventfd)
            {
                drain_results(epfd);
            }
            // 【分支 E】已连接的客户端有动静（可读 / 可写）
            else
            {
                ClientContext *ctx = find_client(fd);
                if (ctx == NULL)
                {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                // 可写：继续把没发完的缓冲发出去
                if (events[i].events & EPOLLOUT)
                {
                    flush_send(ctx, epfd);
                }

                // 可读：收数据
                if (events[i].events & EPOLLIN)
                {
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
                                        send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"subscribed\"}", epfd);
                                        printf("【订阅】fd=%d 订阅了周期推送\n", fd);
                                    }
                                    else if (type == TYPE_SUBSCRIBE_STOP)
                                    {
                                        ctx->subscribed = 0;
                                        send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"unsubscribed\"}", epfd);
                                        printf("【订阅】fd=%d 取消了订阅\n", fd);
                                    }
                                    else if (type == TYPE_CONTROL)
                                    {
                                        // 控制指令：{"device":"relay1","action":"on"|"off"}
                                        const char *device = NULL, *action = NULL;
                                        cJSON *ctrl = cJSON_Parse(json_payload);
                                        if (ctrl)
                                        {
                                            cJSON *d = cJSON_GetObjectItem(ctrl, "device");
                                            cJSON *a = cJSON_GetObjectItem(ctrl, "action");
                                            if (d && cJSON_IsString(d)) device = d->valuestring;
                                            if (a && cJSON_IsString(a)) action = a->valuestring;
                                        }

                                        int ok = 0;
                                        if (device && action)
                                        {
                                            if      (strcmp(action, "on")  == 0) ok = (control_set(device, 1) == 0);
                                            else if (strcmp(action, "off") == 0) ok = (control_set(device, 0) == 0);
                                        }

                                        // 回 ack 帧，告诉客户端执行结果
                                        cJSON *ack = cJSON_CreateObject();
                                        cJSON_AddStringToObject(ack, "device", device ? device : "?");
                                        cJSON_AddStringToObject(ack, "action", action ? action : "?");
                                        cJSON_AddStringToObject(ack, "result", ok ? "ok" : "failed");
                                        char *ack_str = cJSON_PrintUnformatted(ack);
                                        if (ack_str)
                                        {
                                            send_frame(fd, TYPE_CONTROL, ack_str, epfd);
                                            cJSON_free(ack_str);
                                        }
                                        cJSON_Delete(ack);
                                        if (ctrl) cJSON_Delete(ctrl);
                                    }
                                    else
                                    {
                                        QueryTaskArgs *args = (QueryTaskArgs *)malloc(sizeof(QueryTaskArgs));
                                        if (args != NULL)
                                        {
                                            args->client_fd = fd;
                                            args->session_id = ctx->session_id;
                                            args->type = type;
                                            strncpy(args->query, json_payload, sizeof(args->query) - 1);
                                            args->query[sizeof(args->query) - 1] = '\0';
                                            threadpool_add_task(pool, background_db_query, args);
                                        }
                                    }
                                    offset += res;
                                }
                                else if (res == -1) { break; }
                                else if (res == -2)
                                {
                                    int bad = resync_frame((const uint8_t *)(ctx->recv_buf + offset), ctx->buf_len - offset);
                                    offset += bad;
                                }
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
    }

    // ===== 优雅退出：回收线程池、关数据库、关串口、关 fd =====
    printf("=== 收到退出信号，开始优雅退出 ===\n");
    threadpool_destroy(pool, free);   // free 用于释放队列里没跑完的查询任务参数

    // 清空结果队列里可能残留的响应（worker 已全部退出，不会再有新结果进来）
    Result *r = g_result_head;
    while (r)
    {
        Result *next = r->next;
        free(r);
        r = next;
    }
    g_result_head = g_result_tail = NULL;

    db_close();
    serial_close();
    if (g_result_eventfd >= 0) close(g_result_eventfd);
    if (timer_fd >= 0) close(timer_fd);
    close(epfd);
    close(listen_fd);
    printf("=== 网关已干净退出 ===\n");
    return 0;
}
