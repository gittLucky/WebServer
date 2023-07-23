#include "util.h"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

// 每次读取n字节
ssize_t readn(int fd, void *buff, size_t n)
{
    size_t nleft = n;
    ssize_t nread = 0;
    ssize_t readSum = 0;
    char *ptr = (char*)buff;
    while (nleft > 0)
    {
        if ((nread = read(fd, ptr, nleft)) < 0)
        {
            // 读取被中断需要重新读
            if (errno == EINTR)
                nread = 0;
            // 非阻塞形式
            else if (errno == EAGAIN)
            {
                return readSum;
            }
            else
            {
                return -1;
            }  
        }
        else if (nread == 0)
            break;
        readSum += nread;
        nleft -= nread;
        ptr += nread;
    }
    return readSum;
}

// 每次写n字节
ssize_t writen(int fd, void *buff, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten = 0;
    ssize_t writeSum = 0;
    char *ptr = (char*)buff;
    while (nleft > 0)
    {
        if ((nwritten = write(fd, ptr, nleft)) <= 0)
        {
            if (nwritten < 0)
            {
                // 如果被中断或者缓存区满了写不出去，一直写，直到写出去
                if (errno == EINTR || errno == EAGAIN)
                {
                    nwritten = 0;
                    continue;
                }
                else
                    return -1;
            }
        }
        writeSum += nwritten;
        nleft -= nwritten;
        ptr += nwritten;
    }
    return writeSum;
}

/*
 当client连接到server之后，这时候server准备向client发送多条数据，但在发送之前，client进程意外奔溃了，那么接下来server在发送多条
 信息的过程中，就会出现SIGPIPE信号。
 此时相当于四次挥手客户端方向已经断开，但是服务器还可以发送数据，对一个已经收到FIN包的socket调用read方法，如果缓存为空，则返回0表示
 客户端连接关闭
 但第一次调用write时可以正常发送，对端发送RST报文，第二次调用write方法，会收到SIGPIPE信号，导致整个进程结束
*/

// 忽略SIGPIPE信号，write第二次发送时会返回-1，error的值设为EPIPE，所以不会产生SIGPIPE信号
void handle_for_sigpipe()
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if(sigaction(SIGPIPE, &sa, NULL))
        return;
}

// 把socket设置为非阻塞的方式
int setnonblocking(int sockfd)
{
    if (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK) == -1)
        return -1;
    return 0;
}