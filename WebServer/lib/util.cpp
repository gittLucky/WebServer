#include "util.h"
#include "_cmpublic.h"

// HTTP读取缓存大小
const int MAX_BUFF = 4096;

bool TcpRead(const int sockfd, char *buffer, int *ibuflen, const int itimeout)
{
  if (sockfd == -1)
    return false;

  if (itimeout > 0)
  {
    fd_set tmpfd;

    FD_ZERO(&tmpfd);
    FD_SET(sockfd, &tmpfd);

    struct timeval timeout;
    timeout.tv_sec = itimeout;
    timeout.tv_usec = 0;

    int i;
    if ((i = select(sockfd + 1, &tmpfd, 0, 0, &timeout)) <= 0)
      return false;
  }

  (*ibuflen) = 0;

  if (readn(sockfd, (void *)ibuflen, 4) == false)
    return false;

  (*ibuflen) = ntohl(*ibuflen); // 把网络字节序转换为主机字节序

  if (readn(sockfd, buffer, (*ibuflen)) == false)
    return false;

  return true;
}

// 全局的发送函数，在多线程中使用
bool TcpWrite(const int sockfd, const char *buffer, const int ibuflen)
{
  if (sockfd == -1)
    return false;

  fd_set tmpfd;

  FD_ZERO(&tmpfd);
  FD_SET(sockfd, &tmpfd);

  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;

  if (select(sockfd + 1, 0, &tmpfd, 0, &timeout) <= 0)
    return false;

  int ilen = 0;

  // 如果长度为0，就采用字符串的长度
  if (ibuflen == 0)
    ilen = strlen(buffer);
  else
    ilen = ibuflen;

  int ilenn = htonl(ilen); // 转换为网络字节序

  char strTBuffer[ilen + 4];
  memset(strTBuffer, 0, sizeof(strTBuffer));
  memcpy(strTBuffer, &ilenn, 4);
  memcpy(strTBuffer + 4, buffer, ilen);

  if (writen(sockfd, strTBuffer, ilen + 4) == false)
    return false;

  return true;
}

// 每次读取n字节
ssize_t readn(int fd, void *buff, size_t n)
{
  size_t nleft = n;
  ssize_t nread = 0;
  ssize_t readSum = 0;
  char *ptr = (char *)buff;
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

ssize_t readn(int fd, std::string &inBuffer)
{
  ssize_t nread = 0;
  ssize_t readSum = 0;
  while (true)
  {
    char buff[MAX_BUFF];
    if ((nread = read(fd, buff, MAX_BUFF)) < 0)
    {
      if (errno == EINTR)
        continue;
      else if (errno == EAGAIN)
      {
        return readSum;
      }
      else
      {
        // perror("read error");
        return -1;
      }
    }
    else if (nread == 0)
      break;
    readSum += nread;
    inBuffer += std::string(buff, buff + nread);
  }
  return readSum;
}

// 每次写n字节
ssize_t writen(int fd, void *buff, size_t n)
{
  size_t nleft = n;
  ssize_t nwritten = 0;
  ssize_t writeSum = 0;
  char *ptr = (char *)buff;
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

ssize_t writen(int fd, std::string &sbuff)
{
  size_t nleft = sbuff.size();
  ssize_t nwritten = 0;
  ssize_t writeSum = 0;
  const char *ptr = sbuff.c_str();
  while (nleft > 0)
  {
    if ((nwritten = write(fd, ptr, nleft)) <= 0)
    {
      if (nwritten < 0)
      {
        if (errno == EINTR)
        {
          nwritten = 0;
          continue;
        }
        // 缓存区已满
        else if (errno == EAGAIN)
          break;
        else
          return -1;
      }
    }
    writeSum += nwritten;
    nleft -= nwritten;
    ptr += nwritten;
  }
  if (writeSum == sbuff.size())
    sbuff.clear();
  else
    sbuff = sbuff.substr(writeSum);
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
  if (sigaction(SIGPIPE, &sa, NULL))
    return;
}

// 把socket设置为非阻塞的方式
int setnonblocking(int sockfd)
{
  if (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK) == -1)
    return -1;
  return 0;
}