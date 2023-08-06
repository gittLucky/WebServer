#include "HttpRequestData.h"
#include "util.h"
#include "epoll.h"
#include "_cmpublic.h"
#include "log.h"

// #include <opencv/cv.h>
// #include <opencv2/core/core.hpp>
// #include <opencv2/highgui/highgui.hpp>
// #include <opencv2/opencv.hpp>
// using namespace cv;

CLogFile logfile;

// 静态成员类外初始化
pthread_once_t MimeType::once_control = PTHREAD_ONCE_INIT;
std::unordered_map<std::string, std::string> MimeType::mime;

// 定义请求的格式
void MimeType::init()
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

std::string MimeType::getMime(const std::string &suffix)
{
    // 本函数使用初值为PTHREAD_ONCE_INIT的once_control变量保证init函数在本进程执行序列中仅执行一次
    pthread_once(&once_control, MimeType::init);
    if (mime.find(suffix) == mime.end())
        return mime["default"];
    else
        return mime[suffix];
}

// 监听描述符构造函数
RequestData::RequestData() : now_read_pos(0),
                             state(STATE_PARSE_URI),
                             h_state(h_start),
                             keep_alive(true),
                             isAbleRead(true),
                             isAbleWrite(false),
                             isError(false),
                             events(0),
                             againTimes(0)
{
}

// 连接描述符构造函数
RequestData::RequestData(int _epollfd, int _fd, std::string addr_IP, std::string _path) : now_read_pos(0),
                                                                                          state(STATE_PARSE_URI),
                                                                                          h_state(h_start),
                                                                                          keep_alive(true),
                                                                                          againTimes(0),
                                                                                          path(_path),
                                                                                          fd(_fd),
                                                                                          IP(addr_IP),
                                                                                          epollfd(_epollfd),
                                                                                          isAbleRead(true),
                                                                                          isAbleWrite(false),
                                                                                          events(0),
                                                                                          isError(false)
{
}

// 析构函数
RequestData::~RequestData()
{
    // cout << "~requestData()" << endl;
    // struct epoll_event ev;
    // // 超时的一定都是读请求，没有"被动"写。
    // ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    // ev.data.ptr = (void *)this;
    // epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    // if (timer != NULL)
    // {
    // timer->clearReq();
    // timer = NULL;
    // }
    close(fd);
}

// 为RequestData添加计时
void RequestData::linkTimer(std::shared_ptr<TimerNode> mtimer)
{
    // if (timer == NULL)
    timer = mtimer;
}

// 获取fd
int RequestData::getFd()
{
    return fd;
}

// 设置fd
void RequestData::setFd(int _fd)
{
    fd = _fd;
}

// 重置requestData
void RequestData::reset()
{
    inBuffer.clear();
    againTimes = 0;
    file_name.clear();
    path.clear();
    now_read_pos = 0;
    state = STATE_PARSE_URI;
    h_state = h_start;
    headers.clear();
    keep_alive = true;
    // weak_ptr：use_count()返回与weak_ptr共享的shared_ptr数量
    // expired()若use_count()为0返回true，对象已死
    // lock()若expired()为true返回一个空的shared_ptr，否则返回一个指向对象的shared_ptr
    if (timer.lock())
    {
        std::shared_ptr<TimerNode> my_timer(timer.lock());
        my_timer->clearReq();
        timer.reset();
    }
}

void RequestData::seperateTimer()
{
    if (timer.lock())
    {
        std::shared_ptr<TimerNode> my_timer(timer.lock());
        my_timer->clearReq();
        // timer制空
        timer.reset();
    }
}

