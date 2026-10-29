#include <stdio.h>
#include <time.h>
#include "db.h"
#include "serial.h"

int main() {
   // 只要 echo 发给 6，你就得监听 5（或者反过来）
    if (serial_init("/dev/pts/7") != 0) return -1;
    if (db_init("gateway.db") != 0) return -1;
    
    printf("网关启动，开始监听串口数据...\n");
    char buf[1024];

    while (1) {
        // 阻塞等待数据
        int len = serial_read(buf, sizeof(buf));
        if (len > 0) {
            printf("【串口收到】: %s", buf);
            
            // 为了简单测试，我们假设收到的数据格式就是纯数字："26.5,55.0"
            double temp, hum;
            int ts = (int)time(NULL);
            if (sscanf(buf, "%lf,%lf", &temp, &hum) == 2) {
                if (db_insert_sensor(temp, hum, ts) == 0) {
                    printf(" -> 【存入数据库】成功 (%.1f, %.1f)\n", temp, hum);
                }
            } else {
                printf(" -> 格式错误\n");
            }
        }
    }

    serial_close();
    db_close();
    return 0;
}