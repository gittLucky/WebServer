cmake_minimum_required(VERSION 3.10)
set(SRCS1
    ../lib/epoll.cpp
    ../lib/HttpRequestData.cpp
    ../lib/threadpool.cpp
    ../lib/util.cpp
)

add_executable(webServer ${SRCS1} main.cpp)