// 事件处理函数
void RequestData::handleRead()
{

    // 此处循环保证边沿触发一次性读取完，continue来保证读取完
    // while (true)
    do
    {
        // readn函数保证一次全部读取完
        int read_num = readn(fd, inBuffer);
        if (read_num < 0)
        {
            // perror("1");
            isError = true;
            handleError(fd, 400, "Bad Request");
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

        if (state == STATE_PARSE_URI)
        {
            int flag = this->parse_URI();
            if (flag == PARSE_URI_AGAIN)
            {
                // continue;
                break;
            }
            else if (flag == PARSE_URI_ERROR)
            {
                handleError(fd, 400, "Bad Request");
                isError = true;
                break;
            }
            else
                state = STATE_PARSE_HEADERS;
        }
        if (state == STATE_PARSE_HEADERS)
        {
            int flag = this->parse_Headers();
            if (flag == PARSE_HEADER_AGAIN)
            {
                // continue;
                break;
            }
            else if (flag == PARSE_HEADER_ERROR)
            {
                handleError(fd, 400, "Bad Request");
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
                handleError(fd, 400, "Bad Request: Lack of argument (Content-length)");
                isError = true;
                break;
            }
            // 数据部分本次未读完
            if (inBuffer.size() < content_length)
                // continue;
                break;
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
    } while (false);

    if (isError)
    {
        MutexLockGuard_LOG();
        logfile.Write("客户端(%s)HTTP解析错误!\n", IP.c_str());
        // delete this;
        Epoll::epoll_del(fd);
        return;
    }
    if (outBuffer.size() > 0)
        events |= EPOLLOUT;
    // 加入epoll继续
    if (state == STATE_FINISH)
    {
        if (keep_alive)
        {
            MutexLockGuard_LOG();
            logfile.Write("客户端(%s)HTTP解析成功!\n", IP.c_str());
            this->reset();
            events |= EPOLLIN;
        }
        else
        {
            // delete this;
            return;
        }
    }
    // 从PARSE_HEADER_AGAIN或PARSE_URI_AGAIN或inBuffer.size() < content_length跳出
    // 表示没有读到预期的内容，重新读
    else
    {
        events |= EPOLLIN;
    }
}

void RequestData::handleWrite()
{
    if (!isError)
    {
        if (writen(fd, outBuffer) < 0)
        {
            // perror("writen");
            events = 0;
            isError = true;
            Epoll::epoll_del(fd, (EPOLLOUT | EPOLLET | EPOLLONESHOT));
        }
        else if (outBuffer.size() > 0)
            events |= EPOLLOUT;
    }
}

void RequestData::handleConn()
{
    if (!isError)
    {
        if (events != 0)
        {
            // 一定要先加时间信息，否则可能会出现刚加进去，下个in触发来了，然后分离失败后，又加入队列，最后超时被删，然后正在线程中进行的任务出错，double free错误。
            // 新增时间信息
            int timeout = 2000;
            if (keep_alive)
                timeout = 5 * 60 * 1000; // 超时时间为5分钟
            isAbleRead = false;
            isAbleWrite = false;
            // 新增时间信息
            // pthread_mutex_lock(&qlock);
            // 使用shared_from_this()函数，不是用this，因为这样会造成2个非共享的share_ptr指向同一个对象，
            // 未增加引用计数导对象被析构两次
            Epoll::add_timer(shared_from_this(), timeout);
            if ((events & EPOLLIN) && (events & EPOLLOUT))
            {
                events = __uint32_t(0);
                events |= EPOLLOUT;
            }
            events |= (EPOLLET | EPOLLONESHOT);
            __uint32_t _events = events;
            events = 0;
            if (Epoll::epoll_mod(fd, shared_from_this(), _events) < 0)
            {
                // 返回错误处理
                MutexLockGuard_LOG();
                logfile.Write("epoll mod failed\n");
            }
        }
        else if (keep_alive) // 正常处理完写
        {
            events |= (EPOLLIN | EPOLLET | EPOLLONESHOT);
            int timeout = 5 * 60 * 1000;
            isAbleRead = false;
            isAbleWrite = false;
            Epoll::add_timer(shared_from_this(), timeout);
            __uint32_t _events = events;
            events = 0;
            if (Epoll::epoll_mod(fd, shared_from_this(), _events) < 0)
            {
                // 返回错误处理
                MutexLockGuard_LOG();
                logfile.Write("epoll mod failed\n");
            }
        }
        else
        {
            Epoll::epoll_del(fd, (EPOLLOUT | EPOLLET | EPOLLONESHOT));
        }
    }
}

// 解析URI：确定属性filename,HTTPversion,method
int RequestData::parse_URI()
{
    // str引用inBuffer，改变str就是改变inBuffer
    std::string &str = inBuffer;
    // 读到完整的请求行再开始解析请求
    int pos = str.find('\r', now_read_pos);
    // 没有找到说明此次读取请求头包含不完全
    if (pos < 0)
    {
        return PARSE_URI_AGAIN;
    }
    // 去掉请求行所占的空间，节省空间
    std::string request_line = str.substr(0, pos);
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
    // filename
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
            std::string ver = request_line.substr(pos + 1, 3);
            if (ver == "1.0")
                HTTPversion = HTTP_10;
            else if (ver == "1.1")
                HTTPversion = HTTP_11;
            else
                return PARSE_URI_ERROR;
        }
    }
    return PARSE_URI_SUCCESS;
}

