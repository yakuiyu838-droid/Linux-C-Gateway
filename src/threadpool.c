#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <stdlib.h>

// 打工仔（Worker）的工作循环
void *worker_routine(void *arg) 
{
    ThreadPool *pool = (ThreadPool *)arg;
    while (1) 
    {
        pthread_mutex_lock(&pool->mutex); // 进休息室，先锁门
        
        // 如果没任务，且没说要解散，就睡觉等活儿
        while (pool->head == NULL && !pool->stop) 
        {
            pthread_cond_wait(&pool->cond, &pool->mutex); 
            // 注意：wait 会暂时解锁并休眠，被叫醒时会重新加锁
        }
        
        if (pool->stop) 
        { // 如果老板说要解散
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        // 取出一个任务（从队列头拿）
        Task *task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL) 
        {
            pool->tail = NULL; // 队列空了
        }
        pthread_mutex_unlock(&pool->mutex); // 拿到纸条了，出门解锁

        // 执行任务！
        task->function(task->arg);
        free(task); // 纸条用完扔了
    }
    return NULL;
}

// 创建线程池
ThreadPool* threadpool_create(int thread_count) 
{
    ThreadPool *pool = (ThreadPool *)malloc(sizeof(ThreadPool));
    pool->head = pool->tail = NULL;
    pool->thread_count = thread_count;
    pool->stop = 0;
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < thread_count; i++) 
    {
        pthread_create(&pool->threads[i], NULL, worker_routine, pool);
    }
    return pool;
}

// 添加任务（主线程调用的）
void threadpool_add_task(ThreadPool *pool, void (*func)(void *), void *arg) 
{
    Task *task = (Task *)malloc(sizeof(Task));
    task->function = func;
    task->arg = arg;
    task->next = NULL;

    pthread_mutex_lock(&pool->mutex);
    // 把任务加进队列尾巴
    if (pool->tail == NULL) 
    {
        pool->head = pool->tail = task;
    } 
    else 
    {
        pool->tail->next = task;
        pool->tail = task;
    }
    // 摇铃铛，叫醒一个睡觉的打工仔
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

// 销毁线程池
void threadpool_destroy(ThreadPool *pool) 
{
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond); // 叫醒所有打工仔下班
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) 
    {
        pthread_join(pool->threads[i], NULL); // 等他们干完最后的活
    }
    
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);
}