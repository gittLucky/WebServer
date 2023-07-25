main.cpp

#include "HttpRequestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"

#include <sys/epoll.h>
#include <queue>
#include <sys/types.h>  // __uint32_t
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

using namespace std;

// 定义线程池最大线程数与最小线程数，任务队列最大任务数
const int THREADPOOL_MAX_THREAD_NUM = 100;
const int THREADPOOL_MIN_THREAD_NUM = 4;
const int QUEUE_MAX_SIZE = 65535;
// const int QUEUE_MAX_SIZE = 100;

const string PATH = "/";

// 服务器使用的端口
const int PORT = 8888;

// 计时器超时时间(毫秒)
const int TIMER_TIME_OUT = 500;

extern CLogFile logfile;  // 服务程序的运行日志

extern pthread_mutex_t qlock;
extern pthread_mutex_t log_lock;
extern struct epoll_event *events;
void acceptConnection(int listen_fd, int epoll_fd, const string &path);

extern priority_queue<mytimer *, deque<mytimer *>, timerCmp> myTimerQueue;

// 初始化监听描述符
int socket_bind_listen(int port)
{
    // 检查port值，取正确区间范围
    if (port < 1024 || port > 65535)
        return -1;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // 消除bind时"Address already in use"错误
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // 设置服务器IP和Port，和监听描述符绑定
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(listen_fd);
        return -1;
    }

    // 开始监听，最大等待队列长为LISTENQ
    if (listen(listen_fd, LISTENQ) == -1) {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// 任务处理函数
void myHandler(void *args)
{
    requestData *req_data = (requestData *)args;
    req_data->handleRequest();
}

// 监听描述符，接受新连接
void acceptConnection(int listen_fd, int epoll_fd, const string &path)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    // 此处使用while循环是解决边沿触发问题，多个连接请求同时到达，epoll_wait只会通知一次，导致有的连接没有响应
    while ((accept_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len)) > 0) {
        /*
        // TCP的保活机制默认是关闭的，开启后会增加网路负荷，从而影响性能
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */

        // 记录连接日志
        char *str = inet_ntoa(client_addr.sin_addr);
        pthread_mutex_lock(&log_lock);
        logfile.Write("客户端(%s)已连接。\n", str);
        // 设为非阻塞模式
        int ret = setnonblocking(accept_fd);
        if (ret < 0) {
            logfile.Write("Set accept non block failed!\n");
            return;
        }
        pthread_mutex_unlock(&log_lock);

        requestData *req_info = new requestData(epoll_fd, accept_fd, string(str), path);

        // 文件描述符可以读，边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        epoll_add(epoll_fd, accept_fd, static_cast<void *>(req_info), _epo_event);
        // 新增时间信息，为每一个新的连接添加一个过期时间
        mytimer *mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info->addTimer(mtimer);
        pthread_mutex_lock(&qlock);
        myTimerQueue.push(mtimer);
        pthread_mutex_unlock(&qlock);
    }
}
// 分发处理函数
void handle_events(
    int epoll_fd, int listen_fd, struct epoll_event *events, int events_num, const string &path, threadpool_t *tp)
{
    for (int i = 0; i < events_num; i++) {
        // 获取有事件产生的描述符
        requestData *request = (requestData *)(events[i].data.ptr);
        int fd = request->getFd();

        // 有事件发生的描述符为监听描述符
        if (fd == listen_fd) {
            // cout << "This is listen_fd" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        } else {
            // 排除错误事件
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN))) {
                printf("error event\n");
                delete request;
                continue;
            }

            // 将请求任务加入到线程池中
            // 加入线程池之前将Timer和request分离
            request->seperateTimer();
            int rc = threadpool_add(tp, myHandler, events[i].data.ptr);
        }
    }
}

/* 处理逻辑是这样的~
因为(1) 优先队列不支持随机访问
(2) 即使支持，随机删除某节点后破坏了堆的结构，需要重新更新堆结构。
所以对于被置为deleted的时间节点，会延迟到它(1)超时 或 (2)它前面的节点都被删除时，它才会被删除。
一个点被置为deleted,它最迟会在TIMER_TIME_OUT时间后被删除。
这样做有两个好处：
(1) 第一个好处是不需要遍历优先队列，省时。
(2)
第二个好处是给超时时间一个容忍的时间，就是设定的超时时间是删除的下限(并不是一到超时时间就立即删除)，如果监听的请求在超时后的下一次请求中又一次出现了，
就不用再重新申请requestData节点了，这样可以继续重复利用前面的requestData，减少了一次delete和一次new的时间。
*/

