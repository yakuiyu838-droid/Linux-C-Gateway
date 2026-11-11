#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

// 打工仔（Worker）的工作循环
void *worker_routine(void *arg)
{
    ThreadPool *pool = (ThreadPool *) arg;
    while (1)
    {
        // 进休息室，先锁门
        pthread_mutex_lock(&pool->mutex);
        // 如果没任务，且没说要解散，就睡觉等活儿
        while (pool->head == NULL && !pool->stop)
        {
            pthread_cond_wait(&pool->cond, &pool->mutex);
            // 注意：wait 会暂时解锁并休眠，被叫醒时会重新加锁
        }
        // 如果老板说要解散
        if (pool->stop)
        {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        // 取出一个任务（从队列头拿）
        Task *task = pool->head;
        pool->head = task->next;   // 头指针后移，指向下一个任务
        // 队列空了，尾指针也要跟着清空
        if (pool->head == NULL)
        {
            pool->tail = NULL;
        }
        // 拿到纸条了，出门解锁
        pthread_mutex_unlock(&pool->mutex);
        // 执行任务！
        task->function(task->arg);
        // 纸条用完扔了
        free(task);
    }
    return NULL;
}

// 创建线程池
ThreadPool* threadpool_create(int thread_count)
{
    // 1. 租个办公室（申请结构体内存）
    ThreadPool *pool = (ThreadPool *)malloc(sizeof(ThreadPool));
    if (pool == NULL) return NULL;
    // 2. 办公室里放好桌子：头指针、尾指针、员工数量、停止标志
    pool->head = pool->tail = NULL;
    pool->thread_count = thread_count;
    pool->stop = 0;
    // 3. 买好门锁和传呼机（初始化 mutex 和 cond）
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    // 3.5 给「员工名单」（线程 ID 数组）分配内存
    //     threads 是指针，必须先 malloc 才能用，否则 pthread_create 会往野地址写
    pool->threads = (pthread_t *)malloc(thread_count * sizeof(pthread_t));
    if (pool->threads == NULL)
    {
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->cond);
        free(pool);
        return NULL;
    }
    // 4. 正式招聘：循环创建 thread_count 个线程，让他们去跑 worker_routine
    for (int i = 0; i < thread_count; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_routine, pool) != 0)
        {
            // 创建失败：让已创建的线程退出、join 回收，再释放资源，返回 NULL
            pool->stop = 1;
            pthread_cond_broadcast(&pool->cond);
            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->cond);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

// 添加任务（主线程调用的）
void threadpool_add_task(ThreadPool *pool, void (*func)(void *), void *arg)
{
    // 1. 写一张任务纸条（申请任务节点内存）
    Task *task = (Task *)malloc(sizeof(Task));
    if (task == NULL) return;   // 内存不足，丢弃任务，避免后续解引用 NULL 崩溃
    task->function = func;
    task->arg = arg;
    task->next = NULL;

    // 2. 锁门！防止打工仔此时来抢
    pthread_mutex_lock(&pool->mutex);

    // 3. 把纸条贴到队列的最后面（经典的链表尾插法）
    if (pool->tail == NULL)
    {
        pool->head = pool->tail = task; // 如果队列是空的，头和尾都是它
    }
    else
    {
        pool->tail->next = task;        // 否则挂在尾巴后面
        pool->tail = task;              // 更新尾巴指针
    }

    // 4. 敲一下传呼机（叫醒一个睡觉的打工仔）
    pthread_cond_signal(&pool->cond);

    // 5. 开门
    pthread_mutex_unlock(&pool->mutex);
}

// 销毁线程池
void threadpool_destroy(ThreadPool *pool, void (*cleanup)(void *arg))
{
    // 如果公司（pool）根本不存在，那就别瞎折腾了，直接退出。
    if (!pool) return;

    // 1. 锁门，贴出告示：公司破产了（stop = 1）
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    // 2. 广播：别睡了！所有人赶紧起来下班！
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    // 3. 等所有打工仔收拾好东西离开公司（join 回收线程资源）
    for (int i = 0; i < pool->thread_count; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // 3.5 worker 退出时如果队列里还有没处理完的任务（纸条），这里清掉，
    //     并调用 cleanup 释放任务参数，避免优雅退出时内存泄漏
    Task *t = pool->head;
    while (t)
    {
        Task *next = t->next;
        if (cleanup) cleanup(t->arg);
        free(t);
        t = next;
    }

    // 4. 销毁门锁、传呼机，退租办公室（释放所有内存）
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
}
