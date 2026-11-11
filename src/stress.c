// 压测客户端：开 N 个并发连接，每个连接打 M 个查询（latest / history 交替），统计成功率与吞吐
// 用法：./stress [连接数] [每连接请求数]
// 例：  ./stress 200 100   （200 并发连接，每连接 100 个查询，共 2 万请求）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "proto.h"

#define SERVER_IP "127.0.0.1"
#define PORT      8888

typedef struct
{
    int      id;
    int      sock;
    int      req_count;   // 这个连接要发多少个查询
    int      ok;
    int      fail;
    // 每个线程自己的接收缓冲（多线程下不能再像 client.c 那样用 static 共享缓冲，会串台）
    uint8_t  buf[4096];
    int      buf_len;
} WorkerCtx;

// 阻塞收一帧（自带缓冲区，处理粘包/半包）
static int recv_frame(WorkerCtx *w, uint8_t *type, char *json, int *json_len)
{
    while (1)
    {
        int res = proto_unpack(w->buf, w->buf_len, type, (uint8_t *)json, json_len);
        if (res > 0)
        {
            memmove(w->buf, w->buf + res, w->buf_len - res);
            w->buf_len -= res;
            return res;
        }
        if (res == -2) { w->buf_len = 0; }   // 坏包，丢弃重新同步

        int n = recv(w->sock, w->buf + w->buf_len, (int)sizeof(w->buf) - w->buf_len, 0);
        if (n <= 0) return -1;
        w->buf_len += n;
    }
}

static void send_cmd(int sock, uint8_t type, const char *json)
{
    uint8_t frame[MAX_FRAME];
    int total = proto_pack(type, json, frame);
    if (total > 0) send(sock, frame, total, 0);
}

static void *worker(void *arg)
{
    WorkerCtx *w = (WorkerCtx *)arg;
    for (int i = 0; i < w->req_count; i++)
    {
        uint8_t type;
        char json[MAX_PAYLOAD + 1];
        int json_len;

        // 交替发 latest / history，模拟混合负载
        if (i % 2 == 0)
            send_cmd(w->sock, TYPE_QUERY_LATEST, "{}");
        else
            send_cmd(w->sock, TYPE_QUERY_HISTORY, "{\"limit\":10}");

        if (recv_frame(w, &type, json, &json_len) > 0)
            w->ok++;
        else
            w->fail++;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int conns = argc > 1 ? atoi(argv[1]) : 100;
    int reqs  = argc > 2 ? atoi(argv[2]) : 100;
    if (conns <= 0) conns = 100;
    if (reqs  <= 0) reqs  = 100;

    WorkerCtx *w = (WorkerCtx *)calloc(conns, sizeof(WorkerCtx));
    pthread_t *t = (pthread_t *)calloc(conns, sizeof(pthread_t));
    if (!w || !t) { fprintf(stderr, "内存不足\n"); return -1; }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    // 1. 建立所有连接
    for (int i = 0; i < conns; i++)
    {
        w[i].id = i;
        w[i].req_count = reqs;
        w[i].sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
        if (connect(w[i].sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            perror("connect");
            w[i].sock = -1;
        }
    }

    // 2. 起线程，每个连接一个 worker
    for (int i = 0; i < conns; i++)
        if (w[i].sock >= 0)
            pthread_create(&t[i], NULL, worker, &w[i]);

    // 3. 等所有 worker 跑完
    for (int i = 0; i < conns; i++)
        if (w[i].sock >= 0)
            pthread_join(t[i], NULL);

    gettimeofday(&t1, NULL);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

    int ok = 0, fail = 0;
    for (int i = 0; i < conns; i++) { ok += w[i].ok; fail += w[i].fail; }

    printf("\n===== 压测结果 =====\n");
    printf("连接数: %d，每连接请求数: %d\n", conns, reqs);
    printf("成功: %d，失败: %d\n", ok, fail);
    if (elapsed > 0)
        printf("总耗时: %.3f 秒，吞吐: %.0f req/s\n", elapsed, (ok + fail) / elapsed);

    for (int i = 0; i < conns; i++)
        if (w[i].sock >= 0) close(w[i].sock);
    free(w);
    free(t);
    return 0;
}
