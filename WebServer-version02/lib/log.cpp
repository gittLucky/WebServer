// 日志操作
// 构造函数
#include "log.h"
#include "_cmpublic.h"

pthread_mutex_t MutexLockGuard_LOG::lock = PTHREAD_MUTEX_INITIALIZER;

CLogFile::CLogFile(const long MaxLogSize)
{
  m_tracefp = 0;
  memset(m_filename, 0, sizeof(m_filename));
  memset(m_openmode, 0, sizeof(m_openmode));
  m_bBackup = true;
  m_bEnBuffer = false;
  m_MaxLogSize = MaxLogSize;
  if (m_MaxLogSize < 10)
    m_MaxLogSize = 10;
}

// 析构函数
CLogFile::~CLogFile()
{
  Close();
}

// 关闭日志文件
void CLogFile::Close()
{
  if (m_tracefp != 0)
  {
    fclose(m_tracefp);
    m_tracefp = 0;
  }

  memset(m_filename, 0, sizeof(m_filename));
  memset(m_openmode, 0, sizeof(m_openmode));
  m_bBackup = true;
  m_bEnBuffer = false;
}

// 打开日志文件
// filename：日志文件名，建议采用绝对路径，如果文件名中的目录不存在，就先创建目录
// openmode：日志文件的打开方式，与fopen库函数打开文件的方式相同，缺省值是"a+"
// bBackup：是否自动切换，true-切换，false-不切换，在多进程的服务程序中，如果多个进行共用一个日志文件，bBackup必须为false
// bEnBuffer：是否启用文件缓冲机制，true-启用，false-不启用，
// 如果启用缓冲区，那么写进日志文件中的内容不会立即写入文件，缺省是不启用
bool CLogFile::Open(const char *filename, const char *openmode, bool bBackup, bool bEnBuffer)
{
  // 如果文件指针是打开的状态，先关闭它
  Close();

  strcpy(m_filename, filename);
  m_bEnBuffer = bEnBuffer;
  m_bBackup = bBackup;
  if (openmode == 0)
    strcpy(m_openmode, "a+");
  else
    strcpy(m_openmode, openmode);

  if ((m_tracefp = FOPEN(m_filename, m_openmode)) == 0)
    return false;

  return true;
}

// 如果日志文件大于100M，就把当前的日志文件备份成历史日志文件，切换成功后清空当前日志文件的内容
// 备份后的文件会在日志文件名后加上日期时间
// 注意，在多进程的程序中，日志文件不可切换，多线程的程序中，日志文件可以切换
bool CLogFile::BackupLogFile()
{
  if (m_tracefp == 0)
    return false;

  // 不备份
  if (m_bBackup == false)
    return true;

  // 定位文件指针，SEEK_END末尾开始，偏移量为0L
  fseek(m_tracefp, 0L, SEEK_END);

  // long int ftell(FILE *filename) 用来计算文件大小(字节)
  if (ftell(m_tracefp) > m_MaxLogSize * 1024 * 1024)
  {
    fclose(m_tracefp);
    m_tracefp = 0;

    char strLocalTime[21];
    memset(strLocalTime, 0, sizeof(strLocalTime));
    LocalTime(strLocalTime, "yyyymmddhh24miss");

    char bak_filename[301];
    memset(bak_filename, 0, sizeof(bak_filename));
    snprintf(bak_filename, 300, "%s.%s", m_filename, strLocalTime);
    // 重命名文件名字 旧名字，新名字，mv
    rename(m_filename, bak_filename);

    if ((m_tracefp = FOPEN(m_filename, m_openmode)) == 0)
      return false;
  }

  return true;
}

// 把内容写入日志文件，fmt是可变参数，使用方法与printf库函数相同
// Write方法会写入当前的时间，WriteEx方法不写时间
bool CLogFile::Write(const char *fmt, ...)
{
  if (m_tracefp == 0)
    return false;

  if (BackupLogFile() == false)
    return false;

  char strtime[20];
  LocalTime(strtime);

  // 指向当前参数列表的指针
  va_list ap;
  // 初始化指向参数列表第一个参数
  va_start(ap, fmt);
  fprintf(m_tracefp, "%s ", strtime);

  // 将格式化的字符串fmt输出到日志文件
  vfprintf(m_tracefp, fmt, ap);
  va_end(ap);

  if (m_bEnBuffer == false)
    fflush(m_tracefp);

  return true;
}

