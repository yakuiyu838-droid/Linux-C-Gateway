#include <stdio.h>
#include "cJSON.h"    // 引入 cJSON
#include "sqlite3.h"  // 引入 SQLite

int main() 
{
    printf("Start building Gateway!\n");
    
    // 测试 cJSON
    printf("cJSON 版本: %s\n", cJSON_Version());
    
    // 测试 SQLite
    printf("SQLite 版本: %s\n", sqlite3_libversion());
    
    return 0;
}