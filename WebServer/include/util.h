#ifndef UTIL
#define UTIL
#include <sys/types.h>
#include <string>

ssize_t readn(int fd, void *buff, size_t n);
ssize_t readn(int fd, std::string &inBuffer);
ssize_t writen(int fd, void *buff, size_t n);
ssize_t writen(int fd, std::string &sbuff);
void handle_for_sigpipe();
int setnonblocking(int fd);

// 接收socket的对端发送过来的数据
// sockfd：可用的socket连接
// buffer：接收数据缓冲区的地址
// ibuflen：本次成功接收数据的字节数
// itimeout：接收等待超时的时间，单位：秒，缺省值是0-无限等待
// 返回值：true-成功；false-失败，失败有两种情况：1）等待超时；2）socket连接已不可用
bool TcpRead(const int sockfd, char *buffer, int *ibuflen, const int itimeout = 0);

// 向socket的对端发送数据
// sockfd：可用的socket连接
// buffer：待发送数据缓冲区的地址
// ibuflen：待发送数据的字节数，如果发送的是ascii字符串，ibuflen取0；
// 如果是二进制流数据，ibuflen为二进制数据块的大小
// 返回值：true-成功；false-失败，如果失败，表示socket连接已不可用
bool TcpWrite(const int sockfd, const char *buffer, const int ibuflen = 0);

#endif