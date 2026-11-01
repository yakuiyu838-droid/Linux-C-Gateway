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

#include "db.h"
#include "serial.h"
#include "threadpool.h"

// 把 fd 设为非阻塞：ET 模式下必须，否则 read/recv 在没数据时会阻塞整个事件循环
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 初始化网络
int init_server(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//端口复用

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);

    // 设置为非阻塞（ET 模式必须先非阻塞）
    if (set_nonblocking(listen_fd) != 0) {
        perror("set_nonblocking");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// 定义一个结构体，用来打包后台任务需要的参数
typedef struct {
    int client_fd;
    char query[256];
} QueryTaskArgs;

// 这是真正在后台（Worker线程）执行的函数
void background_db_query(void *arg)
{
    QueryTaskArgs *args = (QueryTaskArgs *)arg;

    double temp, hum;
    int ts;
    char reply[512];

    // 此时是在子线程里查库，绝不会卡住主线程！
    if (db_get_latest(&temp, &hum, &ts) == 0)
    {
        sprintf(reply, "{\"temp\": %.1f, \"hum\": %.1f}\n", temp, hum);
    }
    else
    {
        sprintf(reply, "{\"error\": \"no data\"}\n");
    }

    // 查完了，直接通过 socket 发给客户端
    send(args->client_fd, reply, strlen(reply), 0);

    free(args); // 用完释放
}

int main()
{
    // 忽略 SIGPIPE：否则对已断开的客户端 send 会直接杀死整个进程
    signal(SIGPIPE, SIG_IGN);

    // ⚠️ 注意这里：以后代码里永远写这个软链接名字！
    if (serial_init("/tmp/gw_tty_read") != 0)
    {
        return -1;
    }
    if (db_init("gateway.db") != 0)
    {
        return -1;
    }

    int listen_fd = init_server(8888);
    int epfd = epoll_create(1);
    struct epoll_event ev, events[10];

    // 塞串口进 epoll（ET 边沿触发）
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = serial_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serial_fd, &ev);

    // 塞网络监听 fd 进 epoll
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("=== 网关终极版启动 ===\n");

    // 创建线程池
    ThreadPool *pool = threadpool_create(3);

    while (1)
    {
        // 1. 核心阻塞点！epoll_wait 参数：epfd（事件表），events（接收就绪事件数组），
        //    10（最多一次取10个），-1（死等）。返回值 nfds：本次有几个 fd 有动静。
        int nfds = epoll_wait(epfd, events, 10, -1);
        if (nfds < 0) {
            // 被信号打断（比如 Ctrl+C），暂未实现优雅退出，先继续等
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        // 2. 遍历所有有动静的 fd
        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // ==========================================================
            // 【分支 A】：串口有动静（传感器发数据来了）
            // ==========================================================
            if (fd == serial_fd)
            {
                // ET 模式：必须循环读到 EAGAIN，否则一次就绪事件只读到一部分，
                // 剩下的数据就丢了
                for (;;)
                {
                    char buf[256];
                    int len = serial_read(buf, sizeof(buf));
                    if (len < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读完了
                        perror("串口读出错");
                        break;
                    }
                    if (len == 0) break;

                    printf("【串口采集】: %s", buf);
                    double temp, hum;
                    int ts = (int)time(NULL); // 获取当前系统时间戳

                    // 用 sscanf 把 "28.5,65.0" 这种字符串精准剥离成两个 double
                    if (sscanf(buf, "%lf,%lf", &temp, &hum) == 2)
                    {
                        // 存入数据库 (底层调用 sqlite3_prepare -> bind -> step)
                        db_insert_sensor(temp, hum, ts);
                        printf("  -> 已存入数据库\n");
                    }
                }
            }

            // ==========================================================
            // 【分支 B】：网络监听 fd 有动静（新客户端连进来）
            // ==========================================================
            else if (fd == listen_fd)
            {
                // ET 模式：可能同时有多个连接，循环 accept 直到 EAGAIN
                for (;;)
                {
                    int c_fd = accept(listen_fd, NULL, NULL);
                    if (c_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 没有新连接了
                        perror("accept");
                        break;
                    }

                    // ⚠️ 关键：客户端 fd 必须设非阻塞，否则 ET 模式下 recv 会卡死事件循环
                    set_nonblocking(c_fd);

                    printf("【网络】客户端接入 fd=%d\n", c_fd);

                    // 🚨 极其关键：新接入的客户端 fd 必须注册进 epoll，否则它发消息 epoll 不理
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }

            // ==========================================================
            // 【分支 C】：已连接的客户端发消息 / 断开
            // ==========================================================
            else
            {
                // ET 模式：循环 recv 直到 EAGAIN，把缓冲区一次读干净
                for (;;)
                {
                    char buf[256];
                    int len = recv(fd, buf, sizeof(buf) - 1, 0);

                    if (len > 0)
                    {
                        buf[len] = '\0';
                        // 客户端发的数据通常带 \r\n，用 strcspn 找到并替换成 \0 去掉换行
                        buf[strcspn(buf, "\r\n")] = '\0';
                        printf("【网络请求】: 收到指令 '%s'\n", buf);

                        // ⚠️ 核心改造：不自己查库，而是打包参数丢进线程池！
                        QueryTaskArgs *args = (QueryTaskArgs *)malloc(sizeof(QueryTaskArgs));
                        if (args != NULL) {
                            args->client_fd = fd;
                            strcpy(args->query, buf);
                            // 任务扔进线程池，主线程立刻回去处理下一个 epoll 事件
                            threadpool_add_task(pool, background_db_query, args);
                        }
                    }
                    // len == 0：客户端正常关闭（close/fin）
                    else if (len == 0)
                    {
                        // 1. 从 epoll 监控树上摘除
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        // 2. 关闭 fd，释放资源
                        close(fd);
                        printf("【网络】客户端 fd=%d 断开\n", fd);
                        break;
                    }
                    // len < 0：要区分「暂时没数据」和「真出错」
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 数据读完，正常
                        if (errno == EINTR) continue;                       // 被信号打断，继续读
                        // 其他错误：真出错，当断开处理
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        printf("【网络】客户端 fd=%d 出错断开\n", fd);
                        break;
                    }
                }
            }
        }
    }

    // 清理（等以后做了优雅退出，这里才会真正走到）
    threadpool_destroy(pool);
    db_close();
    serial_close();
    return 0;
}
