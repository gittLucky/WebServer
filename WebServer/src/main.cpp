#include "HttpRequestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "connectionPool.h"
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

extern CLogFile logfile; // 服务程序的运行日志

// 初始化监听描述符
int socket_bind_listen(int port)
{
    // 检查port值，取正确区间范围
    if (port < 1024 || port > 65535)
        return -1;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // 消除bind时"Address already in use"错误
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // 设置服务器IP和Port，和监听描述符绑定
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        close(listen_fd);
        return -1;
    }

    // 开始监听，最大等待队列长为LISTENQ
    if (listen(listen_fd, LISTENQ) == -1)
    {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

int main()
{
    handle_for_sigpipe();
    // 打开日志文件
    if (logfile.Open("/home/student-4/wh/vscode-workspace/webServer/logfile.log", "a+") == false)
    {
        printf("logfile.Open(%s) failed.\n", "/home/student-4/wh/vscode-workspace/webServer/logfile.log");
        return -1;
    }
    if (Epoll::epoll_init(MAXEVENTS, LISTENQ) < 0)
    {
        logfile.Write("epoll init failed.\n");
        return 1;
    }
    if (ThreadPool::threadpool_create(THREADPOOL_MIN_THREAD_NUM, THREADPOOL_MAX_THREAD_NUM, QUEUE_MAX_SIZE) < 0)
    {
        logfile.Write("threadpool create failed\n");
        return 1;
    }
    if(ConnectionPool::sqlConnectionPoolCreate() < 0)
    {
        logfile.Write("数据库连接失败！\n");
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
    shared_ptr<RequestData> request(new RequestData());
    request->setFd(listen_fd);
    int ret = Epoll::epoll_add(listen_fd, request, event);
    if (ret < 0)
    {
        logfile.Write("epoll add failed\n");
        return 1;
    }
    while (true)
    {
        if (Epoll::my_epoll_wait(listen_fd, MAXEVENTS, -1) < 0)
        {
            MutexLockGuard_LOG();
            // pthread_mutex_lock(&log_lock);
            logfile.Write("epoll wait failed\n");
            // pthread_mutex_unlock(&log_lock);
            return 1;
        }
    }
    return 0;
}