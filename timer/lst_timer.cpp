#include "lst_timer.h"
#include "../http/http_conn.h"

// 定时器容器构造函数
sort_timer_lst::sort_timer_lst()
{
    head = nullptr;
    tail = nullptr;
}

// 定时容器析构
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 添加定时器公有方法
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 如果超时时间最短，直接放在头节点前面成为新的头节点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }

    // 否则调用私有成员函数add，寻找合适位置插入
    add_timer(timer, head);
}

// 调整定时器，任务发生变化时，调整定时器在链表中的位置
// 一般是有数据要传输，将定时器往后延迟3个单位（定义在webserver中的函数，会调整定时器的超时值。然后调用这个函数来调整位置）
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    util_timer *tmp = timer->next;
    // 被调整的定时器在链表尾部或定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    // 如果被调整的定时器是头部，重新设置头部，然后取出定时器重新插入，不是头部直接取出插入
    if (timer == head)
    {
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

//删除定时器
void sort_timer_lst::del_timer(util_timer* timer)
{
    if(!timer)
    {
        return;
    }

    //链表中只有一个定时器
    if(timer == head && timer == tail)
    {
        delete timer;
        head = nullptr;
        tail == nullptr;
        return;
    }

    //被删除的节点为头节点
    if(timer == head)
    {
        head = head->next;
        head->prev = nullptr;
        delete timer;
        return;
    }

    //被删除的节点为尾节点
    if(timer == tail)
    {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    //被删除的节点在内部
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;

}

// 定时信号处理函数dealwithsignal函数收到SIGALARM信号就把主循环中的timeout置为true
// 然后主循环就会调用这个函数找到那些超时的定时器
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    // 获取当前时间
    time_t cur = time(nullptr);
    util_timer *tmp = head;

    // 遍历定时器链表
    while (tmp)
    {
        // 定时器链表升序，前面的先到期
        if (cur < tmp->expire)
        {
            break;
        }

        // 对到期的定时器调用相应的超时处理函数
        tmp->cb_func(tmp->user_data);

        // 处理后的定时器删除掉并重置头节点
        head = tmp->next;
        if (head)
        {
            head->prev = nullptr;
        }
        delete tmp;
        tmp = head;
    }
}

// 调整定时器位置的私有成员，被公有成员add_timer和adjust_time调用
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }

    //遍历完链表也没有插入，说明目标定时器超时时间最大
    if(!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}



void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//向内核事件表注册fd，根据one_shot选择是否开启EPOLLONESHOT， 根据trigmode选择开启ET模式
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(TRIGMode == 1)
    {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else
    {
        event.events = EPOLLIN | EPOLLRDHUP;
    }
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);   
}

//信号处理函数,当捕获到信号后，会调用这个函数（通过sigaction函数设置的）， 
//这个函数会通过管道传递给主循环，主循环通过epoll事件表监听管道另一端得到信号
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    //将信号从管道写端写入，传输的字符类型，而不是整形
    send(u_pipefd[1], (char*)&msg, 1, 0);

    //将原来的errno赋值给档期的errno
    errno = save_errno;
}

//添加关注的信号sig，并设置捕获到信号后如何处理
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    //信号处理函数发送信号到管道中，逻辑处理交给主循环
    sa.sa_handler = handler;

    //SA_RESTART, 使被信号打断的系统调用自动重新发起
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }

    //将所有信号添加到信号掩码中，即处理信号时屏蔽其他信号
    sigfillset(&sa.sa_mask);

    //sigaction函数，sig参数指出要捕获的信号，第二个参数指出新的处理方式（里面包含信号掩码（正在处理信号时屏蔽的信号，这里全部屏蔽）和信号处理函数）
    assert(sigaction(sig, &sa, NULL) != -1);    
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    //tick函数处理超时连接
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

//向用户发送错误原因，并关闭连接描述符
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//静态成员要在类外定义以及初始化，类内只是声明，并未分配内存。
//静态成员是单独存储的，并不是对象的组成部分。如果在类的内部进行定义，在建立多个对象时会多次声明和定义该变量的存储位置。
//在名字空间和作用于相同的情况下会导致重名的问题。
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

//定时器回调函数，删除过期连接
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    //关闭文件描述符
    close(user_data->sockfd);

    //减少连接数
    http_conn::m_user_count--;
}