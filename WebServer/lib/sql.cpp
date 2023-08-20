#include "sql.h"

MysqlConn::MysqlConn()
{
	m_result = nullptr;
	m_mysqlRow = nullptr;
	// 传入nullptr空指针时，会自动分配一个MYSQL对象
	m_conn = mysql_init(nullptr);
}

MysqlConn::~MysqlConn()
{
	freeRes(); // 释放结果集
	if (m_conn != nullptr)
	{
		mysql_close(m_conn);
		m_conn = nullptr;
	}
}

void MysqlConn::freeRes()
{
	if (m_result)
	{
		mysql_free_result(m_result);
		m_result = nullptr;
	}
}

bool MysqlConn::connect(const std::string user, const std::string passwd,
						const std::string dbName, std::string ip,
						const unsigned short port)
{
	// 倒数第二个参数表示不使用socket或者管道机制，最后一个参数常设为0
	// 端口号设置为0会默认连接MySQL 3306端口
	MYSQL *res = mysql_real_connect(m_conn, ip.c_str(), user.c_str(),
									passwd.c_str(), dbName.c_str(), port, nullptr, 0);
	if (res == NULL)
	{
		fprintf(stderr, "Failed to connect to database: Error: %s\n", mysql_error(m_conn));
		return false;
	}
	// 修改编码
	mysql_set_character_set(m_conn, "utf8");
	return res != nullptr;
}

bool MysqlConn::update(const std::string sql) const
{
	// 执行sql语句
	int res = mysql_real_query(m_conn, sql.c_str(), static_cast<unsigned int>(sql.size()));
	if (res != 0)
	{
		return false; // 提示
	}
	return true;
}

bool MysqlConn::query(const std::string sql)
{
	freeRes();
	int res = mysql_real_query(m_conn, sql.c_str(), static_cast<unsigned int>(sql.size()));
	if (res != 0)
	{
		return false; // 提示
	}
	m_result = mysql_store_result(m_conn);
	return true;
}

bool MysqlConn::getRes()
{
	if (m_result != nullptr)
	{
		// char** 获取单行记录
		// 相当于返回字符串数组
		m_mysqlRow = mysql_fetch_row(m_result);
		if (m_mysqlRow != nullptr)
		{
			return true;
		}
		freeRes();
	}
	return false;
}

std::string MysqlConn::getValue(const int fieldIndex) const
{
	// 返回结果表列数
	int fieldCount = mysql_num_fields(m_result);
	if (fieldIndex >= fieldCount || fieldIndex < 0)
	{
		return std::string(); // 提示
	}
	char *value = m_mysqlRow[fieldIndex];
	// 得到一个保存各字段值长度的数组
	unsigned long *len = mysql_fetch_lengths(m_result);
	unsigned long length = len[fieldIndex];
	// 防止结果中存在\0导致数据丢失
	return std::string(value, length);
}

bool MysqlConn::selectDB(const std::string dbName) const
{
	int res = mysql_select_db(m_conn, dbName.c_str());
	if (res != 0)
	{
		return false; // 提示
	}
	return true;
}

bool MysqlConn::createDB(const std::string dbName) const
{
	std::string sql = "create database " + dbName + ";";
	int res = mysql_real_query(m_conn, sql.c_str(), static_cast<unsigned long>(sql.size()));
	if (res != 0)
	{
		return false; // 提示
	}
	return true;
}

// 遍历数据库中的表
void MysqlConn::backupCurrentDB(const std::string path)
{
	std::string sql = "show tables";
	int r = mysql_real_query(m_conn, sql.c_str(), static_cast<unsigned long>(sql.size()));
	if (r != 0)
	{
		return; // 提示
	}
	MYSQL_RES *tableRes = mysql_store_result(m_conn);
	// 返回结果的行数
	for (int i = 0; i < mysql_num_rows(tableRes); ++i)
	{
		MYSQL_ROW tableName = mysql_fetch_row(tableRes);
		backupCurrentTable(path, tableName[0]);
	}
}

// 导出表结构以及表数据
void MysqlConn::backupCurrentTable(const std::string path, const std::string tableName)
{
	std::string file = path + tableName + ".sql";
	ofstream ofs(file);
	if (!ofs.is_open())
	{
		return; // 提示
	}
	// 表结构写入
	// 显示创建表的sql语句
	std::string showCreate = "show create table " + tableName + ";";
	bool res = query(showCreate);
	if (!res)
	{
		return; // 提示
	}
	if (getRes())
	{
		// 结果包含两列 "Table" 和 "Create Table" "Create Table" 列将包含用于创建指定表格的SQL语句
		std::string writeSQL = getValue(1) + ";\n";
		ofs.write(writeSQL.c_str(), writeSQL.size());
		// cout << writeSQL << endl;
	}
	// 表数据写入
	std::string sql = "select * from " + tableName + ";";
	res = query(sql);
	if (!res)
	{
		return; // 提示
	}
	while (getRes())
	{
		std::string writeSQL = "insert into `" + tableName + "` values(";
		for (int i = 0; !getValue(i).empty(); ++i)
		{
			if (i != 0)
			{
				writeSQL += ",";
			}
			// 获取列的信息（类型，名称，长度等），i表示第几列
			MYSQL_FIELD *valueType = mysql_fetch_field_direct(m_result, i);
			if (valueType->type == MYSQL_TYPE_DECIMAL || valueType->type == MYSQL_TYPE_TINY // C语言char
				|| valueType->type == MYSQL_TYPE_SHORT										// C语言short int
				|| valueType->type == MYSQL_TYPE_LONG										// C语言int
				|| valueType->type == MYSQL_TYPE_FLOAT										// C语言float
				|| valueType->type == MYSQL_TYPE_DOUBLE										// C语言double
				|| valueType->type == MYSQL_TYPE_TIMESTAMP									// MYSQL_TIME
				|| valueType->type == MYSQL_TYPE_LONGLONG									// C语言log long int
				|| valueType->type == MYSQL_TYPE_INT24)
			{
				writeSQL += getValue(i);
			}
			else
			{
				writeSQL += "'" + getValue(i) + "'";
			}
		}
		writeSQL += ");\n";
		ofs.write(writeSQL.c_str(), writeSQL.size());
	}
	ofs.close();
}

// 开启事务
bool MysqlConn::transaction() const
{
	// 将事务提交设置为手动提交
	return mysql_autocommit(m_conn, false);
}

// 提交事务
bool MysqlConn::commit() const
{
	return mysql_commit(m_conn);
}

// 事务回滚
bool MysqlConn::rollback() const
{
	return mysql_rollback(m_conn);
}

// 刷新起始空闲时间点
void MysqlConn::refreashAliveTime()
{
	m_alivetime = steady_clock::now();
}

// 计算存活总时长
ll MysqlConn::getAliveTime()
{
	// 毫秒
	milliseconds res = duration_cast<milliseconds>(steady_clock::now() - m_alivetime);
	return res.count();
}
