#include "threadpool.h"
#include "HttpRequestData.h"
#include "_cmpublic.h"

#define DEFAULT_TIME 10        /*10s检测一次*/
#define MIN_WAIT_TASK_NUM 10   /*如果queue_size > MIN_WAIT_TASK_NUM 添加新的线程到线程池*/
#define DEFAULT_THREAD_VARY 10 /*每次创建和销毁线程的个数*/

/* 初始化互斥琐、条件变量 */
pthread_mutex_t ThreadPool::lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ThreadPool::thread_counter = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ThreadPool::queue_not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t ThreadPool::queue_not_empty = PTHREAD_COND_INITIALIZER;

std::vector<pthread_t> ThreadPool::threads;
pthread_t ThreadPool::adjust_tid = 0;
std::vector<ThreadPoolTask> ThreadPool::queue;

int ThreadPool::min_thr_num = 0;
int ThreadPool::max_thr_num = 0;
int ThreadPool::live_thr_num = 0;
int ThreadPool::busy_thr_num = 0;
int ThreadPool::wait_exit_thr_num = 0;

int ThreadPool::queue_front = 0;
int ThreadPool::queue_rear = 0;
int ThreadPool::queue_size = 0;
int ThreadPool::queue_max_size = 0;

int ThreadPool::shutdown = 0;  /* 不关闭线程池 */

// 线程池的创建
int ThreadPool::threadpool_create(int _min_thr_num, int _max_thr_num, int _queue_max_size)
{
    int i;
    do
    {
        min_thr_num = _min_thr_num;
        max_thr_num = _max_thr_num;
        live_thr_num = _min_thr_num; /* 活着的线程数 初值=最小线程数 */
        queue_max_size = _queue_max_size;

        /* 根据最大线程上限数， 给工作线程数组开辟空间, 并清零 */
        threads.resize(_max_thr_num);

        /* 队列开辟空间 */
        queue.resize(_queue_max_size);

        // 设置线程为分离状态
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        /* 启动 min_thr_num 个 work thread */
        for (i = 0; i < min_thr_num; i++)
        {
            if (pthread_create(&threads[i], &attr, threadpool_thread, (void *)(0)) != 0) /*pool指向当前线程池*/
            {
                // threadpool_destroy(pool);
                return -1;
            }
            // printf("start thread 0x%x...\n", (unsigned int)pool->threads[i]);
        }
        if (pthread_create(&adjust_tid, &attr, adjust_thread, (void *)(0)) != 0) /* 启动管理者线程 */
        {
            // threadpool_destroy(pool);
            return -1;
        }
    } while (0);

    // threadpool_free(pool); /* 前面代码调用失败时，释放poll存储空间 */

    return 0;
}

void myHandler(std::shared_ptr<void> req)
{
    std::shared_ptr<RequestData> request = std::static_pointer_cast<RequestData>(req);
    request->handleRequest();
}

