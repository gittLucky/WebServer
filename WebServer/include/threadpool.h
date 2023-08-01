#ifndef __THREADPOOL_H_
#define __THREADPOOL_H_
#include <pthread.h>
#include <functional>
#include <memory>
#include <vector>

// 错误类型
const int THREADPOOL_INVALID = -1;
const int THREADPOOL_LOCK_FAILURE = -2;
const int THREADPOOL_SHUTDOWN = -3;
const int THREADPOOL_THREAD_FAILURE = -4;

struct ThreadPoolTask
{
    std::function<void(std::shared_ptr<void>)> fun;   // functional代替函数指针
    std::shared_ptr<void> args;  // functional的参数
};

// 任务处理函数
void myHandler(std::shared_ptr<void> req);

/* 描述线程池相关信息 */
class ThreadPool
{
private:
    static pthread_mutex_t lock;           /* 用于锁住本结构体 */
    static pthread_mutex_t thread_counter; /* 记录忙状态线程个数de琐 -- busy_thr_num */
    static pthread_cond_t queue_not_full;  /* 当任务队列满时，添加任务的线程阻塞，等待此条件变量 */
    static pthread_cond_t queue_not_empty; /* 任务队列里不为空时，通知等待任务的线程 */

    static std::vector<pthread_t> threads;  /* 存放线程池中每个线程的tid数组 */
    static pthread_t adjust_tid; /* 存管理线程tid */

    static int min_thr_num;       /* 线程池最小线程数 */
    static int max_thr_num;       /* 线程池最大线程数 */
    static int live_thr_num;      /* 当前存活线程个数 */
    static int busy_thr_num;      /* 忙状态线程个数 */
    static int wait_exit_thr_num; /* 要销毁的线程个数 */

    static std::vector<ThreadPoolTask> queue;  /* 任务队列 */
    static int queue_front;               /* task_queue队头下标 */
    static int queue_rear;                /* task_queue队尾下标 */
    static int queue_size;                /* task_queue队中实际任务数 */
    static int queue_max_size;            /* task_queue队列可容纳任务数上限 */

    static int shutdown; /* 标志位，线程池使用状态，true或false */
public:
    static int threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size);
    static int threadpool_add(std::shared_ptr<void> args, std::function<void(std::shared_ptr<void>)> fun = myHandler);
    static int threadpool_destroy();
    static int threadpool_free();
    static int threadpool_all_threadnum();
    static int threadpool_busy_threadnum();
    static void *threadpool_thread(void *threadpool);
    static void *adjust_thread(void *threadpool);
    static int is_thread_alive(pthread_t tid);
};

#endif
