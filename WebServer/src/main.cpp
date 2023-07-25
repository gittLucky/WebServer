#include "HttpRequestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"
#include "_cmpublic.h"
#include "log.h"

using namespace std;

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
    int epoll_fd = epoll_init();
    if (epoll_fd < 0)
    {
        logfile.Write("epoll init failed.\n");
        return 1;
    }
    threadpool_t *threadpool = threadpool_create(THREADPOOL_MIN_THREAD_NUM, THREADPOOL_MAX_THREAD_NUM, QUEUE_MAX_SIZE);
    if (threadpool == NULL) {
        logfile.Write("threadpool create failed\n");
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
    requestData *req = new requestData();
    req->setFd(listen_fd);
    int ret = epoll_add(epoll_fd, listen_fd, static_cast<void*>(req), event);
    if (ret < 0) {
        logfile.Write("epoll add failed\n");
        return 1;
    }
    while (true)
    {
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
        //printf("%zu\n", myTimerQueue.size());
        if (events_num < 0) {
            MutexLockGuard_LOG();
            // pthread_mutex_lock(&log_lock);
            logfile.Write("epoll wait failed\n");
            // pthread_mutex_unlock(&log_lock);
            return 1;
        }
        if (events_num == 0)
            continue;
        // 遍历events数组，根据监听种类及描述符类型分发操作
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);

        handle_expired_event();
    }
    return 0;
}