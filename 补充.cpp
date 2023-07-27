main.cpp

#include "HttpRequestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"
#include "_cmpublic.h"
#include "log.h"

using namespace std;

static const int MAXEVENTS = 5000;
// const int MAXEVENTS = 10;
// 监听最大描述符
static const int LISTENQ = 1024;

// 定义线程池最大线程数与最小线程数，任务队列最大任务数
const int THREADPOOL_MAX_THREAD_NUM = 100;
const int THREADPOOL_MIN_THREAD_NUM = 4;
const int QUEUE_MAX_SIZE = 65535;
// const int QUEUE_MAX_SIZE = 100;

const string PATH = "/";

// 服务器使用的端口
const int PORT = 8888;

// 计时器超时时间(毫秒)
const int TIMER_TIME_OUT = 500;

extern CLogFile logfile;  // 服务程序的运行日志


// extern pthread_mutex_t qlock;
// extern pthread_mutex_t log_lock;
extern struct epoll_event* events;
void acceptConnection(int listen_fd, int epoll_fd, const string &path);

extern priority_queue<mytimer*, deque<mytimer*>, timerCmp> myTimerQueue;

// 初始化监听描述符
int socket_bind_listen(int port)
{
    // 检查port值，取正确区间范围
    if (port < 1024 || port > 65535)
        return -1;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // 消除bind时"Address already in use"错误
    int optval = 1;
    if(setsockopt(listen_fd, SOL_SOCKET,  SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // 设置服务器IP和Port，和监听描述符绑定
    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if(bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        close(listen_fd);
        return -1;
    }

    // 开始监听，最大等待队列长为LISTENQ
    if(listen(listen_fd, LISTENQ) == -1)
    {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// 任务处理函数
void myHandler(void *args)
{
    requestData *req_data = (requestData*)args;
    req_data->handleRequest();
}

// 监听描述符，接受新连接
void acceptConnection(int listen_fd, int epoll_fd, const string &path)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    // 此处使用while循环是解决边沿触发问题，多个连接请求同时到达，epoll_wait只会通知一次，导致有的连接没有响应
    while((accept_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len)) > 0)
    {
        /*
        // TCP的保活机制默认是关闭的
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */
        // 记录连接日志
        char *str = inet_ntoa(client_addr.sin_addr);
        // pthread_mutex_lock(&log_lock);
        {
            MutexLockGuard_LOG();
            logfile.Write("客户端(%s)已连接。\n", str);
            // 设为非阻塞模式
            int ret = setnonblocking(accept_fd);
            if (ret < 0) {
                logfile.Write("Set accept non block failed!\n");
                return;
            }
        }
        // pthread_mutex_unlock(&log_lock);

        requestData *req_info = new requestData(epoll_fd, accept_fd, string(str), path);

        // 文件描述符可以读，边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        epoll_add(epoll_fd, accept_fd, static_cast<void*>(req_info), _epo_event);
        // 新增时间信息，为每一个新的连接添加一个过期时间
        mytimer *mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info->addTimer(mtimer);
        // pthread_mutex_lock(&qlock);
        MutexLockGuard();
        myTimerQueue.push(mtimer);
        // pthread_mutex_unlock(&qlock);
    }
}
// 分发处理函数
void handle_events(int epoll_fd, int listen_fd, struct epoll_event* events, int events_num, const string &path, threadpool_t* tp)
{
    for(int i = 0; i < events_num; i++)
    {
        // 获取有事件产生的描述符
        requestData* request = (requestData*)(events[i].data.ptr);
        int fd = request->getFd();

        // 有事件发生的描述符为监听描述符
        if(fd == listen_fd)
        {
            //cout << "This is listen_fd" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        }
        else
        {
            // 排除错误事件
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                || (!(events[i].events & EPOLLIN)))
            {
                printf("error event\n");
                delete request;
                continue;
            }

            // 将请求任务加入到线程池中
            // 加入线程池之前将Timer和request分离
            request->seperateTimer();
            int rc = threadpool_add(tp, myHandler, events[i].data.ptr);
        }
    }
}

/* 处理逻辑是这样的~
因为(1) 优先队列不支持随机访问
(2) 即使支持，随机删除某节点后破坏了堆的结构，需要重新更新堆结构。
所以对于被置为deleted的时间节点，会延迟到它(1)超时 或 (2)它前面的节点都被删除时，它才会被删除。
一个点被置为deleted,它最迟会在TIMER_TIME_OUT时间后被删除。
这样做有两个好处：
(1) 第一个好处是不需要遍历优先队列，省时。
(2) 第二个好处是给超时时间一个容忍的时间，就是设定的超时时间是删除的下限(并不是一到超时时间就立即删除)，如果监听的请求在超时后的下一次请求中又一次出现了，
就不用再重新申请requestData节点了，这样可以继续重复利用前面的requestData，减少了一次delete和一次new的时间。
*/


/*
 此函数用来计时，当加入epoll红黑树上边的结点超时时候会进行剔除，每次有事件发生时就要进行分离，
 处理完事件后重新进行计时；
 所以计时器计时的不是事件处理  超时，而是加到epoll里长时间滞留，没有事件发生
*/
void handle_expired_event()
{
    // pthread_mutex_lock(&qlock);
    MutexLockGuard();
    while (!myTimerQueue.empty())
    {
        mytimer *ptimer_now = myTimerQueue.top();
        if (ptimer_now->isDeleted())
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        else if (ptimer_now->isvalid() == false)
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        else
        {
            break;
        }
    }
    // pthread_mutex_unlock(&qlock);
}

int main()
{
    handle_for_sigpipe();
    // 打开日志文件
    if (logfile.Open("/home/student-4/wh/vscode-workspace/logfile.log", "a+") == false) {
        printf("logfile.Open(%s) failed.\n", "/home/student-4/wh/vscode-workspace/logfile.log");
        return -1;
    }
    if (Epoll::epoll_init(MAXEVENTS, LISTENQ) < 0)
    {
        logfile.Write("epoll init failed.\n");
        return 1;
    }
    if(ThreadPool::threadpool_create(THREADPOOL_MIN_THREAD_NUM, THREADPOOL_MAX_THREAD_NUM, QUEUE_MAX_SIZE) < 0)
    {
        logfile.Write("threadpool create failed\n");
        return 1;
    }
    int listen_fd = socket_bind_listen(PORT);
    if (listen_fd < 0)
    {
        logfile.Write("socket bind failed\n");
        return 1;
    }
    if (setnonblocking(listen_fd) < 0)
    {
        logfile.Write("set listen socket non block failed\n");
        return 1;
    }
    __uint32_t event = EPOLLIN | EPOLLET;
    shared_ptr<requestData> request(new requestData());
    request->setFd(listen_fd);·
    int ret = Epoll::epoll_add(epoll_fd, listen_fd, static_cast<void*>(req), event);
    if (ret < 0) {
        logfile.Write("epoll add failed\n");
        return 1;
    }
    while (true)
    {
        if (Epoll::my_epoll_wait(listen_fd, MAXEVENTS, -1) < 0) {
            MutexLockGuard_LOG();
            // pthread_mutex_lock(&log_lock);
            logfile.Write("epoll wait failed\n");
            // pthread_mutex_unlock(&log_lock);
            return 1;
        }
        handle_expired_event();
    }
    return 0;
}

epoll.cpp

#include "epoll.h"
#include "_cmpublic.h"

int TIMER_TIME_OUT = 500;
extern std::priority_queue<std::shared_ptr<mytimer>, std::deque<std::shared_ptr<mytimer>>, timerCmp> myTimerQueue;

epoll_event *Epoll::events;
std::unordered_map<int, std::shared_ptr<requestData>> Epoll::fd2req;
int Epoll::epoll_fd = 0;
const std::string Epoll::PATH = "/";

// 注册新描述符
int epoll_add(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;
    //printf("add to epoll %d\n", fd);
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
    {
        // perror("epoll_add error");
        return -1;
    }
    return 0;
}

// 注册新描述符
int Epoll::epoll_add(int fd, std::shared_ptr<requestData> request, __uint32_t events)
{
    struct epoll_event event;
    event.data.fd = fd;
    event.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
    {
        // perror("epoll_add error");
        return -1;
    }
    fd2req[fd] = request;
    return 0;
}

// 修改描述符状态
int epoll_mod(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0)
    {
        // perror("epoll_mod error");
        return -1;
    } 
    return 0;
}

// 从epoll中删除描述符
int epoll_del(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event) < 0)
    {
        // perror("epoll_del error");
        return -1;
    } 
    return 0;
}