// 把内容写入日志文件，fmt是可变参数，使用方法与printf库函数相同
// Write方法会写入当前的时间，WriteEx方法不写时间
bool CLogFile::WriteEx(const char *fmt, ...)
{
  if (m_tracefp == 0)
    return false;

  va_list ap;
  va_start(ap, fmt);
  vfprintf(m_tracefp, fmt, ap);
  va_end(ap);

  if (m_bEnBuffer == false)
    fflush(m_tracefp);

  return true;
}

// 打开文件
// FOPEN函数调用fopen库函数打开文件，如果文件名中包含的目录不存在，就创建目录
// FOPEN函数的参数和返回值与fopen函数完全相同
// 在应用开发中，用FOPEN函数代替fopen库函数
FILE *FOPEN(const char *filename, const char *mode)
{
  if (MKDIR(filename) == false)
    return 0;

  return fopen(filename, mode);
}

/*
取操作系统的时间，并把整数表示的时间转换为字符串表示的格式
stime：用于存放获取到的时间字符串
timetvl：时间的偏移量，单位：秒，0是缺省值，表示当前时间，30表示当前时间30秒之后的时间点，-30表示当前时间30秒之前的时间点。
fmt：输出时间的格式，缺省是"yyyy-mm-dd hh24:mi:ss"，目前支持以下格式：
"yyyy-mm-dd hh24:mi:ss"，此格式是缺省格式。
"yyyymmddhh24miss"
"yyyy-mm-dd"
"yyyymmdd"
"hh24:mi:ss"
"hh24miss"
"hh24:mi"
"hh24mi"
"hh24"
"mi"
注意：
  1）小时的表示方法是hh24，不是hh，这么做的目的是为了保持与数据库的时间表示方法一致；
  2）以上列出了常用的时间格式，如果不能满足你应用开发的需求，请修改源代码增加更多的格式支持；
  3）调用函数的时候，如果fmt与上述格式都不匹配，stime的内容将为空。
*/
void LocalTime(char *stime, const char *fmt, const int timetvl)
{
  if (stime == 0)
    return;

  time_t timer;

  time(&timer);
  timer = timer + timetvl;

  timetostr(timer, stime, fmt);
}

