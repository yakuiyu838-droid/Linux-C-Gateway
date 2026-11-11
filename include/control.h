#ifndef CONTROL_H
#define CONTROL_H

// 控制设备（继电器 / LED / GPIO），往「模拟设备文件」写 1/0
// device: 设备名（如 "relay1"）；on_off: 1 开 / 0 关
// 返回 0 成功，-1 失败（未知设备或写文件失败）
int control_set(const char *device, int on_off);

#endif
