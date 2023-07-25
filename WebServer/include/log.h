#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>
// 以下是日志文件操作类
// 日志文件操作类
class CLogFile
{
public:
  FILE *m_tracefp;      // 日志文件指针
  char m_filename[301]; // 日志文件名，建议采用绝对路径
  char m_openmode[11];  // 日志文件的打开方式，一般采用"a+"
  bool m_bEnBuffer;     // 写入日志时，是否启用操作系统的缓冲机制，缺省不启用
  long m_MaxLogSize;    // 最大日志文件的大小，单位M，缺省100M
  bool m_bBackup;       // 是否自动切换，日志文件大小超过m_MaxLogSize将自动切换，缺省启用

  // 构造函数
  // MaxLogSize：最大日志文件的大小，单位M，缺省100M，最小为10M
  CLogFile(const long MaxLogSize = 100);

  // 打开日志文件
  // filename：日志文件名，建议采用绝对路径，如果文件名中的目录不存在，就先创建目录
  // openmode：日志文件的打开方式，与fopen库函数打开文件的方式相同，缺省值是"a+"
  // bBackup：是否自动切换，true-切换，false-不切换，在多进程的服务程序中，如果多个进程共用一个日志文件，bBackup必须为false
  // bEnBuffer：是否启用文件缓冲机制，true-启用，false-不启用，
  // 如果启用缓冲区，那么写进日志文件中的内容不会立即写入文件，缺省是不启用
  bool Open(const char *filename, const char *openmode = 0, bool bBackup = true, bool bEnBuffer = false);

  // 如果日志文件大于m_MaxLogSize的值，就把当前的日志文件名改为历史日志文件名，再创建新的当前日志文件。
  // 备份后的文件会在日志文件名后加上日期时间，如/tmp/log/filetodb.log.20200101123025。
  // 注意，在多进程的程序中，日志文件不可切换，多线的程序中，日志文件可以切换。
  bool BackupLogFile();

  // 把内容写入日志文件，fmt是可变参数，使用方法与printf库函数相同。
  // Write方法会写入当前的时间，WriteEx方法不写时间。
  bool Write(const char *fmt, ...);
  bool WriteEx(const char *fmt, ...);

  // 关闭日志文件
  void Close();

  ~CLogFile(); // 析构函数会调用Close方法
};

// 打开文件
// FOPEN函数调用fopen库函数打开文件，如果文件名中包含的目录不存在，就创建目录
// FOPEN函数的参数和返回值与fopen函数完全相同
// 在应用开发中，用FOPEN函数代替fopen库函数
FILE *FOPEN(const char *filename, const char *mode);

void LocalTime(char *stime, const char *fmt = 0, const int timetvl = 0);

// 把整数表示的时间转换为字符串表示的时间
// ltime：整数表示的时间
// stime：字符串表示的时间
// fmt：输出字符串时间stime的格式，与LocalTime函数的fmt参数相同，如果fmt的格式不正确，stime将为空
void timetostr(const time_t ltime, char *stime, const char *fmt = 0);

// 目录操作相关的类
// 根据绝对路径的文件名或目录名逐级的创建目录
// pathorfilename：绝对路径的文件名或目录名
// bisfilename：说明pathorfilename的类型，true-pathorfilename是文件名，否则是目录名，缺省值为true
// 返回值：true-成功，false-失败，如果返回失败，原因有大概有三种情况：1）权限不足；
// 2）pathorfilename参数不是合法的文件名或目录名；3）磁盘空间不足
bool MKDIR(const char *pathorfilename, bool bisfilename = true);

// RAII锁机制，使锁能够自动释放
class MutexLockGuard_LOG
{
public:
    explicit MutexLockGuard_LOG();
    ~MutexLockGuard_LOG();

private:
    static pthread_mutex_t lock;

private:
    MutexLockGuard_LOG(const MutexLockGuard_LOG&);
    MutexLockGuard_LOG& operator=(const MutexLockGuard_LOG&);
};

#endif