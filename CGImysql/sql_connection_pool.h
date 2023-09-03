#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
    MYSQL *GetConnection();              // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn); // 释放连接
    int GetFreeConn();                   // 获取空闲连接数
    void DestroyPool();                  // 销毁所有连接

    // 单例模式,程序运行期间只会存在一个数据库对象
    static connection_pool *GetInstance();

    // 初始化数据库
    void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);

private:
    connection_pool();      //构造、析构放在私有里，保证只有一个对象
    ~connection_pool();
    int m_MaxConn;          // 最大连接数
    int m_CurConn;          // 当前已使用的连接数
    int m_FreeConn;         // 空闲的连接数
    locker lock;            // 互斥锁
    list<MYSQL *> connList; // 连接池
    sem reserve;            // 信号量

public:
    string m_url;          // 主机地址
    string m_Port;         // 端口号
    string m_User;         // 数据库登陆用户名
    string m_PassWord;     // 数据库密码
    string m_DatabaseName; // 使用的数据库名
    int m_close_log;       // 日志是否打开，没用
};

class connectionRAII
{
public:
    //数据库连接本身是一个指针，所以这里想要在内部修改它的话，要传入二级指针才能修改
    //通过有参构造，在构造函数内对参数（这里是数据库连接）进行修改
    connectionRAII(MYSQL** con, connection_pool* connPool);
    ~connectionRAII();
private:
    MYSQL* conRAII;
    connection_pool* poolRAII;
};
#endif