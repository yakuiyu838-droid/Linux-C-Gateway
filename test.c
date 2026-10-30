#include <stdio.h>
#include <time.h>

int main() 
{
    double temp = 28.53;
    double hum = 65.08;
    char reply[256];
    
    // 测试 1：时间戳
    int ts = (int)time(NULL);
    printf("时间戳是: %d\n", ts);
    
    // 测试 2：打包 JSON
    sprintf(reply, "{\"temp\": %.1f, \"hum\": %.1f}\n", temp, hum);
    printf("打包后的字符串是: %s", reply);
    
    return 0;
}