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

// 构造函数
connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

// 单例模式,程序运行期间只会存在一个数据库对象
connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

// 初始化数据库连接池
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    // 初始化数据库连接池信息
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    // 创建MaxConn条数据库连接
    for (int i = 0; i < MaxConn; i++)
    {
        MYSQL *con = nullptr;
        // 使用mysql_init初始化连接
        con = mysql_init(con);

        if (con == nullptr)
        {
            LOG_ERROR("MYSQL Error");
            exit(1);
        }

        // 使用mysql_real_connect建立到数据库的连接
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, nullptr, 0);

        if (con == nullptr)
        {
            LOG_ERROR("MYSQL Error");
            exit(1);
        }
        // 更新连接池和空闲连接数量
        connList.push_back(con);
        ++m_FreeConn;
    }

    // 将信号量初始化为最大连接数
    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}
// 当有需要时，返回一个数据库连接，同时更新空闲和使用的连接数以及连接池
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = nullptr;

    if (0 == connList.size())
    {
        return nullptr;
    }

    // 操作信号量，如果为0则需要等待变为非0值,wait操作给信号量减一
    reserve.wait();

    // 防止操作同一个连接， 加锁
    lock.lock();

    con = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return con;
}

// 释放连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (con == nullptr)
    {
        return false;
    }
    lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    // 释放连接信号量原子加一
    reserve.post();
    return true;
}

// 获取空闲连接数
int connection_pool::GetFreeConn()
{
    return this->m_FreeConn;
}

 // 销毁所有连接
void connection_pool::DestroyPool()
{
    lock.lock();
    if(connList.size() > 0)
    {
        //迭代器遍历，关闭数据库连接
        list<MYSQL*>::iterator it;
        for(it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL* con = *it;

            //使用mysql_close关闭连接
            mysql_close(con);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

//析构，摧毁连接池
connection_pool::~connection_pool()
{
    DestroyPool();
}

//RAII
//在构造时获取连接
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool* connPool)
{
    *SQL = connPool->GetConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}
//在析构时释放连接
connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}