/*
 此函数用来计时，当加入epoll红黑树上边的结点超时时候会进行剔除，每次有事件发生时就要进行分离，
 处理完事件后重新进行计时；
 所以计时器计时的不是事件处理  超时，而是加到epoll里长时间滞留，没有事件发生
*/
void handle_expired_event()
{
    pthread_mutex_lock(&qlock);
    while (!myTimerQueue.empty()) {
        mytimer *ptimer_now = myTimerQueue.top();
        if (ptimer_now->isDeleted()) {
            myTimerQueue.pop();
            delete ptimer_now;
        } else if (ptimer_now->isvalid() == false) {
            myTimerQueue.pop();
            delete ptimer_now;
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&qlock);
}

int main()
{
    handle_for_sigpipe();
    // 打开日志文件
    if (logfile.Open("/home/student-4/wh/vscode-workspace/logfile.log", "a+") == false) {
        printf("logfile.Open(%s) failed.\n", "/home/student-4/wh/vscode-workspace/logfile.log");
        return -1;
    }
    int epoll_fd = epoll_init();
    if (epoll_fd < 0) {
        logfile.Write("epoll init failed.\n");
        return 1;
    }
    threadpool_t *threadpool = threadpool_create(THREADPOOL_MIN_THREAD_NUM, THREADPOOL_MAX_THREAD_NUM, QUEUE_MAX_SIZE);
    if (threadpool == NULL) {
        logfile.Write("threadpool create failed\n");
    }
    int listen_fd = socket_bind_listen(PORT);
    if (listen_fd < 0) {
        logfile.Write("socket bind failed\n");
        return 1;
    }
    if (setnonblocking(listen_fd) < 0) {
        logfile.Write("set listen socket non block failed\n");
        return 1;
    }
    __uint32_t event = EPOLLIN | EPOLLET;
    requestData *req = new requestData();
    req->setFd(listen_fd);
    int ret = epoll_add(epoll_fd, listen_fd, static_cast<void *>(req), event);
    if (ret < 0) {
        logfile.Write("epoll add failed\n");
        return 1;
    }
    while (true) {
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
        // printf("%zu\n", myTimerQueue.size());
        if (events_num < 0) {
            pthread_mutex_lock(&log_lock);
            logfile.Write("epoll wait failed\n");
            pthread_mutex_unlock(&log_lock);
            return 1;
        }
        if (events_num == 0)
            continue;
        // 遍历events数组，根据监听种类及描述符类型分发操作
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);

        handle_expired_event();
    }
    return 0;
}

HttpRequestData.cpp

#include "HttpRequestData.h"
#include "util.h"
#include "epoll.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/time.h> // gettimeofday
#include <fcntl.h>    // open
#include <pthread.h>
#include <sys/stat.h> // stat函数
#include <sys/mman.h> // mmap函数
#include <queue>
#include <string.h>

// #include <opencv/cv.h>
// #include <opencv2/core/core.hpp>
// #include <opencv2/highgui/highgui.hpp>
// #include <opencv2/opencv.hpp>
// using namespace cv;

// test
#include <iostream>
using namespace std;

pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

CLogFile logfile;

// 静态成员类外初始化
pthread_mutex_t MimeType::lock = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<std::string, std::string> MimeType::mime;

// 定义请求的格式
std::string MimeType::getMime(const std::string &suffix)
{
    if (mime.size() == 0)
    {
        pthread_mutex_lock(&lock);
        if (mime.size() == 0)
        {
            mime[".html"] = "text/html";
            mime[".avi"] = "video/x-msvideo";
            mime[".bmp"] = "image/bmp";
            mime[".c"] = "text/plain";
            mime[".doc"] = "application/msword";
            mime[".gif"] = "image/gif";
            mime[".gz"] = "application/x-gzip";
            mime[".htm"] = "text/html";
            mime[".ico"] = "application/x-ico";
            mime[".jpg"] = "image/jpeg";
            mime[".png"] = "image/png";
            mime[".txt"] = "text/plain";
            mime[".mp3"] = "audio/mp3";
            mime["default"] = "text/html";
        }
        pthread_mutex_unlock(&lock);
    }
    if (mime.find(suffix) == mime.end())
        return mime["default"];
    else
        return mime[suffix];
}

