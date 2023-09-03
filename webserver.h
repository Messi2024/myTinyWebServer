#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符（HTTP对象数）
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 超时时间

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port, string user, string passWord, string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);
    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer* timer, int sockfd);
    bool dealclientdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    int m_port;       // 端口号
    char *m_root;     // 存放资源的根目录
    int m_log_write;  // 异步还是同步写入日志
    int m_close_log;  // 是否关闭日志
    int m_actormodel; // 并发模型选择（reactor/模拟proactor）

    int m_pipefd[2]; // 信号处理模块与主线程间通信的管道
    int m_epollfd;
    http_conn *users; // 指向http对象数组的指针

    // 数据库
    connection_pool *m_connPool; // 数据库连接池
    string m_user;               // 登录mysql用户名
    string m_passWord;           // 登陆密码
    string m_databaseName;       // 数据库名
    int m_sql_num;               // 数据库内连接数量

    // 线程池相关
    threadpool<http_conn> *m_pool; // 线程池
    int m_thread_num;              // 线程数量

    // epoll相关
    epoll_event events[MAX_EVENT_NUMBER]; // evetns数组

    int m_listenfd;
    int m_OPT_LINGER;     // 优雅关闭连接
    int m_TRIGMode;       // 组合触发模式
    int m_LISTENTrigmode; // listenfd触发模式
    int m_CONNTrigmode;   // connfd触发模式

    // 资源类数组
    client_data *users_timer;
    // 工具类
    Utils utils;
};
#endif