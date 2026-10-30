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
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    return listen_fd;
}

int main() 
{
    // ⚠️ 注意这里：以后代码里永远写这个软链接名字！
    if (serial_init("/tmp/gw_tty_read") != 0) return -1; 
    if (db_init("gateway.db") != 0) return -1;
    
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
        int nfds = epoll_wait(epfd, events, 10, -1);
        for (int i = 0; i < nfds; i++) 
        {
            int fd = events[i].data.fd;

            if (fd == serial_fd) 
            { // 串口
                char buf[256];
                int len = serial_read(buf, sizeof(buf));
                if (len > 0) 
                {
                    printf("【串口采集】: %s", buf);
                    double temp, hum;
                    int ts = (int)time(NULL);
                    if (sscanf(buf, "%lf,%lf", &temp, &hum) == 2) 
                    {
                        db_insert_sensor(temp, hum, ts);
                        printf("  -> 已存入数据库\n");
                    }
                }
            }
            else if (fd == listen_fd) 
            { // 网络新连接
                int c_fd = accept(listen_fd, NULL, NULL);
                if (c_fd > 0) 
                {
                    printf("【网络】客户端接入 fd=%d\n", c_fd);
                    ev.events = EPOLLIN;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }
            else 
            { // 网络发消息
                char buf[256];
                int len = recv(fd, buf, sizeof(buf) - 1, 0);
                if (len > 0) 
                {
                    buf[strcspn(buf, "\r\n")] = '\0'; // 去换行符
                    printf("【网络请求】: 收到指令 '%s'\n", buf);
                    double temp, hum; int ts;
                    char reply[512];
                    if (db_get_latest(&temp, &hum, &ts) == 0) 
                    {
                        sprintf(reply, "{\"temp\": %.1f, \"hum\": %.1f}\n", temp, hum);
                    } else 
                    {
                        sprintf(reply, "{\"error\": \"no data\"}\n");
                    }
                    send(fd, reply, strlen(reply), 0);
                } 
                else if (len <= 0) 
                {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }
    return 0;
}