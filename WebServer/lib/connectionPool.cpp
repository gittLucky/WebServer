#include "connectionPool.h"
#include <unistd.h>
#include "../tinyxml/include/tinyxml.h"

// 初始化互斥锁与条件变量
pthread_mutex_t ConnectionPool::lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ConnectionPool::empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t ConnectionPool::not_empty = PTHREAD_COND_INITIALIZER;

std::queue<MysqlConn *> ConnectionPool::m_connections;
std::string ConnectionPool::m_ip;
unsigned short ConnectionPool::m_port; // 端口
std::string ConnectionPool::m_user;         // 用户名
std::string ConnectionPool::m_passwd;       // 密码
std::string ConnectionPool::m_dbName;       // 数据库名称
int ConnectionPool::m_minSize;         // 初始连接量(最小连接量)
int ConnectionPool::m_maxSize;         // 最大连接量
int ConnectionPool::m_maxIdleTime;     // 最大空闲时长

atomic_int ConnectionPool::m_num; // 连接的总数量
pthread_t ConnectionPool::conn_thread;  // 负责连接的线程
pthread_t ConnectionPool::destroy_thread;  // 负责清除连接的线程
int ConnectionPool::shutdown; /* 标志位，数据库连接池使用状态，true或false */

int ConnectionPool::sqlConnectionPoolCreate()
{
    if (!parseXmlFile())
        return -1;
    for (m_num = 0; m_num < m_minSize;)
    {
        bool flag = addConnection();
        if (!flag)
        {
            return -1;
        }
    }
    // 设置线程为分离状态
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // 如果子线程的任务函数是类的非静态函数，我们需要指定任务函数的地址和任务函数的所有者
    // 创建连接线程
    if (pthread_create(&conn_thread, &attr, productConnection, (void *)(0)) != 0)
    {
        return -1;
    }
    // 创建销毁线程
    if (pthread_create(&destroy_thread, &attr, recycleConnection, (void *)(0)) != 0)
    {
        return -1;
    }
    return 0;
}

void ConnectionPool::sqlConnectionPoolDestroy()
{
    pthread_mutex_lock(&lock);
    shutdown = 1;
    pthread_mutex_unlock(&lock);
    /*通知等待的连接线程*/
    pthread_cond_broadcast(&not_empty);
    // 等待连接线程结束
    pthread_join(conn_thread, NULL);
    // 等待销毁线程结束
    pthread_join(destroy_thread, NULL);
    while (!m_connections.empty())
    {
        MysqlConn *conn = m_connections.front();
        m_connections.pop();
        delete conn;
    }
}

bool ConnectionPool::parseXmlFile()
{
    TiXmlDocument xml("../doc/mysql.xml");
    // 加载文件
    bool res = xml.LoadFile();
    if (!res)
    {
        return false; // 提示
    }
    // 根
    TiXmlElement *rootElement = xml.RootElement();
    TiXmlElement *childElement = rootElement->FirstChildElement("mysql");
    // 读取信息
    m_ip = childElement->FirstChildElement("ip")->GetText();
    m_port = static_cast<unsigned short>(stoi(string(childElement->FirstChildElement("port")->GetText())));
    m_user = childElement->FirstChildElement("username")->GetText();
    m_passwd = childElement->FirstChildElement("password")->GetText();
    m_dbName = childElement->FirstChildElement("dbName")->GetText();
    m_minSize = static_cast<int>(stoi(string(childElement->FirstChildElement("minSize")->GetText())));
    m_maxSize = static_cast<int>(stoi(string(childElement->FirstChildElement("maxSize")->GetText())));
    m_maxIdleTime = static_cast<int>(stoi(string(childElement->FirstChildElement("maxIdleTime")->GetText())));
    shutdown = static_cast<int>(stoi(string(childElement->FirstChildElement("shutdown")->GetText())));
    return true;
}

bool ConnectionPool::addConnection()
{
    if(!shutdown)
    {
        MysqlConn *conn = new MysqlConn;
        bool res = conn->connect(m_user, m_passwd, m_dbName, m_ip, m_port);
        if (res)
        {
            // 刷新空闲时间
            conn->refreashAliveTime();
            m_connections.push(conn);
            ++m_num;
            return true;
        }
        else
        {
            delete conn;
            return false; // 提示
        }
    }
}

void *ConnectionPool::productConnection(void *args)
{
    while (true)
    {
        // 休眠一段时间 0.5s
        sleep(500);
        if (pthread_mutex_lock(&lock) != 0)
        {
            break;
        }
        while (!m_connections.empty() && !shutdown)
        {
            if (pthread_cond_wait(&not_empty, &lock) != 0)
            {
                return NULL;
            }
        }
        if (shutdown)
        {
            if (pthread_mutex_unlock(&lock) != 0)
            {
                return NULL;
            }
            return NULL;
        }
        if (m_num < m_maxSize)
        {
            bool flag = addConnection();
            if (!flag)
            {
                break;
            }
        }
        // 唤醒
        if ((pthread_cond_broadcast(&empty) != 0) || (pthread_mutex_unlock(&lock) != 0))
        {
            break;
        }
    }
    pthread_exit(NULL);
}

// 清除超时的连接
void *ConnectionPool::recycleConnection(void *args)
{
    while (true)
    {
        // 休眠一段时间 0.5s
        sleep(500);
        if (pthread_mutex_lock(&lock) != 0)
        {
            break;
        }
        while (!m_connections.empty() && m_num > m_minSize && !shutdown)
        {
            MysqlConn *conn = m_connections.front();
            // ms单位，检查空闲的连接是否超时
            if (conn->getAliveTime() >= m_maxIdleTime)
            {
                m_connections.pop();
                delete conn;
                --m_num;
            }
            else
            {
                break;
            }
        }
        if (pthread_mutex_unlock(&lock) != 0)
        {
            break;
        }
        if(shutdown)
        {
            break;
        }
    }
    pthread_exit(NULL);
}

shared_ptr<MysqlConn> ConnectionPool::getConnection()
{
    if (pthread_mutex_lock(&lock) != 0)
    {
        return nullptr;
    }
    while (m_connections.empty() && !shutdown)
    {
        if (pthread_cond_wait(&empty, &lock) != 0)
        {
            return nullptr;
        }
    }
    if (shutdown)
    {
        if (pthread_mutex_unlock(&lock) != 0)
        {
            return NULL;
        }
        return NULL;
    }
    // 要指定删除器destructor，来保证连接的归还
    shared_ptr<MysqlConn> conn(m_connections.front(), [](MysqlConn *conn)
                               {
                                   // 加锁保证队列线程安全
                                    pthread_mutex_lock(&lock);
                                    conn->refreashAliveTime();
                                    m_connections.push(conn);
                                    pthread_mutex_unlock(&lock);
                               });
    m_connections.pop();
    // 唤醒
    if ((pthread_cond_broadcast(&not_empty) != 0) || (pthread_mutex_unlock(&lock) != 0))
    {
        return nullptr;
    }
    return conn;
}