// 返回活跃事件数
int my_epoll_wait(int epoll_fd, struct epoll_event* events, int max_events, int timeout)
{
    int ret_count = epoll_wait(epoll_fd, events, max_events, timeout);
    if (ret_count < 0)
    {
        // perror("epoll wait error");
        return -1;
    }
    return ret_count;
}

// 返回活跃事件数
int Epoll::my_epoll_wait(int listen_fd, int max_events, int timeout)
{
    // printf("fd2req.size()==%d\n", fd2req.size());
    int event_count = epoll_wait(epoll_fd, events, max_events, timeout);
    if (event_count < 0)
        // perror("epoll wait error");
        return -1;
    std::vector<std::shared_ptr<requestData>> req_data = getEventsRequest(listen_fd, event_count, PATH);
    if (req_data.size() > 0)
    {
        for (auto &req: req_data)
        {
            if (ThreadPool::threadpool_add(req) < 0)
            {
                // 线程池满了或者关闭了等原因，抛弃本次监听到的请求。
                break;
            }
        }
    }
    return 0;
}

// 初始化epoll
int Epoll::epoll_init(int maxevents, int listen_num)
{
    epoll_fd = epoll_create(listen_num + 1);
    if(epoll_fd == -1)
        return -1;
    //events.reset(new epoll_event[maxevents], [](epoll_event *data){delete [] data;});
    events = new epoll_event[maxevents];
    return 0;
}