// 把整数表示的时间转换为字符串表示的时间
// ltime：整数表示的时间
// stime：字符串表示的时间
// fmt：输出字符串时间stime的格式，与LocalTime函数的fmt参数相同，如果fmt的格式不正确，stime将为空
void timetostr(const time_t ltime, char *stime, const char *fmt)
{
  if (stime == 0)
    return;

  strcpy(stime, "");

  // 用ltime的值生成tm结构
  struct tm sttm = *localtime(&ltime);

  sttm.tm_year = sttm.tm_year + 1900; // 自1900年起的年份
  sttm.tm_mon++;                      // ++是因为范围是0-11

  if (fmt == 0)
  {
    snprintf(stime, 20, "%04u-%02u-%02u %02u:%02u:%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour,
             sttm.tm_min, sttm.tm_sec);
    return;
  }

  if (strcmp(fmt, "yyyy-mm-dd hh24:mi:ss") == 0)
  {
    snprintf(stime, 20, "%04u-%02u-%02u %02u:%02u:%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour,
             sttm.tm_min, sttm.tm_sec);
    return;
  }

  if (strcmp(fmt, "yyyy-mm-dd hh24:mi") == 0)
  {
    snprintf(stime, 17, "%04u-%02u-%02u %02u:%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour,
             sttm.tm_min);
    return;
  }

  if (strcmp(fmt, "yyyy-mm-dd hh24") == 0)
  {
    snprintf(stime, 14, "%04u-%02u-%02u %02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour);
    return;
  }

  if (strcmp(fmt, "yyyy-mm-dd") == 0)
  {
    snprintf(stime, 11, "%04u-%02u-%02u", sttm.tm_year, sttm.tm_mon, sttm.tm_mday);
    return;
  }

  if (strcmp(fmt, "yyyy-mm") == 0)
  {
    snprintf(stime, 8, "%04u-%02u", sttm.tm_year, sttm.tm_mon);
    return;
  }

  if (strcmp(fmt, "yyyymmddhh24miss") == 0)
  {
    snprintf(stime, 15, "%04u%02u%02u%02u%02u%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour,
             sttm.tm_min, sttm.tm_sec);
    return;
  }

  if (strcmp(fmt, "yyyymmddhh24mi") == 0)
  {
    snprintf(stime, 13, "%04u%02u%02u%02u%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour,
             sttm.tm_min);
    return;
  }

  if (strcmp(fmt, "yyyymmddhh24") == 0)
  {
    snprintf(stime, 11, "%04u%02u%02u%02u", sttm.tm_year,
             sttm.tm_mon, sttm.tm_mday, sttm.tm_hour);
    return;
  }

  if (strcmp(fmt, "yyyymmdd") == 0)
  {
    snprintf(stime, 9, "%04u%02u%02u", sttm.tm_year, sttm.tm_mon, sttm.tm_mday);
    return;
  }

  if (strcmp(fmt, "hh24miss") == 0)
  {
    snprintf(stime, 7, "%02u%02u%02u", sttm.tm_hour, sttm.tm_min, sttm.tm_sec);
    return;
  }

  if (strcmp(fmt, "hh24mi") == 0)
  {
    snprintf(stime, 5, "%02u%02u", sttm.tm_hour, sttm.tm_min);
    return;
  }

  if (strcmp(fmt, "hh24") == 0)
  {
    snprintf(stime, 3, "%02u", sttm.tm_hour);
    return;
  }

  if (strcmp(fmt, "mi") == 0)
  {
    snprintf(stime, 3, "%02u", sttm.tm_min);
    return;
  }
}

// 根据绝对路径的文件名或目录名逐级的创建目录
// pathorfilename：绝对路径的文件名或目录名
// bisfilename：说明pathorfilename的类型，true-pathorfilename是文件名，否则是目录名，缺省值为true
// 返回值：true-创建成功，false-创建失败，如果返回失败，原因有大概有三种情况：1）权限不足；
// 2）pathorfilename参数不是合法的文件名或目录名；3）磁盘空间不足
bool MKDIR(const char *filename, bool bisfilename)
{
  // 检查目录是否存在，如果不存在，逐级创建子目录
  char strPathName[301];

  int ilen = strlen(filename);

  for (int ii = 1; ii < ilen; ii++)
  {
    if (filename[ii] != '/')
      continue;

    memset(strPathName, 0, sizeof(strPathName));
    strncpy(strPathName, filename, ii);

    // 如果文件不存在，直接返回失败
    // access(const char *pathname, int mode)
    // 如果文件具有指定的访问权限，则函数返回0；如果文件不存在或者不能访问指定的权限，则返回-1
    // 00——只检查文件是否存在 02——写权限 04——读权限 06——读写权限
    if (access(strPathName, F_OK) == 0)
      continue;

    // mkdir只能一次创建一层目录
    // int mkdir (const char *__path, __mode_t __mode)
    // 以mode方式创建一个以pathname为名字的目录，mode定义所创建目录的权限
    // 一般mode参数设置为0755（可执行权限）
    // 0创建成功，-1失败
    if (mkdir(strPathName, 0755) != 0)
      return false;
  }

  if (bisfilename == false)
  {
    if (access(filename, F_OK) != 0)
    {
      if (mkdir(filename, 0755) != 0)
        return false;
    }
  }

  return true;
}

// 在构造函数中构造锁
MutexLockGuard_LOG::MutexLockGuard_LOG()
{
    pthread_mutex_lock(&lock);
}

// 在析构函数中释放锁
MutexLockGuard_LOG::~MutexLockGuard_LOG()
{
    pthread_mutex_unlock(&lock);
}