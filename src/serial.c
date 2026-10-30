#include "serial.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>

int serial_fd = -1; // 注意：这里去掉了 static

int serial_init(const char *device_path) 
{
    serial_fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) 
    {
        perror("打开串口失败");
        return -1;
    }

    struct termios options;
    tcgetattr(serial_fd, &options);      //获取当前默认配置。相当于拿出一张出厂设置表
    cfsetispeed(&options, B115200);      //设置波特率
    cfsetospeed(&options, B115200);
    options.c_cflag &= ~PARENB; // 关闭校验位 (Parity Enable = 0)
    options.c_cflag &= ~CSTOPB; // 关闭两位停止位 (Stop Bit = 0，默认就是1位)
    options.c_cflag &= ~CSIZE;  // 先清除原有的数据位设置
    options.c_cflag |= CS8;     // 设置为 8 位数据位 (Character Size = 8)
    options.c_cflag |= (CLOCAL | CREAD); 
    // CLOCAL: 忽略调制解调器控制线
    // CREAD: 启用接收器 (如果不加，串口只发不收)
    options.c_lflag &= ~(ICANON | ECHO | ECHOE |ISIG);  // 设为 Raw 模式（极其重要：关掉规范模式，来啥收啥，不加工）
   

    printf("串口 %s 初始化成功 (非阻塞)\n", device_path);
    return 0;
}

int serial_read(char *buffer, int max_len) 
{
    if (serial_fd < 0)
    {
        return -1;
    } 
    int len = read(serial_fd, buffer, max_len - 1);
    if (len > 0) 
    {
        buffer[len] = '\0';
        return len;
    }
}

void serial_close() 
{
    if (serial_fd >= 0) 
    {
        close(serial_fd);
    }
}