#ifndef __THREADPOOL_H_
#define __THREADPOOL_H_
#include <pthread.h>

const int THREADPOOL_INVALID = -1;
const int THREADPOOL_LOCK_FAILURE = -2;
const int THREADPOOL_SHUTDOWN = -3;
const int THREADPOOL_THREAD_FAILURE = -4;

typedef struct
{
    void (*function)(void *); /* 函数指针，任务回调函数 */
    void *arg;                 /* 上面回调函数的参数 */
} threadpool_task_t;           /* 各子线程任务结构体 */

/* 描述线程池相关信息 */
struct threadpool_t
{
    pthread_mutex_t lock;           /* 用于锁住本结构体 */
    pthread_mutex_t thread_counter; /* 记录忙状态线程个数de琐 -- busy_thr_num */
    pthread_cond_t queue_not_full;  /* 当任务队列满时，添加任务的线程阻塞，等待此条件变量 */
    pthread_cond_t queue_not_empty; /* 任务队列里不为空时，通知等待任务的线程 */

    pthread_t *threads;   /* 存放线程池中每个线程的tid数组 */
    pthread_t adjust_tid; /* 存管理线程tid */

    int min_thr_num;       /* 线程池最小线程数 */
    int max_thr_num;       /* 线程池最大线程数 */
    int live_thr_num;      /* 当前存活线程个数 */
    int busy_thr_num;      /* 忙状态线程个数 */
    int wait_exit_thr_num; /* 要销毁的线程个数 */

    threadpool_task_t *task_queue; /* 任务队列 */
    int queue_front;               /* task_queue队头下标 */
    int queue_rear;                /* task_queue队尾下标 */
    int queue_size;                /* task_queue队中实际任务数 */
    int queue_max_size;            /* task_queue队列可容纳任务数上限 */

    int shutdown; /* 标志位，线程池使用状态，true或false */
};

typedef struct threadpool_t threadpool_t;

/**
 * @function threadpool_create
 * @descCreates a threadpool_t object.
 * @param thr_num  thread num
 * @param max_thr_num  max thread size
 * @param queue_max_size   size of the queue.
 * @return a newly created thread pool or NULL
 */
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size);

/**
 * @function threadpool_add
 * @desc add a new task in the queue of a thread pool
 * @param pool     Thread pool to which add the task.
 * @param function Pointer to the function that will perform the task.
 * @param argument Argument to be passed to the function.
 * @return 0 if all goes well,else -1
 */
int threadpool_add(threadpool_t *pool, void (*function)(void *arg), void *arg);

/**
 * @function threadpool_destroy
 * @desc Stops and destroys a thread pool.
 * @param pool  Thread pool to destroy.
 * @return 0 if destory success else -1
 */
int threadpool_destroy(threadpool_t *pool);

// 线程池的释放
int threadpool_free(threadpool_t *pool);

/**
 * @desc 返回线程池中所有活着的线程
 * @pool 线程池
 * @return 线程数
 */
int threadpool_all_threadnum(threadpool_t *pool);

/**
 * desc 返回线程池中忙的线程
 * @param 线程池
 * return 忙的线程数
 */
int threadpool_busy_threadnum(threadpool_t *pool);

/**
 * @function void *threadpool_thread(void *threadpool)
 * @desc 工作线程
 * @param 传参线程池本身
 */
void *threadpool_thread(void *threadpool);

/**
 * @function void *adjust_thread(void *threadpool);
 * @desc 管理线程
 * @param 传参线程池本身
 */
void *adjust_thread(void *threadpool);

/**
 * 检查线程是否活着
 */
int is_thread_alive(pthread_t tid);

#endif
