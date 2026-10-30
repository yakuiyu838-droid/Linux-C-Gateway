#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "db.h"
#include "serial.h"


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
    
    //设置为非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return listen_fd;
}

int main() 
{
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

    // 塞串口进 epoll
    ev.events = EPOLLIN;
    ev.data.fd = serial_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serial_fd, &ev);

    // 塞网络进 epoll
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("=== 网关终极版启动 ===\n");

    while (1) 
    {
        // 1. 核心阻塞点！
        // 参数说明：epfd（事件表），events（用来接收就绪事件的数组），10（最多一次取10个），-1（死等，没事件就睡大觉）。
        // 返回值 nfds：本次有几个文件描述符（fd）有动静了。
        int nfds = epoll_wait(epfd, events, 10, -1);
        
        // 2. 遍历所有有动静的 fd
        for (int i = 0; i < nfds; i++) 
        {
            // 取出当前有动静的那个 fd
            int fd = events[i].data.fd;

            // ==========================================================
            // 【分支 A】：如果是串口有动静（底层硬件发数据来了）
            // ==========================================================
            if (fd == serial_fd) 
            { 
                char buf[256];
                // 注意：因为串口设置了 O_NONBLOCK，所以就算此时没数据，read 也不会卡死，而是立刻返回 -1。
                // 但在这里，因为 epoll 通知了，说明肯定有数据，所以直接读。
                int len = serial_read(buf, sizeof(buf));
                
                if (len > 0) 
                {
                    printf("【串口采集】: %s", buf);
                    double temp, hum;
                    int ts = (int)time(NULL); // 获取当前系统时间戳
                    
                    // 用 sscanf 把 "28.5,65.0" 这种字符串，精准剥离成两个 double 变量
                    if (sscanf(buf, "%lf,%lf", &temp, &hum) == 2) 
                    {
                        // 存入数据库 (底层调用 sqlite3_prepare -> bind -> step)
                        db_insert_sensor(temp, hum, ts);
                        printf("  -> 已存入数据库\n");
                    }
                }
            }
            
            // ==========================================================
            // 【分支 B】：如果是网络监听 fd 有动静（有新的客户端连进来了）
            // ==========================================================
            else if (fd == listen_fd) 
            { 
                // 因为 listen_fd 也是非阻塞的，如果 accept 失败不会卡死。
                int c_fd = accept(listen_fd, NULL, NULL);
                if (c_fd > 0) 
                {
                    printf("【网络】客户端接入 fd=%d\n", c_fd);
                    
                    // 🚨 极其关键的一步：新接入的客户端 fd，必须注册进 epoll！
                    // 如果不加这句，客户端发消息过来 epoll 根本不会理睬。
                    ev.events = EPOLLIN;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }
            
            // ==========================================================
            // 【分支 C】：如果是已经连上的客户端发消息了（或者断开了）
            // ==========================================================
            else 
            { 
                char buf[256];
                int len = recv(fd, buf, sizeof(buf) - 1, 0);
                
                if (len > 0) 
                {
                    // 客户端发来的数据通常带有回车(\r\n)，用 strcspn 找到它的位置并替换为 \0，去掉换行符。
                    buf[strcspn(buf, "\r\n")] = '\0'; 
                    printf("【网络请求】: 收到指令 '%s'\n", buf);
                    
                    double temp, hum; int ts;
                    char reply[512];
                    
                    // 去数据库查最新的一条数据
                    if (db_get_latest(&temp, &hum, &ts) == 0) 
                    {
                        // 查到了，用 sprintf 拼装成标准的 JSON 格式字符串
                        sprintf(reply, "{\"temp\": %.1f, \"hum\": %.1f}\n", temp, hum);
                    }
                    else 
                    {
                        sprintf(reply, "{\"error\": \"no data\"}\n");
                    }
                    // 将 JSON 字符串通过 socket 发送给客户端
                    send(fd, reply, strlen(reply), 0);
                } 
                // 🚨 核心坑点：recv 返回 <= 0，代表客户端主动断开连接。
                // 如果不清理，epoll 会陷入无限死循环(一直报告断开)，且会导致 fd 泄漏。
                else if (len <= 0) 
                {
                    // 1. 先从 epoll 监控树上摘除
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    // 2. 关闭文件描述符，释放系统资源
                    close(fd);
                    printf("【网络】客户端 fd=%d 断开\n", fd); // 建议自己加上这句
                }
            }
        }
    }
    return 0;
}