// 保存过期时间列表为小根堆
priority_queue<mytimer *, deque<mytimer *>, timerCmp> myTimerQueue;

// 监听描述符构造函数
requestData::requestData() : now_read_pos(0), state(STATE_PARSE_URI), h_state(h_start),
                             keep_alive(true), againTimes(0), timer(NULL)
{
    cout << "requestData constructed !" << endl;
}

// 连接描述符构造函数
requestData::requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path) : now_read_pos(0), state(STATE_PARSE_URI), h_state(h_start),
                                                                     keep_alive(true), againTimes(0), timer(NULL),
                                                                     path(_path), fd(_fd), IP(addr_IP), epollfd(_epollfd)
{
}

// 析构函数
requestData::~requestData()
{
    cout << "~requestData()" << endl;
    struct epoll_event ev;
    // 超时的一定都是读请求，没有"被动"写。
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.ptr = (void *)this;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    if (timer != NULL)
    {
        timer->clearReq();
        timer = NULL;
    }
    close(fd);
}

// 为requestData添加计时
void requestData::addTimer(mytimer *mtimer)
{
    if (timer == NULL)
        timer = mtimer;
}

// 获取fd
int requestData::getFd()
{
    return fd;
}

// 设置fd
void requestData::setFd(int _fd)
{
    fd = _fd;
}

// 重置requestData
void requestData::reset()
{
    againTimes = 0;
    content.clear();
    file_name.clear();
    path.clear();
    IP.clear();
    now_read_pos = 0;
    state = STATE_PARSE_URI;
    h_state = h_start;
    headers.clear();
    keep_alive = true;
}

void requestData::seperateTimer()
{
    if (timer)
    {
        timer->clearReq();
        timer = NULL;
    }
}

// 事件处理函数
void requestData::handleRequest()
{
    char buff[MAX_BUFF];
    bool isError = false;

    // 此处循环保证边沿触发一次性读取完，continue来保证读取完
    while (true)
    {
        int read_num = readn(fd, buff, MAX_BUFF);
        if (read_num < 0)
        {
            // perror("1");
            isError = true;
            break;
        }
        else if (read_num == 0)
        {
            // 非阻塞模式第一次没有读到，或者对端连接已断开会返回0
            // 有请求出现但是读不到数据，可能是Request Aborted，或者来自网络的数据没有达到等原因
            // perror("read_num == 0");
            // 非阻塞模式第一次没有读到
            if (errno == EAGAIN)
            {
                if (againTimes > AGAIN_MAX_TIMES)
                    isError = true;
                else
                    ++againTimes;
            }
            else if (errno != 0)
                isError = true;
            break;
        }
        string now_read(buff, buff + read_num);
        content += now_read;

        if (state == STATE_PARSE_URI)
        {
            int flag = this->parse_URI();
            if (flag == PARSE_URI_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_URI_ERROR)
            {
                // perror("2");
                isError = true;
                break;
            }
        }
        if (state == STATE_PARSE_HEADERS)
        {
            int flag = this->parse_Headers();
            if (flag == PARSE_HEADER_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_HEADER_ERROR)
            {
                // perror("3");
                isError = true;
                break;
            }
            // 一般POST请求在空行后带请求数据；GET请求不带请求数据，携带在URI中
            if (method == METHOD_POST)
            {
                state = STATE_RECV_BODY;
            }
            else
            {
                state = STATE_ANALYSIS;
            }
        }
        // POST请求
        if (state == STATE_RECV_BODY)
        {
            int content_length = -1;
            if (headers.find("Content-length") != headers.end())
            {
                content_length = stoi(headers["Content-length"]);
            }
            else
            {
                isError = true;
                break;
            }
            // 数据部分本次未读完
            if (content.size() < content_length)
                continue;
            state = STATE_ANALYSIS;
        }
        if (state == STATE_ANALYSIS)
        {
            int flag = this->analysisRequest();
            if (flag < 0)
            {
                isError = true;
                break;
            }
            else if (flag == ANALYSIS_SUCCESS)
            {

                state = STATE_FINISH;
                break;
            }
            else
            {
                isError = true;
                break;
            }
        }
    }

    if (isError)
    {
        pthread_mutex_lock(&log_lock);
        logfile.Write("客户端(%s)HTTP解析错误!\n", IP.c_str());
        pthread_mutex_unlock(&log_lock);
        delete this;
        return;
    }
    // 加入epoll继续
    if (state == STATE_FINISH)
    {
        if (keep_alive)
        {
            // printf("ok\n");
            pthread_mutex_lock(&log_lock);
            logfile.Write("客户端(%s)HTTP解析成功!\n", IP.c_str());
            pthread_mutex_unlock(&log_lock);
            this->reset();
        }
        else
        {
            delete this;
            return;
        }
    }
    // 一定要先加时间信息，否则可能会出现刚加进去，下个in触发来了，然后分离失败后，又加入队列，最后超时被删，
    // 然后正在线程中进行的任务出错，double free错误

    // 新增时间信息
    pthread_mutex_lock(&qlock);
    mytimer *mtimer = new mytimer(this, EPOLL_WAIT_TIME);
    timer = mtimer;
    myTimerQueue.push(mtimer);
    pthread_mutex_unlock(&qlock);

    __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
    int ret = epoll_mod(epollfd, fd, static_cast<void *>(this), _epo_event);
    if (ret < 0)
    {
        // 返回错误处理
        delete this;
        return;
    }
}