// 解析请求头
int RequestData::parse_Headers()
{
    std::string &str = inBuffer;
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
                std::string key(str.begin() + key_start, str.begin() + key_end);
                std::string value(str.begin() + value_start, str.begin() + value_end);
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
int RequestData::analysisRequest()
{
    if (method == METHOD_POST)
    {
        // get inBuffer
        std::string header;
        header += std::string("HTTP/1.1 200 OK\r\n");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive")
            {
                header += "Connection: keep-alive\r\n";
                header += "Keep-Alive: timeout=" + std::to_string(5 * 60 * 1000) + "\r\n";
            }
            else
            {
                keep_alive = false;
                header += "Connection: close\r\n";
            }
        }
        std::string send_content = "I have receiced this.";

        header += "Content-length:" + std::to_string(send_content.size()) + "\r\n\r\n";
        outBuffer += header + send_content;

        // cout << "content size ==" << content.size() << endl;
        // 保存发送方数据到vector
        // vector<char> data(content.begin(), content.end());
        // // opencv函数：将vector中内容读到Mat矩阵中
        // Mat test = imdecode(data, CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
        // // 保存到指定的文件receive.bmp
        // imwrite("receive.bmp", test);
        int length = stoi(headers["Content-length"]);
        inBuffer = inBuffer.substr(length);
        return ANALYSIS_SUCCESS;
    }
    else if (method == METHOD_GET)
    {
        std::string header;
        header += std::string("HTTP/1.1 200 OK\r\n");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive")
            {
                header += "Connection: keep-alive\r\n";
                header += "Keep-Alive: timeout=" + std::to_string(5 * 60 * 1000) + "\r\n";
            }
            else
            {
                keep_alive = false;
                header += "Connection: close\r\n";
            }
        }
        int dot_pos = file_name.find('.');
        std::string filetype;
        if (dot_pos < 0)
            filetype = MimeType::getMime("default");
        else
            filetype = MimeType::getMime(file_name.substr(dot_pos));
        // 此结构体描述文件的信息
        struct stat sbuf;
        // stat函数获取文件信息保存到sbuf中
        if (stat(file_name.c_str(), &sbuf) < 0)
        {
            header.clear();
            handleError(fd, 404, "Not Found!");
            return ANALYSIS_ERROR;
        }

        header += "Content-type: " + filetype + "\r\n";
        header += "Content-length: " + std::to_string(sbuf.st_size) + "\r\n";
        // 头部结束
        header += "\r\n";
        outBuffer += header;
        // 打开文件，O_RDONLY只读打开
        int src_fd = open(file_name.c_str(), O_RDONLY, 0);
        // mmap函数类似于read与write，只不过减少了用户态到核心态的拷贝，直接映射到核心态
        // 映射区域起始地址NULL(自动分配)；大小(一般为4KB整数倍)；映射区域自己权限(PROT_READ)可读权限;
        // 映射标志位(MAP_PRIVATE)对映射区的写入操作只反映到缓存区中不会真正写入到文件；文件描述符；偏移量
        // 返回映射起始地址
        char *src_addr = static_cast<char *>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);

        outBuffer += src_addr;
        // 删除映射
        munmap(src_addr, sbuf.st_size);
        return ANALYSIS_SUCCESS;
    }
    else
        return ANALYSIS_ERROR;
}

// 处理文件GET：URI请求错误
void RequestData::handleError(int fd, int err_num, std::string short_msg)
{
    short_msg = " " + short_msg;
    char send_buff[MAX_BUFF];
    std::string body_buff, header_buff;
    body_buff += "<html><title>出错了！</title>";
    body_buff += "<body bgcolor=\"ffffff\">";
    body_buff += std::to_string(err_num) + short_msg;
    body_buff += "<hr><em> WH's Web Server</em>\n</body></html>";

    header_buff += "HTTP/1.1 " + std::to_string(err_num) + short_msg + "\r\n";
    header_buff += "Content-type: text/html\r\n";
    header_buff += "Connection: close\r\n";
    header_buff += "Content-length: " + std::to_string(body_buff.size()) + "\r\n";
    header_buff += "\r\n";
    // 错误处理不考虑writen不完的情况
    sprintf(send_buff, "%s", header_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
    sprintf(send_buff, "%s", body_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
}

void RequestData::enableRead()
{
    isAbleRead = true;
}

void RequestData::enableWrite()
{
    isAbleWrite = true;
}

bool RequestData::canRead()
{
    return isAbleRead;
}

bool RequestData::canWrite()
{
    return isAbleWrite;
}

void RequestData::disableReadAndWrite()
{
    isAbleRead = false;
    isAbleWrite = false;
}