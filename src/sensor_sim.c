// 传感器模拟器：独立进程，周期往虚拟串口写 "温度,湿度" 数据
// 用法：./sensor_sim <设备路径> [间隔秒数]
// 例：  ./sensor_sim /tmp/gw_tty_write 2    每 2 秒发一帧
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>

int main(int argc, char *argv[])
{
    const char *dev      = argc > 1 ? argv[1] : "/tmp/gw_tty_write";
    int         interval = argc > 2 ? atoi(argv[2]) : 2;

    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("模拟器打开串口失败");
        return -1;
    }

    // 和网关端保持一致，配成 115200 8N1 raw（对 pty 波特率是摆设，真串口才起效）
    struct termios opt;
    tcgetattr(fd, &opt);
    cfsetispeed(&opt, B115200);
    cfsetospeed(&opt, B115200);
    opt.c_cflag &= ~PARENB;
    opt.c_cflag &= ~CSTOPB;
    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= CS8;
    opt.c_cflag |= (CLOCAL | CREAD);
    opt.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    opt.c_oflag &= ~OPOST;   // 关输出处理，保证发的就是 '\n' 不变成 '\r\n'
    tcsetattr(fd, TCSANOW, &opt);

    srand((unsigned)time(NULL));

    printf("传感器模拟器启动：设备 %s，每 %d 秒发一帧 (Ctrl+C 退出)\n", dev, interval);

    for (int i = 1; ; i++) 
    {
        // 模拟温度 20.0~34.9、湿度 40.0~79.9 的随机漂移
        double temp = 20.0 + (rand() % 1500) / 100.0;
        double hum  = 40.0 + (rand() % 4000) / 100.0;

        char frame[64];
        int  n = snprintf(frame, sizeof(frame), "%.1f,%.1f\n", temp, hum);
        write(fd, frame, n);
        printf("[第 %d 帧] 已发送: %s", i, frame);

        sleep(interval);
    }

    close(fd);
    return 0;
}
