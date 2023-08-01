#pragma once
#include "nocopyable.hpp"
#include <pthread.h>

class MutexLock : noncopyable
{
public:
    MutexLock()
    {
        pthread_mutex_init(&mutex, NULL);
    }
    ~MutexLock()
    {
        pthread_mutex_lock(&mutex);
        pthread_mutex_destroy(&mutex);
    }
    void lock()
    {
        pthread_mutex_lock(&mutex);
    }
    void unlock()
    {
        pthread_mutex_unlock(&mutex);
    }

private:
    pthread_mutex_t mutex;
};

// RAII锁机制，使锁能够自动释放
// 析构函数先析构自己
class MutexLockGuard : noncopyable
{
public:
    explicit MutexLockGuard(MutexLock &_mutex) : mutex(_mutex)
    {
        mutex.lock();
    }
    ~MutexLockGuard()
    {
        mutex.unlock();
    }

private:
    MutexLock &mutex;
};