// 监听描述符，接受新连接
void Epoll::acceptConnection(int listen_fd, int epoll_fd, const std::string path)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    // 此处使用while循环是解决边沿触发问题，多个连接请求同时到达，epoll_wait只会通知一次，导致有的连接没有响应
    while((accept_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len)) > 0)
    {
        
        cout << inet_addr(client_addr.sin_addr.s_addr) << endl;
        cout << client_addr.sin_port << endl;
        /*
        // TCP的保活机制默认是关闭的
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */
        
        // 记录连接日志
        char *str = inet_ntoa(client_addr.sin_addr);
        // pthread_mutex_lock(&log_lock);
        {
            MutexLockGuard_LOG();
            logfile.Write("客户端(%s)已连接。\n", str);
            // 设为非阻塞模式
            int ret = setnonblocking(accept_fd);
            if (ret < 0) {
                logfile.Write("Set accept non block failed!\n");
                return;
            }
        }
        // pthread_mutex_unlock(&log_lock);

        std::shared_ptr<requestData> req_info(new requestData(epoll_fd, accept_fd, string(str), path));

        // 文件描述符可以读，边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        Epoll::epoll_add(accept_fd, req_info, _epo_event);
        // 新增时间信息，为每一个新的连接添加一个过期时间
        std::shared_ptr<mytimer> mtimer(new mytimer(req_info, TIMER_TIME_OUT));
        req_info->addTimer(mtimer);
        MutexLockGuard lock;
        myTimerQueue.push(mtimer);
    }
}

// 分发处理函数
std::vector<std::shared_ptr<requestData>> Epoll::getEventsRequest(int listen_fd, int events_num, const std::string path)
{
    std::vector<std::shared_ptr<requestData>> req_data;
    for(int i = 0; i < events_num; ++i)
    {
        // 获取有事件产生的描述符
        int fd = events[i].data.fd;

        // 有事件发生的描述符为监听描述符
        if(fd == listen_fd)
        {
            //cout << "This is listen_fd" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        }
        // 排除标准输入、输出、标准错误输出
        else if (fd < 3)
        {
            break;
        }
        else
        {
            // 排除错误事件
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                || (!(events[i].events & EPOLLIN)))
            {
                //printf("error event\n");
                auto fd_ite = fd2req.find(fd);
                if (fd_ite != fd2req.end())
                    fd2req.erase(fd_ite);
                //printf("fd = %d, here\n", fd);
                continue;
                ?????????????????????????????????下树
            }

            // 将请求任务加入到线程池中
            // 加入线程池之前将Timer和request分离
            std::shared_ptr<requestData> cur_req(fd2req[fd]);
            //printf("cur_req.use_count=%d\n", cur_req.use_count());
            cur_req->seperateTimer();
            req_data.push_back(cur_req);
            ?????????????????
            auto fd_ite = fd2req.find(fd);
            if (fd_ite != fd2req.end())
                fd2req.erase(fd_ite);
        }
    }
    return req_data;
}

threadpool.cpp


#include "threadpool.h"
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
    std::shared_ptr<requestData> request = std::static_pointer_cast<requestData>(req);
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
        (task.function)(task.args); /*执行回调函数任务*/

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


threadpool.h


#ifndef __THREADPOOL_H_
#define __THREADPOOL_H_
#include <pthread.h>
#include <functional>
#include <memory>
#include <vector>

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


epoll.h

#ifndef EVENTPOLL
#define EVENTPOLL
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <memory>


