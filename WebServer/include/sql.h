#pragma once
#include <iostream>
#include <memory>
#include <string>
#include <chrono> // 时钟
#include <fstream>
#include "../mysql/mysql/include/mysql.h"

using namespace std;
using namespace chrono;
typedef long long ll;

class MysqlConn
{
private:
    // 绝对时钟
    steady_clock::time_point m_alivetime;
    // 连接
    MYSQL *m_conn;
    // 查询的结果集
    MYSQL_RES *m_result;
    // 单记录结果集
    MYSQL_ROW m_mysqlRow;

    // 结果集释放
    void freeRes();
    // 导出某一张表中的数据
    void backupCurrentTable(const string path, const string tableName);

public:
    // 初始化数据库
    MysqlConn();
    // 数据库连接释放
    ~MysqlConn();
    // 连接数据库, 需提供用户 密码 数据库名称 ip 端口
    bool connect(const string user, const string passwd,
                 const string dbName, string ip,
                 const unsigned short port = 3306U);
    // 更新数据库:增删改操作
    bool update(const string sql) const;
    // 查询数据库
    bool query(const string sql);
    // 遍历查询结果集
    bool getRes();
    // 获取结果集中的字段值
    string getValue(const int fieldIndex) const;
    // 切换数据库
    bool selectDB(const string dbName) const;
    // 建库
    bool createDB(const string dbName) const;
    // 备份某个库
    void backupCurrentDB(const string path);
    // 事务操作
    bool transaction() const;
    // 提交事务
    bool commit() const;
    // 事务回滚
    bool rollback() const;
    // 刷新起始的空闲时间点
    void refreashAliveTime();
    // 计算存活总时长
    ll getAliveTime();
};
