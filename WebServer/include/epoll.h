#ifndef EVENTPOLL
#define EVENTPOLL
#include "HttpRequestData.h"
#include "timer.h"
#include <sys/types.h>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>
#include <memory>

class Epoll
{
public:
    typedef std::shared_ptr<RequestData> SP_ReqData;
private:
    // epoll返回事件
    static epoll_event *events;
    // 存放fd与request对应关系的哈希表
    static std::unordered_map<int, SP_ReqData> fd2req;
    static int epoll_fd;
    static const std::string PATH;

    static TimerManager timer_manager;

public:
    static int epoll_init(int maxevents, int listen_num);
    static int epoll_add(int fd, SP_ReqData request, __uint32_t events);
    static int epoll_mod(int fd, SP_ReqData request, __uint32_t events);
    static int epoll_del(int fd, __uint32_t events = (EPOLLIN | EPOLLET | EPOLLONESHOT));
    static int my_epoll_wait(int listen_fd, int max_events, int timeout);
    static void acceptConnection(int listen_fd, int epoll_fd, const std::string path);
    static std::vector<SP_ReqData> getEventsRequest(int listen_fd, int events_num, const std::string path);

    static void add_timer(SP_ReqData request_data, int timeout);
};

#endif