// const int MAXEVENTS = 5000;
// // const int MAXEVENTS = 10;
// // 监听最大描述符
// const int LISTENQ = 1024;

class Epoll
{
private:
    // epoll返回事件
    static epoll_event *events;
    static std::unordered_map<int, std::shared_ptr<requestData>> fd2req;
    static int epoll_fd;
    static const std::string PATH;
public:
    static int epoll_init(int maxevents, int listen_num);
    static int epoll_add(int fd, std::shared_ptr<requestData> request, __uint32_t events);
    static int epoll_mod(int fd, std::shared_ptr<requestData> request, __uint32_t events);
    static int epoll_del(int fd, __uint32_t events);
    static int my_epoll_wait(int listen_fd, int max_events, int timeout);
    static void acceptConnection(int listen_fd, int epoll_fd, const std::string path);
    static std::vector<std::shared_ptr<requestData>> getEventsRequest(int listen_fd, int events_num, const std::string path);
};

// int epoll_init();
// int epoll_add(int epoll_fd, int fd, void *request, __uint32_t events);
// int epoll_mod(int epoll_fd, int fd, void *request, __uint32_t events);
// int epoll_del(int epoll_fd, int fd, void *request, __uint32_t events);
// int my_epoll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout);

#endif


HttpRequestData.cpp

#include "HttpRequestData.h"
#include "util.h"
#include "epoll.h"
#include "_cmpublic.h"
#include "log.h"

// #include <opencv/cv.h>
// #include <opencv2/core/core.hpp>
// #include <opencv2/highgui/highgui.hpp>
// #include <opencv2/opencv.hpp>
// using namespace cv;

// test
using namespace std;

// pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t MutexLockGuard::lock = PTHREAD_MUTEX_INITIALIZER;

CLogFile logfile;

// 静态成员类外初始化
pthread_mutex_t MimeType::lock = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<std::string, std::string> MimeType::mime;

// 定义请求的格式
std::string MimeType::getMime(const std::string &suffix)
{
    if (mime.size() == 0)
    {
        pthread_mutex_lock(&lock);
        if (mime.size() == 0)
        {
            mime[".html"] = "text/html";
            mime[".avi"] = "video/x-msvideo";
            mime[".bmp"] = "image/bmp";
            mime[".c"] = "text/plain";
            mime[".doc"] = "application/msword";
            mime[".gif"] = "image/gif";
            mime[".gz"] = "application/x-gzip";
            mime[".htm"] = "text/html";
            mime[".ico"] = "application/x-ico";
            mime[".jpg"] = "image/jpeg";
            mime[".png"] = "image/png";
            mime[".txt"] = "text/plain";
            mime[".mp3"] = "audio/mp3";
            mime["default"] = "text/html";
        }
        pthread_mutex_unlock(&lock);
    }
    if (mime.find(suffix) == mime.end())
        return mime["default"];
    else
        return mime[suffix];
}

// 保存过期时间列表为小根堆
priority_queue<shared_ptr<mytimer>, std::deque<shared_ptr<mytimer>>, timerCmp> myTimerQueue;

// 监听描述符构造函数
requestData::requestData():
    now_read_pos(0), 
    state(STATE_PARSE_URI), 
    h_state(h_start),
    keep_alive(true), 
    againTimes(0)
{}

// 连接描述符构造函数
requestData::requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path): 
    now_read_pos(0), 
    state(STATE_PARSE_URI),
    h_state(h_start),
    keep_alive(true), 
    againTimes(0), 
    path(_path),
    fd(_fd), 
    IP(addr_IP), 
    epollfd(_epollfd)
{}

// 析构函数
requestData::~requestData()
{
    // cout << "~requestData()" << endl;
    // struct epoll_event ev;
    // // 超时的一定都是读请求，没有"被动"写。
    // ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    // ev.data.ptr = (void *)this;
    // epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    // if (timer != NULL)
    // {
    //     timer->clearReq();
    //     timer = NULL;
    // }
    close(fd);
}

// 为requestData添加计时
void requestData::addTimer(shared_ptr<mytimer> mtimer)
{
    // if (timer == NULL)
    timer = mtimer;
}

// 获取fd
int requestData::getFd()
{
    return fd;
}

// 设置fd
void requestData::setFd(int _fd)
{
    fd = _fd;
}

