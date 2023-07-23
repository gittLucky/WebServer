#ifndef UTIL
#define UTIL
#include <sys/types.h>

ssize_t readn(int fd, void *buff, size_t n);
ssize_t writen(int fd, void *buff, size_t n);
void handle_for_sigpipe();
int setnonblocking(int fd);

#endif