#include "HttpRequestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"
#include "_cmpublic.h"
#include "log.h"

using namespace std;

// static const int MAXEVENTS = 5000;
const int MAXEVENTS = 10;
// 监听最大描述符
static const int LISTENQ = 1024;

// 定义线程池最大线程数与最小线程数，任务队列最大任务数
const int THREADPOOL_MAX_THREAD_NUM = 100;
const int THREADPOOL_MIN_THREAD_NUM = 4;
// const int QUEUE_MAX_SIZE = 65535;
const int QUEUE_MAX_SIZE = 100;

// 服务器使用的端口
const int PORT = 8888;

extern CLogFile logfile;  // 服务程序的运行日志

extern priority_queue<shared_ptr<mytimer>, deque<shared_ptr<mytimer>>, timerCmp> myTimerQueue;

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
        shared_ptr<mytimer> ptimer_now = myTimerQueue.top();
        if (ptimer_now->isDeleted())
        {
            myTimerQueue.pop();
            // delete ptimer_now;
        }
        else if (ptimer_now->isvalid() == false)
        {
            myTimerQueue.pop();
            // delete ptimer_now;
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
    if (logfile.Open("/home/student-4/wh/vscode-workspace/WebServer0730/logfile.log", "a+") == false) {
        printf("logfile.Open(%s) failed.\n", "/home/student-4/wh/vscode-workspace/WebServer0730/logfile.log");
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
    request->setFd(listen_fd);
    int ret = Epoll::epoll_add(listen_fd, request, event);
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