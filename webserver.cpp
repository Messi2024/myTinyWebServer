#include "webserver.h"

// 构造，构造65536个http对象和定时器
WebServer::WebServer()
{
    // http对象
    users = new http_conn[MAX_FD];

    // m_root中存放root文件夹路径（服务器资源）
    char server_path[200];
    // getcwd可以获取当前项目运行的地址
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 每个HTTP对象对应的对象数据，包含地址、文件描述符，指向定时器的指针（刚开始为空）
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);   // 关闭epollfd
    close(m_listenfd);  // 关闭listenfd
    close(m_pipefd[1]); // 关闭管道
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

// 初始化
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

// 设定触发模式
void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 根据是否关闭日志来选择是否初始化日志，并根据传入参数选择写入日志是异步还是同步
void WebServer::log_write()
{
    if (m_close_log == 0)
    {
        if (1 == m_log_write)
        {
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        }
        else
        {
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}

// 获取数据库实例，初始化数据库连接池
void WebServer::sql_pool()
{
    // 单例模式，为webserver获取数据库实例
    m_connPool = connection_pool::GetInstance();
    // 初始化数据库数据库主机名是本地"localhost"，端口3306
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 将数据库中数据取到本地存在map中
    users->initmysql_result(m_connPool);
}

// 创建线程池，运行线程函数
void WebServer::thread_pool()
{
    // 调用构造函数
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 开始监听，创建内核事件表，创建管道用于发送信号，设定想要捕捉的信号，触发定时事件
void WebServer::eventListen()
{
    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);
    // 优雅关闭连接，此选项指定函数close对面向连接的协议如何操作（如TCP）
    // 给setsocket传递一个linger结构体，里面有两个成员，一个l_onoff，一个l_linger
    // 设置 l_onoff为0，则该选项关闭，l_linger的值被忽略，close用默认行为关闭socket，即close调用会立即返回给调用者，TCP模块负责把该sokcey对应的TCP
    // 发送缓冲区中残留的数据发送给对方
    // 设置 l_onoff为非0，l_linger为0，则套接口关闭时TCP夭折连接，TCP将丢弃保留在套接口发送缓冲区中的任何数据并发送一个RST给对方
    // 设置 l_onoff 为非0，l_linger为非0，将根据socket是阻塞还是非阻塞，缓冲区内还有没有数据来决定行为。
    // 如果是阻塞的，将等待l_linger长的时间直到TCP模块发送完所有数据并得到对方确认，如果没发送完返回-1并设置errno为EWOLUDBLOCK
    // 如果是非阻塞的，close立即返回，并根据其返回值和errno来判断残留数据是否已经发送完毕
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 创建监听socket的地址结构体用于绑定到m_listenfd上
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    // INADDR_ANY是0.0.0.0，这里表示把套接字绑定到服务器所有的可用接口（一个服务器可能有多个IP），表示监听所有网络
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // 允许端口被重复使用，可用于服务器快速重启
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 绑定
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    // 开始监听，第二个参数表示全连接队列数量，指未被accpet取走的
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 初始化连接超时事件
    utils.init(TIMESLOT);

    // 创建epoll内核事件表
    epoll_event events[MAX_EVENT_NUMBER]; // 当有事件发生时，就存放在这里（这个不是内核事件表）
    m_epollfd = epoll_create(5);          // 参数被忽略，只需要大于0， m_epollfd唯一标识一个内核事件表
    assert(m_epollfd != -1);

    // 把listenfd注册到m_epollfd标识的内核事件表中
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    // 把m_pipefd创建为管道，用于传输信号(超时)
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    // 设置管道写端为非阻塞，为什么写端要非阻塞？
    // send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其设定为非阻塞。
    utils.setnonblocking(m_pipefd[1]);
    // 将管道读端设置为ET非阻塞，放入内核事件表中
    utils.addfd(m_epollfd, m_pipefd[0], false, 1);

    // 在linux下写socket的程序的时候，若是尝试send到一个disconnected socket上，就会让底层抛出一个SIGPIPE信号。
    // 这个信号的缺省处理方法是退出进程，我们不希望这样，因此给它传一个新的信号处理方法
    // SIG_IGN为忽略该信号,SIGPIPE信号的交付对线程没有影响
    utils.addsig(SIGPIPE, SIG_IGN);

    // 捕捉SIGALARM和SIGTERM两个信号，传入的sig_handler信号处理函数作用是通过管道发送给主循环
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);
    // 开始第一次定时触发SIG_ALARM信号，之后会通过超时处理函数不断重新设置定时信号
    alarm(TIMESLOT);

    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 初始化该连接的连接资源，将对应文件描述符注册到内核事件表，初始化连接对应的定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    // 初始化该连接对应的HTTP对象，将connfd注册到内核事件表中
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 将用户地址和该连接的sockfd绑定到该连接对应的用户数据类上
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    // 创建连接对应的定时器
    util_timer *timer = new util_timer;
    // 设置定时器对应的连接资源
    timer->user_data = &users_timer[connfd];
    // 设置超时定时器的回调函数
    timer->cb_func = cb_func;
    // 获取当前时间并设定超时时间为三倍的TIMESLOT;
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;
    // 让用户数据中的定时器指针指向创建的定时器
    users_timer[connfd].timer = timer;
    // 将定时器加入到链表中
    utils.m_timer_lst.add_timer(timer);
}

// 该连接有数据接收或发送，说明是活跃的，调整它对应的定时器的超时时间
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

// 处理异常事件，从sock缓冲区中读写失败或超时或客户端发生异常调用这个关闭连接
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    // 删除内核事件表上该socket，关闭连接，减少计数
    timer->cb_func(&users_timer[sockfd]);
    // 在链表上移除该定时器
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 处理客户连接
bool WebServer::dealclientdata()
{
    // 初始化客户端连接地址
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    // 根据listenfd触发模式，选择一次处理完请求还是一次处理一个
    // LT模式，一次处理一个
    if (m_LISTENTrigmode == 0)
    {
        // 调用accept接受连接
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        // 连接失败
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is :%d", "accept error", errno);
            return false;
        }
        // 连接用户数量超出上限
        if (http_conn::m_user_count >= MAX_FD)
        {
            // 向用户发送错误原因，并关闭连接
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        // 连接成功，connfd作为下标初始化该连接的连接资源，将对应文件描述符注册到内核事件表，初始化连接对应的定时器
        timer(connfd, client_address);
    }
    else
    {
        // ET模式，区别就是利用循环一次处理完
        while (1)
        {
            // 调用accept接受连接
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                // 连接失败
                LOG_ERROR("%s:errno is :%d", "accept error", errno);
                break;
            }
            // 连接用户数量超出上限
            if (http_conn::m_user_count >= MAX_FD)
            {
                // 向用户发送错误原因，并关闭连接
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            // 连接成功，connfd作为下标初始化该连接的连接资源，将对应文件描述符注册到内核事件表，初始化连接对应的定时器
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 处理信号,根据信号修改timeout（是否超时）和stop_server（是否关闭连接）
// 主循环会根据这两个值进行相应的操作
// timeout为真，主循环调用time_handler处理超时连接并重设超时
// stopserver为真，主循环停止
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];

    // 从管道读端读出信号放入signals里，成功返回读取字节数，失败返回-1
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);

    if (ret == -1)
    {
        // 处理错误
        return false;
    }
    else if (ret == 0)
    {
        // 处理错误
        return false;
    }
    else
    {
        // 处理信号值对应的逻辑
        for (int i = 0; i < ret; i++)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

// 处理可读事件
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    if (m_actormodel == 1) // reactor模式，工作线程处理IO事件。主线程将读事件加入线程池请求队列，读成功的话调用http请求处理函数
    {
        if (timer)
        {
            adjust_timer(timer);
        }
        // 将读事件加入线程池请求队列中，第二个参数0标识是读事件
        m_pool->append(users + sockfd, 0); // 不断查询sockfd的两个参数看是否读成功
        // 读了的话improv置为1，成功timerflag为0，失败为1
        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (users[sockfd].timer_flag == 1)
                {
                    // 读失败，关闭连接，删除定时器
                    deal_timer(timer, sockfd);
                    // 重置回0
                    users[sockfd].timer_flag = 0;
                }
                // 读成功
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else // proactor模式，主线程处理IO事件
    {
        if (users[sockfd].read_once()) // 读成功
        {
            LOG_INFO("deal withthe client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 读完将客户请求放入请求队列等待工作线程处理
            m_pool->append_p(users + sockfd);

            // 有数据传输，说明连接是活跃的，调整定时器超时时间
            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else // 否则关闭连接
        {
            deal_timer(timer, sockfd);
        }
    }
}

// 处理可写事件，逻辑跟可读事件差不多
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    if (m_actormodel == 1) // reactor模式
    {
        if (timer)
        {
            adjust_timer(timer);
        }
        // 监测到可写事件，将请求放入请求队列中
        m_pool->append(users + sockfd, 1);

        while (true) // 循环查看是否写成功
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                // 如果timer不为空，说明读成功有数据传输，连接是活跃的，调整定时器超时时间
                break;
            }
        }
    }
    else
    {
        // proactor主线程负责写
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

// 主循环
void WebServer::eventLoop()
{
    // 超时标志
    bool timeout = false;
    // 是否停止循环标志，收到SIGTERM时被置为1
    bool stop_server = false;

    while (!stop_server)
    {
        // 主线程调用epoll_wait在一段超时时间内等待一组文件描述符上的事件，并将当前所有就绪的epoll_event复制到events数组中
        // 最后一个参数是超时时间，设置为-1则一直阻塞直到有事件发生，等于0则立即返回
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // EINTR错误是由于在任何请求的事件发生或超时到期之前，信号处理程序中断了该应用，如果不是这种错误，则终止循环
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 遍历这一数组处理已经就绪的事件
        for (int i = 0; i < number; i++)
        {
            // 事件表中就绪的sockfd，包括listen, 管道和http连接sockfd(可读或可写)
            int sockfd = events[i].data.fd;
            // 有新连接到来
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (flag == false)
                    continue;
            }
            // 客户链接发生异常，关闭连接，移除注册再内核事件表中的事件，删除该定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理捕捉到的信号（由信号处理函数通过管道发送）
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                // dealwithsignal会根据信号设置timeout, stopserver
                bool flag = dealwithsignal(timeout, stop_server);
                if (flag == false)
                    LOG_ERROR("%s", "dealsignal failure")
            }
            // 某个http连接有可读事件（sockfd读缓冲区有数据到来）
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            // 某个http连接有可写事件（sockfd写缓冲有空闲，可以从http写缓冲区写入sockfd写缓冲区）
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
            if (timeout)
            {
                // 遍历处理超时定时器，有超时的就关闭连接，删除定时器，处理完重设一个alarm延时信号
                utils.timer_handler();

                LOG_INFO("%s", "timer tick");

                timeout = false;
            }
        }
    }
}