// 重置requestData
void requestData::reset()
{
    againTimes = 0;
    content.clear();
    file_name.clear();
    path.clear();
    now_read_pos = 0;
    state = STATE_PARSE_URI;
    h_state = h_start;
    headers.clear();
    keep_alive = true;
    // weak_ptr：use_count()返回与weak_ptr共享的shared_ptr数量
    // expired()若use_count()为0返回true，对象已死
    // lock()若expired()为true返回一个空的shared_ptr，否则返回一个指向对象的shared_ptr
    if (timer.lock())
    {
        shared_ptr<mytimer> my_timer(timer.lock());
        my_timer->clearReq();
        ????????????
        timer = NULL;
    }
}

void requestData::seperateTimer()
{
    if (timer.lock())
    {
        shared_ptr<mytimer> my_timer(timer.lock());
        my_timer->clearReq();
        timer = NULL;
    }
}

// 事件处理函数
void requestData::handleRequest()
{
    char buff[MAX_BUFF];
    bool isError = false;

    // 此处循环保证边沿触发一次性读取完，continue来保证读取完
    while (true)
    {
        int read_num = readn(fd, buff, MAX_BUFF);
        if (read_num < 0)
        {
            // perror("1");
            isError = true;
            break;
        }
        else if (read_num == 0)
        {
            // 非阻塞模式第一次没有读到，或者对端连接已断开会返回0
            // 有请求出现但是读不到数据，可能是Request Aborted，或者来自网络的数据没有达到等原因
            perror("read_num == 0");
            // 非阻塞模式第一次没有读到
            if (errno == EAGAIN)
            {
                if (againTimes > AGAIN_MAX_TIMES)
                    isError = true;
                else
                    ++againTimes;
            }
            else if (errno != 0)
                isError = true;
            break;
        }
        string now_read(buff, buff + read_num);
        content += now_read;

        if (state == STATE_PARSE_URI)
        {
            int flag = this->parse_URI();
            if (flag == PARSE_URI_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_URI_ERROR)
            {
                // perror("2");
                isError = true;
                break;
            }
        }
        if (state == STATE_PARSE_HEADERS)
        {
            int flag = this->parse_Headers();
            if (flag == PARSE_HEADER_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_HEADER_ERROR)
            {
                // perror("3");
                isError = true;
                break;
            }
            // 一般POST请求在空行后带请求数据；GET请求不带请求数据，携带在URI中
            if (method == METHOD_POST)
            {
                state = STATE_RECV_BODY;
            }
            else
            {
                state = STATE_ANALYSIS;
            }
        }
        // POST请求
        if (state == STATE_RECV_BODY)
        {
            int content_length = -1;
            if (headers.find("Content-length") != headers.end())
            {
                content_length = stoi(headers["Content-length"]);
            }
            else
            {
                isError = true;
                break;
            }
            // 数据部分本次未读完
            if (content.size() < content_length)
                continue;
            state = STATE_ANALYSIS;
        }
        if (state == STATE_ANALYSIS)
        {
            int flag = this->analysisRequest();
            if (flag < 0)
            {
                isError = true;
                break;
            }
            else if (flag == ANALYSIS_SUCCESS)
            {

                state = STATE_FINISH;
                break;
            }
            else
            {
                isError = true;
                break;
            }
        }
    }

    if (isError)
    {
        pthread_mutex_lock(&log_lock);
        logfile.Write("客户端(%s)HTTP解析错误!\n", IP.c_str());
        pthread_mutex_unlock(&log_lock);
        delete this;
        return;
    }
    // 加入epoll继续
    if (state == STATE_FINISH)
    {
        if (keep_alive)
        {
            // printf("ok\n");
            pthread_mutex_lock(&log_lock);
            logfile.Write("客户端(%s)HTTP解析成功!\n", IP.c_str());
            pthread_mutex_unlock(&log_lock);
            this->reset();
        }
        else
        {
            delete this;
            return;
        }
    }
    // 一定要先加时间信息，否则可能会出现刚加进去，下个in触发来了，然后分离失败后，又加入队列，最后超时被删，
    // 然后正在线程中进行的任务出错，double free错误

    // 新增时间信息
    // pthread_mutex_lock(&qlock);
    mytimer *mtimer = new mytimer(this, EPOLL_WAIT_TIME);
    timer = mtimer;
    {
        MutexLockGuard();
        myTimerQueue.push(mtimer);
    }
    // pthread_mutex_unlock(&qlock);

    __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
    int ret = epoll_mod(epollfd, fd, static_cast<void *>(this), _epo_event);
    if (ret < 0)
    {
        // 返回错误处理
        pthread_mutex_lock(&log_lock);
        logfile.Write("epoll mod failed\n");
        pthread_mutex_unlock(&log_lock);
        delete this;
        return;
    }
}