// 解析URI：确定属性filename,HTTPversion,method
int requestData::parse_URI()
{
    // str引用content，改变str就是改变content
    string &str = content;
    // 读到完整的请求行再开始解析请求
    int pos = str.find('\r', now_read_pos);
    // 没有找到说明此次读取请求头包含不完全
    if (pos < 0)
    {
        return PARSE_URI_AGAIN;
    }
    // 去掉请求行所占的空间，节省空间
    string request_line = str.substr(0, pos);
    if (str.size() > pos + 1)
        str = str.substr(pos + 1);
    else
        str.clear();
    // Method
    pos = request_line.find("GET");
    if (pos < 0)
    {
        pos = request_line.find("POST");
        if (pos < 0)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            method = METHOD_POST;
        }
    }
    else
    {
        method = METHOD_GET;
    }
    // printf("method = %d\n", method);
    //  filename
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        int _pos = request_line.find(' ', pos);
        if (_pos < 0)
            return PARSE_URI_ERROR;
        else
        {
            if (_pos - pos > 1)
            {
                file_name = request_line.substr(pos, _pos - pos);
                int __pos = file_name.find('?');
                if (__pos >= 0)
                {
                    file_name = file_name.substr(0, __pos);
                }
            }

            else
                file_name = "../doc/index.html";
        }
        pos = _pos;
    }
    // cout << "file_name: ----------" << file_name << endl;
    //  HTTP 版本号
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        if (request_line.size() - pos <= 3)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            string ver = request_line.substr(pos + 1, 3);
            if (ver == "1.0")
                HTTPversion = HTTP_10;
            else if (ver == "1.1")
                HTTPversion = HTTP_11;
            else
                return PARSE_URI_ERROR;
        }
    }
    state = STATE_PARSE_HEADERS;
    return PARSE_URI_SUCCESS;
}

