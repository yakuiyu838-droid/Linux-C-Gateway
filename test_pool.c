#include <stdio.h>
#include <unistd.h>
#include "threadpool.h"

#include <stdlib.h>

// 模拟你的存库/查库操作
void my_task(void *arg) 
{
    int num = *(int *)arg;
    printf(">>> 打工仔正在处理任务: %d\n", num);
    sleep(1); // 故意睡1秒，模拟查库慢
    free(arg);
}

int main() 
{
    ThreadPool *pool = threadpool_create(3); // 雇 3 个打工仔
    printf("线程池创建成功，有3个打工仔待命\n");

    for (int i = 0; i < 10; i++) 
    {
        int *arg = (int *)malloc(sizeof(int));
        *arg = i;
        // 主线程只负责扔任务，扔完就跑！
        threadpool_add_task(pool, my_task, arg);
    }

    sleep(5); // 等他们干完活
    threadpool_destroy(pool);
    printf("主程序结束\n");
    return 0;
}