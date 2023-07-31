#ifndef HTTPREQUESTDATA
#define HTTPREQUESTDATA
#include <string>
#include <unordered_map>
#include <memory>

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
const int HTTP_11 = 2; // 浏览器发起请求后默认为1.1版本  所以Connection字段会省略

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
  h_start = 0,          // 开始
  h_key,                // 键key
  h_colon,              // 冒号
  h_spaces_after_colon, // 冒号后空格
  h_value,              // 值value
  h_CR,                 // \r
  h_LF,                 // \n
  h_end_CR,             // 空行\r
  h_end_LF              // 空行\n
};

// 结构体声明
struct mytimer;
class requestData;
class MutexLockGuard;

class requestData : public std::enable_shared_from_this<requestData> // 自动添加成员函数shared_from_this
{
private:
  int againTimes;   // Request Aborted次数
  std::string path; // PATH="/"
  int fd;           // 客户端(服务器)fd
  std::string IP;   // 客户端IP
  int epollfd;      // epollfd
  // content的内容用完就清
  std::string content;                                  // 读取的内容
  int method;                                           // 请求方式GET/POST
  int HTTPversion;                                      // HTTP版本
  std::string file_name;                                // 请求的文件路径
  int now_read_pos;                                     // 当前读取下标
  int state;                                            // 当前读取状态
  int h_state;                                          // 请求头状态
  bool isfinish;                                        // 是否解析完
  bool keep_alive;                                      // 长连接
  std::unordered_map<std::string, std::string> headers; // 请求头key-value
  std::weak_ptr<mytimer> timer;

private:
  int parse_URI();
  int parse_Headers();
  int analysisRequest();

public:
  requestData();
  requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path);
  ~requestData();
  void addTimer(std::shared_ptr<mytimer> mtimer);
  void reset();
  void seperateTimer();
  int getFd();
  void setFd(int _fd);
  void handleRequest();
  void handleError(int fd, int err_num, std::string short_msg);
};

struct mytimer
{
  bool deleted;        // 是否删除计时器
  size_t expired_time; // 过期时间
  std::shared_ptr<requestData> request_data;

  mytimer(std::shared_ptr<requestData> _request_data, int timeout);
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
  bool operator()(std::shared_ptr<mytimer> &a, std::shared_ptr<mytimer> &b) const;
};

// RAII锁机制，使锁能够自动释放
class MutexLockGuard
{
public:
  explicit MutexLockGuard();
  ~MutexLockGuard();

private:
  static pthread_mutex_t lock;

private:
  MutexLockGuard(const MutexLockGuard &);
  MutexLockGuard &operator=(const MutexLockGuard &);
};
#endif