// 解析URI：确定属性filename,HTTPversion,method
int requestData::parse_URI()
{
    // str引用content，改变str就是改变content
    string &str = content;
    // 读到完整的请求行再开始解析请求
    int pos = str.find('\r', now_read_pos);
    // 没有找到说明此次读取请求头包含不完全
    if (pos < 0)
    {
        return PARSE_URI_AGAIN;
    }
    // 去掉请求行所占的空间，节省空间
    string request_line = str.substr(0, pos);
    if (str.size() > pos + 1)
        str = str.substr(pos + 1);
    else
        str.clear();
    // Method
    pos = request_line.find("GET");
    if (pos < 0)
    {
        pos = request_line.find("POST");
        if (pos < 0)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            method = METHOD_POST;
        }
    }
    else
    {
        method = METHOD_GET;
    }
    // printf("method = %d\n", method);
    //  filename
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        int _pos = request_line.find(' ', pos);
        if (_pos < 0)
            return PARSE_URI_ERROR;
        else
        {
            if (_pos - pos > 1)
            {
                file_name = request_line.substr(pos, _pos - pos);
                int __pos = file_name.find('?');
                if (__pos >= 0)
                {
                    file_name = file_name.substr(0, __pos);
                }
            }

            else
                file_name = "../doc/index.html";
        }
        pos = _pos;
    }
    // cout << "file_name: ----------" << file_name << endl;
    //  HTTP 版本号
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        if (request_line.size() - pos <= 3)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            string ver = request_line.substr(pos + 1, 3);
            if (ver == "1.0")
                HTTPversion = HTTP_10;
            else if (ver == "1.1")
                HTTPversion = HTTP_11;
            else
                return PARSE_URI_ERROR;
        }
    }
    state = STATE_PARSE_HEADERS;
    return PARSE_URI_SUCCESS;
}