/* 向线程池中 添加一个任务，args传给function */
int ThreadPool::threadpool_add(std::shared_ptr<void> args, std::function<void(std::shared_ptr<void>)> fun)
{
    int err = 0;
    if (pthread_mutex_lock(&lock) != 0)
    {
        return THREADPOOL_LOCK_FAILURE;
    }

    /* ==为真，队列已经满， 调wait阻塞 */
    while ((queue_size == queue_max_size) && !shutdown)
    {
        if (pthread_cond_wait(&queue_not_full, &lock) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
    }
    if (shutdown)
    {
        if (pthread_mutex_unlock(&lock) != 0)
        {
            return THREADPOOL_LOCK_FAILURE;
        }
        return THREADPOOL_SHUTDOWN;
    }

    // /* 清空工作线程调用的回调函数 的参数arg */
    // if (pool->task_queue[pool->queue_rear].arg != NULL)
    // {
    //     free(pool->task_queue[pool->queue_rear].arg);
    //     pool->task_queue[pool->queue_rear].arg = NULL;
    // }
    /*添加任务到任务队列里*/
    queue[queue_rear].fun = fun;
    queue[queue_rear].args = args;
    queue_rear = (queue_rear + 1) % queue_max_size; /* 队尾指针移动, 模拟环形 */
    queue_size++;

    /*添加完任务后，队列不为空，唤醒线程池中 等待处理任务的线程*/
    if ((pthread_cond_broadcast(&queue_not_empty) != 0) || (pthread_mutex_unlock(&lock) != 0))
    {
        err = THREADPOOL_LOCK_FAILURE;
    }
    return err;
}

/* 线程池中各个工作线程 */
void *ThreadPool::threadpool_thread(void *args)
{
    int err = 0;

    while (true)
    {
        ThreadPoolTask task;
        /* Lock must be taken to wait on conditional variable */
        /*刚创建出线程，等待任务队列里有任务，否则阻塞等待任务队列里有任务后再唤醒接收任务*/
        if (pthread_mutex_lock(&lock) != 0)
        {
            break;
        }

        /*queue_size == 0 说明没有任务，调 wait 阻塞在条件变量上, 若有任务，跳过该while*/
        while ((queue_size == 0) && !shutdown)
        {
            // printf("thread 0x%x is waiting\n", (unsigned int)pthread_self());
            if (pthread_cond_wait(&queue_not_empty, &lock) != 0)
            {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }

            // 阻塞的线程为空闲的线程
            /*清除指定数目的空闲线程，如果要结束的线程个数大于0，结束线程*/
            if (wait_exit_thr_num > 0)
            {
                /*如果线程池里线程个数大于最小值时可以结束当前线程*/
                if (live_thr_num > min_thr_num)
                {
                    wait_exit_thr_num--;
                    // printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
                    int k = 0;
                    for (k = 0; k < max_thr_num; k++)
                    {
                        if (pthread_self() == threads[k])
                        {
                            break;
                        }
                    }
                    memset(&threads[k], 0x00, sizeof(pthread_t));
                    live_thr_num--;
                    if (pthread_mutex_unlock(&lock) != 0)
                    {
                        err = THREADPOOL_LOCK_FAILURE;
                        break;
                    }
                    pthread_exit(NULL);
                }
            }
        }
        if (err != 0)
        {
            break;
        }
        /*如果指定了true，要关闭线程池里的每个线程，自行退出处理*/
        if (shutdown)
        {
            if (pthread_mutex_unlock(&lock) != 0)
            {
                break;
            }
            // printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
            pthread_exit(NULL); /* 线程自行结束 */
        }

        /*从任务队列里获取任务, 是一个出队操作*/
        task.fun = queue[queue_front].fun;
        task.args = queue[queue_front].args;

        queue_front = (queue_front + 1) % queue_max_size; /* 出队，模拟环形队列 */
        queue_size--;

        /*通知可以有新的任务添加进来*/
        /*任务取出后，立即将 线程池琐 释放*/
        if ((pthread_cond_broadcast(&queue_not_full) != 0) || (pthread_mutex_unlock(&lock) != 0))
        {
            break;
        }

        /*执行任务*/
        // printf("thread 0x%x start working\n", (unsigned int)pthread_self());
        if (pthread_mutex_lock(&thread_counter) != 0) /*忙状态线程数变量琐*/
        {
            break;
        }
        busy_thr_num++; /*忙状态线程数+1*/
        if (pthread_mutex_unlock(&thread_counter) != 0)
        {
            break;
        }
        (task.fun)(task.args); /*执行回调函数任务*/

        /*任务结束处理*/
        // printf("thread 0x%x end working\n", (unsigned int)pthread_self());
        if (pthread_mutex_lock(&thread_counter) != 0)
        {
            break;
        }
        busy_thr_num--; /*处理掉一个任务，忙状态数线程数-1*/
        if (pthread_mutex_unlock(&thread_counter) != 0)
        {
            break;
        }
    }

    pthread_exit(NULL);
}

/* 管理线程 */
void *ThreadPool::adjust_thread(void *args)
{
    int i, err = 0;
    while (!shutdown)
    {

        sleep(DEFAULT_TIME); /*定时 对线程池管理*/

        if (pthread_mutex_lock(&lock) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
        int _queue_size = queue_size;     /* 关注 任务数 */
        int _live_thr_num = live_thr_num; /* 存活 线程数 */
        if (pthread_mutex_unlock(&lock) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }

        if (pthread_mutex_lock(&thread_counter) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
        int _busy_thr_num = busy_thr_num; /* 忙着的线程数 */
        if (pthread_mutex_unlock(&thread_counter) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }

        /* 创建新线程 算法： 当前任务数大于最小任务数, 且存活的线程数少于最大线程个数时 如：30>=10 && 40<100*/
        if (_queue_size >= MIN_WAIT_TASK_NUM && _live_thr_num < max_thr_num)
        {
            if (pthread_mutex_lock(&lock) != 0)
            {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }
            int add = 0;

            /*一次增加 DEFAULT_THREAD_VARY个线程*/
            // i < pool->max_thr_num防止越界
            // 设置线程为分离状态
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            for (i = 0; i < max_thr_num && add < DEFAULT_THREAD_VARY && live_thr_num < max_thr_num; i++)
            {
                if (threads[i] == 0 || !is_thread_alive(threads[i]))
                {
                    if (pthread_create(&threads[i], &attr, threadpool_thread, (void *)(0)) != 0)
                    {
                        // threadpool_destroy(pool);
                        return NULL;
                    }
                    add++;
                    live_thr_num++;
                }
            }

            if (pthread_mutex_unlock(&lock) != 0)
            {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }
        }

        /* 销毁多余的空闲线程 算法：忙线程X2 小于 存活的线程数 且 存活的线程数 大于 最小线程数时*/
        if ((_busy_thr_num * 2) < _live_thr_num && _live_thr_num > min_thr_num)
        {

            /* 一次销毁DEFAULT_THREAD_VARY个线程 */
            if (pthread_mutex_lock(&lock) != 0)
            {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }
            wait_exit_thr_num = DEFAULT_THREAD_VARY; /* 要销毁的线程数 设置为10 */
            if (pthread_mutex_unlock(&lock) != 0)
            {
                err = THREADPOOL_LOCK_FAILURE;
                break;
            }

            for (i = 0; i < DEFAULT_THREAD_VARY; i++)
            {
                /* 通知处在空闲状态的线程, 他们会自行终止*/
                pthread_cond_signal(&queue_not_empty);
            }
        }
    }

    pthread_exit(NULL);
}

// 线程池的销毁
int ThreadPool::threadpool_destroy()
{
    printf("Thread pool destroy !\n");
    int i, err = 0;
    if (pthread_mutex_lock(&lock) != 0)
    {
        return THREADPOOL_LOCK_FAILURE;
    }
    do
    {
        /* Already shutting down */
        if (shutdown)
        {
            err = THREADPOOL_SHUTDOWN;
            break;
        }
        shutdown = true;
        if (pthread_mutex_unlock(&lock) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
        /*先销毁管理线程*/
        // 等待管理线程结束
        pthread_join(adjust_tid, NULL);
        /*通知所有的空闲线程*/
        // 通知消费者
        if (pthread_cond_broadcast(&queue_not_empty) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
        // 等待所有消费者全部退出
        for (i = 0; i < max_thr_num; i++)
        {
            if (pthread_join(threads[i], NULL) != 0)
            {
                err = THREADPOOL_THREAD_FAILURE;
            }
        }
    } while (false);

    if (!err)
    {
        threadpool_free();
    }

    return err;
}

int ThreadPool::threadpool_free()
{
    if (threads.size())
    {
        threads.clear();
    }
    if (queue.size())
    {
        queue.clear();
    }
    pthread_mutex_destroy(&lock);
    pthread_mutex_destroy(&thread_counter);
    pthread_cond_destroy(&queue_not_empty);
    pthread_cond_destroy(&queue_not_full);

    return 0;
}

// 返回线程池中所有活着的线程
int ThreadPool::threadpool_all_threadnum()
{
    int all_threadnum = -1;
    pthread_mutex_lock(&lock);
    all_threadnum = live_thr_num;
    pthread_mutex_unlock(&lock);
    return all_threadnum;
}

// 返回线程池中忙的线程
int ThreadPool::threadpool_busy_threadnum()
{
    int busy_threadnum = -1;
    pthread_mutex_lock(&thread_counter);
    busy_threadnum = busy_thr_num;
    pthread_mutex_unlock(&thread_counter);
    return busy_threadnum;
}

// 判断线程是否活着
int ThreadPool::is_thread_alive(pthread_t tid)
{
    int kill_rc = pthread_kill(tid, 0); // 发0号信号，测试线程是否存活
    // 不存在返回特定的错误码ESRCH
    if (kill_rc == ESRCH)
    {
        return false;
    }

    return true;
}

/*测试*/
#if 0
/* 线程池中的线程，模拟处理业务 */
void process(void *arg)
{
    printf("thread 0x%x working on task %d\n ", (unsigned int)pthread_self(), *(int *)arg);
    sleep(1);
    printf("task %d is end\n", *(int *)arg);
}
int main(void)
{
    threadpool_t *thp = threadpool_create(3, 100, 100); /*创建线程池，池里最小3个线程，最大100，队列最大100*/
    printf("pool inited");

    for (int i = 0; i < 20; i++)
    {
        int *ptr = (int *)malloc(sizeof(int));
        *ptr = i;
        printf("add task %d\n", i);
        threadpool_add(thp, process, (void *)ptr); /* 向线程池中添加任务 */
    }
    sleep(10); /* 等子线程完成任务 */
    threadpool_destroy(thp);

    return 0;
}

#endif