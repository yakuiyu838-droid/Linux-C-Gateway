#include "control.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

// 模拟「设备文件」所在的目录。真实硬件时把这里换成 /sys/class/gpio/gpioN/
// （写它的 value 文件 1/0 就是拉高拉低电平），接口不用改。
#define CONTROL_DIR "/tmp"

// 白名单：本项目只允许控制这几个设备。真实场景里每个设备对应一路 GPIO/继电器。
static const char *g_known_devices[] = { "relay1", "relay2", "led1" };
#define KNOWN_DEVICE_COUNT (sizeof(g_known_devices) / sizeof(g_known_devices[0]))

int control_set(const char *device, int on_off)
{
    // 1. 设备名校验：必须在白名单里，防止客户端发任意路径写坏系统文件（路径穿越）
    int known = 0;
    for (int i = 0; i < (int)KNOWN_DEVICE_COUNT; i++)
    {
        if (strcmp(device, g_known_devices[i]) == 0)
        {
            known = 1;
            break;
        }
    }
    if (!known)
    {
        fprintf(stderr, "[控制] 未知设备: %s\n", device);
        return -1;
    }

    // 2. 打开/创建对应设备文件（O_TRUNC 保证只保留最新状态）
    char path[256];
    snprintf(path, sizeof(path), "%s/gateway-%s.state", CONTROL_DIR, device);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("[控制] 打开设备文件失败");
        return -1;
    }

    // 3. 写 1/0（模拟往 GPIO value 文件写高低电平）
    const char *val = on_off ? "1\n" : "0\n";
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    if (n < 0)
    {
        perror("[控制] 写设备文件失败");
        return -1;
    }

    printf("[控制] %s -> %s（已写 %s）\n", device, on_off ? "开/ON" : "关/OFF", path);
    return 0;
}
