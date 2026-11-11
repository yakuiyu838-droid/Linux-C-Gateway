#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

// 定义一个任务结构体（纸条）
typedef struct Task 
{
    void (*function)(void *arg); // 函数指针：要干什么活
    void *arg;                   // 参数：干活需要的工具
    struct Task *next;           // 链表指针：指向下一个任务
} Task;

// 线程池结构体
typedef struct ThreadPool 
{
    Task *head;                  // 任务队列的头
    Task *tail;                  // 任务队列的尾
    int thread_count;            // 打工仔（线程）数量
    pthread_t *threads;          // 存放线程 ID 的数组
    pthread_mutex_t mutex;       // 互斥锁：防止大家抢纸条抢打架
    pthread_cond_t cond;         // 条件变量：没活干时，让打工仔睡觉
    int stop;                    // 线程池关闭标志
} ThreadPool;

// 核心接口
ThreadPool* threadpool_create(int thread_count);
void threadpool_add_task(ThreadPool *pool, void (*func)(void *), void *arg);
// cleanup: 销毁时对「队列里还没跑完的任务」的参数做释放（如传 free）；传 NULL 则不释放
void threadpool_destroy(ThreadPool *pool, void (*cleanup)(void *arg));

#endif