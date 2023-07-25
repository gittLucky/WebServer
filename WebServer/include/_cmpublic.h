// 头文件归纳
#ifndef _cmpublic_H
#define _cmpublic_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>  // __uint32_t
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>   // open fcntl
#include <time.h>
#include <utime.h>  // struct utimbuf
#include <netdb.h>  // gethostbyname
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>  // errno初始值为0表示没有错误
#include <sys/epoll.h>
#include <netinet/in.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>  // va_list
#include <sys/time.h> // gettimeofday
#include <pthread.h>
#include <sys/stat.h> // stat函数
#include <sys/mman.h> // mmap函数
#include <iostream>
#include <queue>

#endif