// 解析请求头
int requestData::parse_Headers()
{
    string &str = content;
    int key_start = -1, key_end = -1, value_start = -1, value_end = -1;
    int now_read_line_begin = 0;
    bool notFinish = true;
    for (int i = 0; i < str.size() && notFinish; ++i)
    {
        switch (h_state)
        {
        case h_start:
        {
            if (str[i] == '\n' || str[i] == '\r')
                break;
            h_state = h_key;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        case h_key:
        {
            if (str[i] == ':')
            {
                key_end = i;
                if (key_end - key_start <= 0)
                    return PARSE_HEADER_ERROR;
                h_state = h_colon;
            }
            else if (str[i] == '\n' || str[i] == '\r')
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_colon:
        {
            if (str[i] == ' ')
            {
                h_state = h_spaces_after_colon;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_spaces_after_colon:
        {
            h_state = h_value;
            value_start = i;
            break;
        }
        case h_value:
        {
            if (str[i] == '\r')
            {
                h_state = h_CR;
                value_end = i;
                if (value_end - value_start <= 0)
                    return PARSE_HEADER_ERROR;
            }
            else if (i - value_start > 255)
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_LF;
                string key(str.begin() + key_start, str.begin() + key_end);
                string value(str.begin() + value_start, str.begin() + value_end);
                headers[key] = value;
                now_read_line_begin = i;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_LF:
        {
            if (str[i] == '\r')
            {
                h_state = h_end_CR;
            }
            else
            {
                key_start = i;
                h_state = h_key;
            }
            break;
        }
        case h_end_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_end_LF;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        // 要么后面还有请求体，要么刚好读完空行\n
        case h_end_LF:
        {
            notFinish = false;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        }
    }
    if (h_state == h_end_LF)
    {
        str = str.substr(now_read_line_begin);
        return PARSE_HEADER_SUCCESS;
    }
    // 没读完头
    str = str.substr(now_read_line_begin);
    return PARSE_HEADER_AGAIN;
}

// HTTP响应
int requestData::analysisRequest()
{
    if (method == METHOD_POST)
    {
        // get content
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        // cout << "content=" << content << endl;
        //  test char*
        char *send_content = "I have receiced this.";

        sprintf(header, "%sContent-length: %zu\r\n", header, strlen(send_content));
        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }

        send_len = (size_t)writen(fd, send_content, strlen(send_content));
        if (send_len != strlen(send_content))
        {
            // perror("Send content failed");
            return ANALYSIS_ERROR;
        }
        // cout << "content size ==" << content.size() << endl;
        // 保存发送方数据到vector
        // vector<char> data(content.begin(), content.end());
        // // opencv函数：将vector中内容读到Mat矩阵中
        // Mat test = imdecode(data, CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
        // // 保存到指定的文件receive.bmp
        // imwrite("receive.bmp", test);
        return ANALYSIS_SUCCESS;
    }
    else if (method == METHOD_GET)
    {
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        int dot_pos = file_name.find('.');
        const char *filetype;
        if (dot_pos < 0)
            filetype = MimeType::getMime("default").c_str();
        else
            filetype = MimeType::getMime(file_name.substr(dot_pos)).c_str();
        // 此结构体描述文件的信息
        struct stat sbuf;
        // stat函数获取文件信息保存到sbuf中
        if (stat(file_name.c_str(), &sbuf) < 0)
        {
            handleError(fd, 404, "Not Found!");
            return ANALYSIS_ERROR;
        }

        sprintf(header, "%sContent-type: %s\r\n", header, filetype);
        // 通过Content-length返回文件大小
        sprintf(header, "%sContent-length: %ld\r\n", header, sbuf.st_size);

        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }
        // 打开文件，O_RDONLY只读打开
        int src_fd = open(file_name.c_str(), O_RDONLY, 0);
        // mmap函数类似于read与write，只不过减少了用户态到核心态的拷贝，直接映射到核心态
        // 映射区域起始地址NULL(自动分配)；大小(一般为4KB整数倍)；映射区域自己权限(PROT_READ)可读权限;
        // 映射标志位(MAP_PRIVATE)对映射区的写入操作只反映到缓存区中不会真正写入到文件；文件描述符；偏移量
        // 返回映射起始地址
        char *src_addr = static_cast<char *>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);

        // 发送文件并校验完整性
        send_len = writen(fd, src_addr, sbuf.st_size);
        if (send_len != sbuf.st_size)
        {
            // perror("Send file failed");
            return ANALYSIS_ERROR;
        }
        // 删除映射
        munmap(src_addr, sbuf.st_size);
        return ANALYSIS_SUCCESS;
    }
    else
        return ANALYSIS_ERROR;
}

// 处理文件GET：URI请求错误
void requestData::handleError(int fd, int err_num, string short_msg)
{
    short_msg = " " + short_msg;
    char send_buff[MAX_BUFF];
    string body_buff, header_buff;
    body_buff += "<html><title>TKeed Error</title>";
    body_buff += "<body bgcolor=\"ffffff\">";
    body_buff += to_string(err_num) + short_msg;
    body_buff += "<hr><em> LinYa's Web Server</em>\n</body></html>";

    header_buff += "HTTP/1.1 " + to_string(err_num) + short_msg + "\r\n";
    header_buff += "Content-type: text/html\r\n";
    header_buff += "Connection: close\r\n";
    header_buff += "Content-length: " + to_string(body_buff.size()) + "\r\n";
    header_buff += "\r\n";
    sprintf(send_buff, "%s", header_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
    sprintf(send_buff, "%s", body_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
}

// 为每一个连接添加一个过期时间
mytimer::mytimer(requestData *_request_data, int timeout) : deleted(false), request_data(_request_data)
{
    // cout << "mytimer()" << endl;
    struct timeval now;
    gettimeofday(&now, NULL);
    // 以毫秒计
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

mytimer::~mytimer()
{
    // cout << "~mytimer()" << endl;
    if (request_data != NULL)
    {
        // cout << "request_data=" << request_data << endl;
        delete request_data;
        request_data = NULL;
    }
}

void mytimer::update(int timeout)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

// 是否有效(没有超时)
bool mytimer::isvalid()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    size_t temp = ((now.tv_sec * 1000) + (now.tv_usec / 1000));
    if (temp < expired_time)
    {
        return true;
    }
    else
    {
        this->setDeleted();
        return false;
    }
}

void mytimer::clearReq()
{
    // 给智能指针赋空值
    request_data = NULL;
    this->setDeleted();
}

void mytimer::setDeleted()
{
    deleted = true;
}

// 是否设置为删除
bool mytimer::isDeleted() const
{
    return deleted;
}

size_t mytimer::getExpTime() const
{
    return expired_time;
}

// 优先级队列的比较规则
bool timerCmp::operator()(const mytimer *a, const mytimer *b) const
{
    return a->getExpTime() > b->getExpTime();
}

// 在构造函数中构造锁
MutexLockGuard::MutexLockGuard()
{
    pthread_mutex_lock(&lock);
}

// 在析构函数中释放锁
MutexLockGuard::~MutexLockGuard()
{
    pthread_mutex_unlock(&lock);
}


HttpRequestData.h


#ifndef HTTPREQUESTDATA
#define HTTPREQUESTDATA
#include <string>
#include <unordered_map>
#include <memory>

// URI请求行
const int STATE_PARSE_URI = 1;
// 请求头
const int STATE_PARSE_HEADERS = 2;
// 请求体
const int STATE_RECV_BODY = 3;
// 解析
const int STATE_ANALYSIS = 4;
// 完成
const int STATE_FINISH = 5;

// HTTP读取缓存大小
const int MAX_BUFF = 4096;

// 有请求出现但是读不到数据,可能是Request Aborted,
// 或者来自网络的数据没有达到等原因,
// 对这样的请求尝试超过一定的次数就抛弃
const int AGAIN_MAX_TIMES = 200;

// URI请求行
const int PARSE_URI_AGAIN = -1;
const int PARSE_URI_ERROR = -2;
const int PARSE_URI_SUCCESS = 0;

// 请求头
const int PARSE_HEADER_AGAIN = -1;
const int PARSE_HEADER_ERROR = -2;
const int PARSE_HEADER_SUCCESS = 0;

// 解析
const int ANALYSIS_ERROR = -2;
const int ANALYSIS_SUCCESS = 0;

// 请求方式与HTTP版本
const int METHOD_POST = 1;
const int METHOD_GET = 2;
const int HTTP_10 = 1;
const int HTTP_11 = 2;   // 浏览器发起请求后默认为1.1版本  所以Connection字段会省略

// 计时器过期时间
const int EPOLL_WAIT_TIME = 500;

// 单例模式
class MimeType
{
private:
  static pthread_mutex_t lock;
  static std::unordered_map<std::string, std::string> mime;
  MimeType();
  MimeType(const MimeType &m);

public:
  static std::string getMime(const std::string &suffix);
};

// 请求头格式
enum HeadersState
{
  h_start = 0,  // 开始
  h_key,        // 键key
  h_colon,      // 冒号
  h_spaces_after_colon,  // 冒号后空格
  h_value,   // 值value
  h_CR,     // \r
  h_LF,    // \n
  h_end_CR,   // 空行\r
  h_end_LF    // 空行\n
};

// 结构体声明
struct mytimer;
class requestData;
class MutexLockGuard;

class requestData : public std::enable_shared_from_this<requestData>  // 自动添加成员函数shared_from_this
{
private:
  int againTimes;    // Request Aborted次数
  std::string path;   // PATH="/"
  int fd;     // 客户端(服务器)fd
  std::string IP;    // 客户端IP
  int epollfd;   // epollfd
  // content的内容用完就清
  std::string content;   // 读取的内容
  int method;    // 请求方式GET/POST
  int HTTPversion;   // HTTP版本
  std::string file_name;    // 请求的文件路径
  int now_read_pos;      // 当前读取下标
  int state;    // 当前读取状态
  int h_state;   // 请求头状态
  bool isfinish;   // 是否解析完
  bool keep_alive;   // 长连接
  std::unordered_map<std::string, std::string> headers;   // 请求头key-value
  std::weak_ptr<mytimer> timer;

private:
  int parse_URI();
  int parse_Headers();
  int analysisRequest();

public:
  requestData();
  requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path);
  ~requestData();
  void addTimer(std::shared_ptr<mytimer> mtimer);
  void reset();
  void seperateTimer();
  int getFd();
  void setFd(int _fd);
  void handleRequest();
  void handleError(int fd, int err_num, std::string short_msg);
};

struct mytimer
{
  bool deleted;   // 是否删除计时器
  size_t expired_time;    // 过期时间
  std::shared_ptr<requestData> request_data;

  mytimer(std::shared_ptr<requestData> _request_data, int timeout);
  ~mytimer();
  void update(int timeout);
  bool isvalid();
  void clearReq();
  void setDeleted();
  bool isDeleted() const;
  size_t getExpTime() const;
};

// 计时器比较规则(小根堆)
struct timerCmp
{
  bool operator()(std::shared_ptr<mytimer> &a, std::shared_ptr<mytimer> &b) const;
};

// RAII锁机制，使锁能够自动释放
class MutexLockGuard
{
public:
    explicit MutexLockGuard();
    ~MutexLockGuard();

private:
    static pthread_mutex_t lock;

private:
    MutexLockGuard(const MutexLockGuard&);
    MutexLockGuard& operator=(const MutexLockGuard&);
};
#endif
