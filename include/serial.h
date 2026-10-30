#ifndef SERIAL_H
#define SERIAL_H

// 告诉外面，有个全局串口变量;
extern int serial_fd;
// 初始化串口，传入设备路径，比如 "/dev/pts/2"
int serial_init(const char *device_path);
// 从串口读取数据，存入 buffer
int serial_read(char *buffer, int max_len);
// 关闭串口
void serial_close();

#endif
