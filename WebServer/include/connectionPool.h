#pragma once
#include <queue>
#include <pthread.h>
#include <atomic>
#include <string>
#include "sql.h"

#if 0
// 应用-单例模式：懒汉模式[需要考虑多线程安全问题]
class ConnectionPool
{
private:
    ConnectionPool();
    // 移动拷贝最终还是有且仅有一个对象，所以依旧是属于单例模式
    // delete 阻止拷贝构造和拷贝赋值的类对象生成
    ConnectionPool(ConnectionPool &) = delete;
    ConnectionPool &operator=(ConnectionPool &) = delete;
    ~ConnectionPool();
    // 解析xml配置文件 读取数据库及连接池的相关信息
    bool parseXmlFile();
    // 添加连接
    bool addConnection();
    // 线程处理函数
    void *productConnection(void *args);
    void *recycleConnection(void *args);

    // 存放数据库连接池建立的连接
    queue<MysqlConn *> m_connections;
    // 一些基本信息
    std::string m_ip;           // IP
    unsigned short m_port; // 端口
    std::string m_user;         // 用户名
    std::string m_passwd;       // 密码
    std::string m_dbName;       // 数据库名称
    int m_minSize;         // 初始连接量(最小连接量)
    int m_maxSize;         // 最大连接量
    int m_maxIdleTime;     // 最大空闲时长
    // 线程安全相关
    pthread_mutex_t lock;
    pthread_cond_t empty;
    pthread_cond_t not_empty;
    // 连接数量
    // atomic_int线程安全的int
    atomic_int m_num; // 连接的总数量
    pthread_t conn_thread;  // 负责连接的线程
    pthread_t destroy_thread;  // 负责清除连接的线程
    int shutdown; /* 标志位，数据库连接池使用状态，true或false */
public:
    // 获取单例对象的接口
    static ConnectionPool *getConnectPool();
    // 用户获取连接的接口, 如果获取失败，会返回nullptr
    shared_ptr<MysqlConn> getConnection();
};

#endif

// 由于线程处理函数没法是类的成员函数，所以全部定义为static
class ConnectionPool
{
private:
    // 解析xml配置文件 读取数据库及连接池的相关信息
    static bool parseXmlFile();
    // 添加连接
    static bool addConnection();
    // 线程处理函数
    static void *productConnection(void *args);
    static void *recycleConnection(void *args);

    // 存放数据库连接池建立的连接
    static std::queue<MysqlConn *> m_connections;
    // 一些基本信息
    static std::string m_ip;           // IP
    static unsigned short m_port; // 端口
    static std::string m_user;         // 用户名
    static std::string m_passwd;       // 密码
    static std::string m_dbName;       // 数据库名称
    static int m_minSize;         // 初始连接量(最小连接量)
    static int m_maxSize;         // 最大连接量
    static int m_maxIdleTime;     // 最大空闲时长
    // 线程安全相关
    static pthread_mutex_t lock;
    static pthread_cond_t empty;
    static pthread_cond_t not_empty;
    // 连接数量
    // atomic_int线程安全的int
    static atomic_int m_num; // 连接的总数量
    static pthread_t conn_thread;  // 负责连接的线程
    static pthread_t destroy_thread;  // 负责清除连接的线程
    static int shutdown; /* 标志位，数据库连接池使用状态，true或false */
public:
    static int sqlConnectionPoolCreate();
    // 用户获取连接的接口, 如果获取失败，会返回nullptr
    static shared_ptr<MysqlConn> getConnection();
    static void sqlConnectionPoolDestroy();
};