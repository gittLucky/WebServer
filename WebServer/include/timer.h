#ifndef TIMER_H
#define TIMER_H
#include "HttpRequestData.h"
#include "../base/nocopyable.hpp"
#include "../base/mutexLock.hpp"
#include <unistd.h>
#include <memory>
#include <queue>
#include <deque>

class RequestData;

class TimerNode
{
public:
    typedef std::shared_ptr<RequestData> SP_ReqData;
private:
    bool deleted;
    size_t expired_time;
    SP_ReqData request_data;

public:
    TimerNode(SP_ReqData _request_data, int timeout);
    ~TimerNode();
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
    bool operator()(std::shared_ptr<TimerNode> &a, std::shared_ptr<TimerNode> &b) const
    {
        return a->getExpTime() > b->getExpTime();
    }
};

class TimerManager
{
public:
    typedef std::shared_ptr<RequestData> SP_ReqData;
    typedef std::shared_ptr<TimerNode> SP_TimerNode;

private:
    // 保存过期时间列表为小根堆
    std::priority_queue<SP_TimerNode, std::deque<SP_TimerNode>, timerCmp> TimerNodeQueue;
    MutexLock lock;

public:
    TimerManager();
    ~TimerManager();
    void addTimer(SP_ReqData request_data, int timeout);
    void addTimer(SP_TimerNode timer_node);
    void handle_expired_event();
};

#endif