// 解析请求头
int requestData::parse_Headers()
{
    string &str = content;
    int key_start = -1, key_end = -1, value_start = -1, value_end = -1;
    int now_read_line_begin = 0;
    bool notFinish = true;
    for (int i = 0; i < str.size() && notFinish; ++i)
    {
        switch (h_state)
        {
        case h_start:
        {
            if (str[i] == '\n' || str[i] == '\r')
                break;
            h_state = h_key;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        case h_key:
        {
            if (str[i] == ':')
            {
                key_end = i;
                if (key_end - key_start <= 0)
                    return PARSE_HEADER_ERROR;
                h_state = h_colon;
            }
            else if (str[i] == '\n' || str[i] == '\r')
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_colon:
        {
            if (str[i] == ' ')
            {
                h_state = h_spaces_after_colon;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_spaces_after_colon:
        {
            h_state = h_value;
            value_start = i;
            break;
        }
        case h_value:
        {
            if (str[i] == '\r')
            {
                h_state = h_CR;
                value_end = i;
                if (value_end - value_start <= 0)
                    return PARSE_HEADER_ERROR;
            }
            else if (i - value_start > 255)
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_LF;
                string key(str.begin() + key_start, str.begin() + key_end);
                string value(str.begin() + value_start, str.begin() + value_end);
                headers[key] = value;
                now_read_line_begin = i;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_LF:
        {
            if (str[i] == '\r')
            {
                h_state = h_end_CR;
            }
            else
            {
                key_start = i;
                h_state = h_key;
            }
            break;
        }
        case h_end_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_end_LF;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        // 要么后面还有请求体，要么刚好读完空行\n
        case h_end_LF:
        {
            notFinish = false;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        }
    }
    if (h_state == h_end_LF)
    {
        str = str.substr(now_read_line_begin);
        return PARSE_HEADER_SUCCESS;
    }
    // 没读完头
    str = str.substr(now_read_line_begin);
    return PARSE_HEADER_AGAIN;
}

// HTTP响应
int requestData::analysisRequest()
{
    if (method == METHOD_POST)
    {
        // get content
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        // cout << "content=" << content << endl;
        //  test char*
        char *send_content = "I have receiced this.";

        sprintf(header, "%sContent-length: %zu\r\n", header, strlen(send_content));
        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }

        send_len = (size_t)writen(fd, send_content, strlen(send_content));
        if (send_len != strlen(send_content))
        {
            // perror("Send content failed");
            return ANALYSIS_ERROR;
        }
        // cout << "content size ==" << content.size() << endl;
        // 保存发送方数据到vector
        // vector<char> data(content.begin(), content.end());
        // // opencv函数：将vector中内容读到Mat矩阵中
        // Mat test = imdecode(data, CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
        // // 保存到指定的文件receive.bmp
        // imwrite("receive.bmp", test);
        return ANALYSIS_SUCCESS;
    }
    else if (method == METHOD_GET)
    {
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        int dot_pos = file_name.find('.');
        const char *filetype;
        if (dot_pos < 0)
            filetype = MimeType::getMime("default").c_str();
        else
            filetype = MimeType::getMime(file_name.substr(dot_pos)).c_str();
        // 此结构体描述文件的信息
        struct stat sbuf;
        // stat函数获取文件信息保存到sbuf中
        if (stat(file_name.c_str(), &sbuf) < 0)
        {
            handleError(fd, 404, "Not Found!");
            return ANALYSIS_ERROR;
        }

        sprintf(header, "%sContent-type: %s\r\n", header, filetype);
        // 通过Content-length返回文件大小
        sprintf(header, "%sContent-length: %ld\r\n", header, sbuf.st_size);

        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }
        // 打开文件，O_RDONLY只读打开
        int src_fd = open(file_name.c_str(), O_RDONLY, 0);
        // mmap函数类似于read与write，只不过减少了用户态到核心态的拷贝，直接映射到核心态
        // 映射区域起始地址NULL(自动分配)；大小(一般为4KB整数倍)；映射区域自己权限(PROT_READ)可读权限;
        // 映射标志位(MAP_PRIVATE)对映射区的写入操作只反映到缓存区中不会真正写入到文件；文件描述符；偏移量
        // 返回映射起始地址
        char *src_addr = static_cast<char *>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);

        // 发送文件并校验完整性
        send_len = writen(fd, src_addr, sbuf.st_size);
        if (send_len != sbuf.st_size)
        {
            // perror("Send file failed");
            return ANALYSIS_ERROR;
        }
        // 删除映射
        munmap(src_addr, sbuf.st_size);
        return ANALYSIS_SUCCESS;
    }
    else
        return ANALYSIS_ERROR;
}

// 处理文件GET：URI请求错误
void requestData::handleError(int fd, int err_num, string short_msg)
{
    short_msg = " " + short_msg;
    char send_buff[MAX_BUFF];
    string body_buff, header_buff;
    body_buff += "<html><title>TKeed Error</title>";
    body_buff += "<body bgcolor=\"ffffff\">";
    body_buff += to_string(err_num) + short_msg;
    body_buff += "<hr><em> LinYa's Web Server</em>\n</body></html>";

    header_buff += "HTTP/1.1 " + to_string(err_num) + short_msg + "\r\n";
    header_buff += "Content-type: text/html\r\n";
    header_buff += "Connection: close\r\n";
    header_buff += "Content-length: " + to_string(body_buff.size()) + "\r\n";
    header_buff += "\r\n";
    sprintf(send_buff, "%s", header_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
    sprintf(send_buff, "%s", body_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
}

// 为每一个连接添加一个过期时间
mytimer::mytimer(requestData *_request_data, int timeout) : deleted(false), request_data(_request_data)
{
    // cout << "mytimer()" << endl;
    struct timeval now;
    gettimeofday(&now, NULL);
    // 以毫秒计
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

mytimer::~mytimer()
{
    cout << "~mytimer()" << endl;
    if (request_data != NULL)
    {
        // cout << "request_data=" << request_data << endl;
        delete request_data;
        request_data = NULL;
    }
}

void mytimer::update(int timeout)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

// 是否有效(没有超时)
bool mytimer::isvalid()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    size_t temp = ((now.tv_sec * 1000) + (now.tv_usec / 1000));
    if (temp < expired_time)
    {
        return true;
    }
    else
    {
        this->setDeleted();
        return false;
    }
}

void mytimer::clearReq()
{
    request_data = NULL;
    this->setDeleted();
}

void mytimer::setDeleted()
{
    deleted = true;
}

// 是否设置为删除
bool mytimer::isDeleted() const
{
    return deleted;
}

size_t mytimer::getExpTime() const
{
    return expired_time;
}

// 优先级队列的比较规则
bool timerCmp::operator()(const mytimer *a, const mytimer *b) const
{
    return a->getExpTime() > b->getExpTime();
}

HttpRequestData.h

#ifndef HTTPREQUESTDATA
#define HTTPREQUESTDATA
#include <string>
#include <unordered_map>

// URI请求行
const int STATE_PARSE_URI = 1;
// 请求头
const int STATE_PARSE_HEADERS = 2;
// 请求体
const int STATE_RECV_BODY = 3;
// 解析
const int STATE_ANALYSIS = 4;
// 完成
const int STATE_FINISH = 5;

// HTTP读取缓存大小
const int MAX_BUFF = 4096;

// 有请求出现但是读不到数据,可能是Request Aborted,
// 或者来自网络的数据没有达到等原因,
// 对这样的请求尝试超过一定的次数就抛弃
const int AGAIN_MAX_TIMES = 200;

// URI请求行
const int PARSE_URI_AGAIN = -1;
const int PARSE_URI_ERROR = -2;
const int PARSE_URI_SUCCESS = 0;

// 请求头
const int PARSE_HEADER_AGAIN = -1;
const int PARSE_HEADER_ERROR = -2;
const int PARSE_HEADER_SUCCESS = 0;

// 解析
const int ANALYSIS_ERROR = -2;
const int ANALYSIS_SUCCESS = 0;

// 请求方式与HTTP版本
const int METHOD_POST = 1;
const int METHOD_GET = 2;
const int HTTP_10 = 1;
const int HTTP_11 = 2;   // 浏览器发起请求后默认为1.1版本  所以Connection字段会省略

// 计时器过期时间
const int EPOLL_WAIT_TIME = 500;

// 单例模式
class MimeType
{
private:
  static pthread_mutex_t lock;
  static std::unordered_map<std::string, std::string> mime;
  MimeType();
  MimeType(const MimeType &m);

public:
  static std::string getMime(const std::string &suffix);
};

// 请求头格式
enum HeadersState
{
  h_start = 0,  // 开始
  h_key,        // 键key
  h_colon,      // 冒号
  h_spaces_after_colon,  // 冒号后空格
  h_value,   // 值value
  h_CR,     // \r
  h_LF,    // \n
  h_end_CR,   // 空行\r
  h_end_LF    // 空行\n
};

// 结构体声明
struct mytimer;
struct requestData;

struct requestData
{
private:
  int againTimes;    // Request Aborted次数
  std::string path;   // PATH="/"
  int fd;     // 客户端(服务器)fd
  std::string IP;    // 客户端IP
  int epollfd;   // epollfd
  // content的内容用完就清
  std::string content;   // 读取的内容
  int method;    // 请求方式GET/POST
  int HTTPversion;   // HTTP版本
  std::string file_name;    // 请求的文件路径
  int now_read_pos;      // 当前读取下标
  int state;    // 当前读取状态
  int h_state;   // 请求头状态
  bool isfinish;   // 是否解析完
  bool keep_alive;   // 长连接
  std::unordered_map<std::string, std::string> headers;   // 请求头key-value
  mytimer *timer;

private:
  int parse_URI();
  int parse_Headers();
  int analysisRequest();

public:
  requestData();
  requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path);
  ~requestData();
  void addTimer(mytimer *mtimer);
  void reset();
  void seperateTimer();
  int getFd();
  void setFd(int _fd);
  void handleRequest();
  void handleError(int fd, int err_num, std::string short_msg);
};

struct mytimer
{
  bool deleted;   // 是否删除计时器
  size_t expired_time;    // 过期时间
  requestData *request_data;

  mytimer(requestData *_request_data, int timeout);
  ~mytimer();
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
  bool operator()(const mytimer *a, const mytimer *b) const;
};
#endif
