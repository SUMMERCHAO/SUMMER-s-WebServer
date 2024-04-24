#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

/*构造函数*/
connection_pool::connection_pool()
{
	this->CurConn = 0;
	this->FreeConn = 0;
}

/*获取数据库连接池的唯一实例*/
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

/*构造初始化*/
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
	/*初始化数据库连接*/
	this->url = url;
	this->Port = Port;
	this->User = User;
	this->PassWord = PassWord;
	this->DatabaseName = DBName;

	lock.lock();
	/*创建MaxConn条数据库连接*/
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;

		/*初始化MYSQL对象，若初始化失败，则输出错误信息*/
		con = mysql_init(con);
		if (con == NULL)
		{
			cout << "Error:" << mysql_error(con);
			exit(1);
		}

		/*使用mysql_real_connect函数来真正地连接数据库。传入的各个参数包括数据库地址，用户名，密码，数据库名称以及端口号等信息*/
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
		if (con == NULL)
		{
			cout << "Error: " << mysql_error(con);
			exit(1);
		}

		connList.push_back(con);	//连接成功的MYSQL对象会存储到连接池
		++FreeConn;			//空闲连接数FreeConn增加1
	}

	reserve = sem(FreeConn);	//将信号量初始化为最大连接次数

	this->MaxConn = FreeConn;
	
	lock.unlock();
}


/*获取数据库连接。当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数*/
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();	//取出连接，信号量原子减1，为0则等待
	
	lock.lock();

	con = connList.front();
	connList.pop_front();

	--FreeConn;
	++CurConn;

	lock.unlock();
	return con;
}

/*释放连接。释放当前使用的连接*/
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++FreeConn;
	--CurConn;

	lock.unlock();

	reserve.post();		//释放连接原子加1
	return true;
}

/*销毁所有连接,销毁数据库连接池*/
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		/*通过迭代器遍历，关闭数据库连接*/
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		CurConn = 0;
		FreeConn = 0;
		//清空list
		connList.clear();

		lock.unlock();
	}

	lock.unlock();
}

/*获取连接。当前空闲的连接数*/
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}