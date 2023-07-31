#ifndef EVENTPOLL
#define EVENTPOLL
#include "HttpRequestData.h"
#include <sys/types.h>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>
#include <memory>

class Epoll
{
private:
    // epoll返回事件
    static epoll_event *events;
    // 存放fd与request对应关系的哈希表
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

#endif