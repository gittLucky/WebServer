cmake_minimum_required(VERSION 3.10)
set(SRCS1
    ../lib/epoll.cpp
    ../lib/HttpRequestData.cpp
    ../lib/threadpool.cpp
    ../lib/util.cpp
    ../lib/log.cpp
    ../lib/sql.cpp
    ../lib/timer.cpp
    ../lib/connectionPool.cpp
    ../tinyxml/src/tinyxml.cpp
    ../tinyxml/src/tinystr.cpp
    ../tinyxml/src/tinyxmlerror.cpp
    ../tinyxml/src/tinyxmlparser.cpp
)
add_executable(webServer ${SRCS1} main.cpp)
target_link_libraries(webServer mysqlclient)