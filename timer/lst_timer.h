#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

// 资源类需要用到定时器类，所以前向声明一下
class util_timer;

// 连接资源类,webserver有一个数组用于存放这个类
struct client_data
{
    // 客户端socket地址
    sockaddr_in address;

    // socket文件描述符
    int sockfd;

    // 定时器
    util_timer *timer;
};

// 定时器类
class util_timer
{
public:
    util_timer() : prev(nullptr), next(nullptr) {}

    // 回调函数，用来处理超时连接，在设置定时器的时候可以设置该函数的行为（通过给这个函数指针赋值）
    void (*cb_func)(client_data *);

public:
    // 超时时间
    time_t expire;
    // 前向定时器
    util_timer *prev;
    // 后继定时器
    util_timer *next;
    // 指向与该定时器对应的连接资源
    client_data *user_data;
};

// 定时器容器类，带头尾节点的升序双向链表
class sort_timer_lst
{
public:
    // 构造函数
    sort_timer_lst();
    // 析构函数，销毁链表
    ~sort_timer_lst();

    // 插入定时器公有方法，内部跟头节点超时时间对比，如果小于头节点超时时间，则直接插在头节点前，否则调用私有方法add_timer( , )找到合适位置插入
    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head; // 指向头节点（即将到期的）
    util_timer *tail; // 指向尾节点（最后到期的）
};

// 工具类， 里面包含定时器、信号处理模块， 可以向内核注册fd、设置fd、添加信号等
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    // 初始化,设置超时时间
    void init(int timeslot);

    // 设置fd为非阻塞（用在传输信号时的写端）
    int setnonblocking(int fd);

    // 向内核事件表注册fd，根据one_shot选择是否开启EPOLLONESHOT， 根据trigmode选择开启ET模式
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数,当捕获到信号后，会调用这个函数（通过sigaction函数设置的），
    // 这个函数会通过管道传递给主循环，主循环通过epoll事件表监听管道另一端得到信号
    static void sig_handler(int sig);

    // 添加信号sig并设置触发信号后的信号处理函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时来不断触发SIGALARM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;       // 用来传输信号的双向管道
    sort_timer_lst m_timer_lst; // 存放定时器的链表
    static int u_epollfd;
    int m_TIMESLOT; // 超时时间
};

//全局函数在头文件中声明一下，这样包含这个头文件的文件就能直接使用而不用使用extern
void cb_func(client_data *user_data);

#endif