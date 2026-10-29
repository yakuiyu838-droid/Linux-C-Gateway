#include "serial.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>

static int serial_fd = -1;

int serial_init(const char *device_path) 
{
    // 1. 打开串口设备（O_RDWR 读写模式，O_NOCTTY 告诉系统这不是控制终端）
    serial_fd = open(device_path, O_RDWR | O_NOCTTY | O_NDELAY);
    if (serial_fd < 0) 
    {
        perror("打开串口失败");
        return -1;
    }

    // 2. 获取当前串口属性
    struct termios options;
    tcgetattr(serial_fd, &options);

    // 3. 设置波特率（115200）
    cfsetispeed(&options, B115200);//设置输入波特率
    cfsetospeed(&options, B115200);//设置输出波特率

    // 4. 配置 8N1 模式（8个数据位，无校验，1个停止位）
    options.c_cflag &= ~PARENB; // 无校验  &= ~清除配置
    options.c_cflag &= ~CSTOPB; // 1位停止位  |=设置配置
    options.c_cflag &= ~CSIZE;  // 清除数据位设置
    options.c_cflag |= CS8;     // 8位数据位
    options.c_cflag |= (CLOCAL | CREAD); // 忽略调制解调器控制线，启用接收器

    // 5. 设置为原始模式（Raw Mode），不做任何特殊字符处理
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // 6. 应用设置
    tcsetattr(serial_fd, TCSANOW, &options);
    
    printf("串口 %s 初始化成功\n", device_path);
    return 0;
}

int serial_read(char *buffer, int max_len) 
{
    if (serial_fd < 0) return -1;
    // read 会阻塞等待，直到串口有数据过来
    int len = read(serial_fd, buffer, max_len - 1);
    if (len > 0) 
    {
        buffer[len] = '\0'; // 添加字符串结束符
    }
    return len;
}

void serial_close() 
{
    if (serial_fd >= 0) close(serial_fd);
}