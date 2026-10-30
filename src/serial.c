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
    tcgetattr(serial_fd, &options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tcsetattr(serial_fd, TCSANOW, &options);

    printf("串口 %s 初始化成功 (非阻塞)\n", device_path);
    return 0;
}

int serial_read(char *buffer, int max_len) 
{
    if (serial_fd < 0) return -1;
    int len = read(serial_fd, buffer, max_len - 1);
    if (len > 0) buffer[len] = '\0';
    return len;
}

void serial_close() 
{
    if (serial_fd >= 0) close(serial_fd);
}