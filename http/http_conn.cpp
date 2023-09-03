#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http相应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to statisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual probliem serving the request file.\n";

// 互斥锁
locker m_lock;
// 存储数据库中已存在的账户信息，用于登陆验证
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取出一个连接
    MYSQL *mysql = nullptr;
    connectionRAII mysqlcon(&mysql, connPool);

    // mysql_query执行查询语句
    if (mysql_query(mysql, "SELECT username, passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 使用mysql_store_result
    MYSQL_RES *result = mysql_store_result(mysql);

    // 使用mysql_num_fields(result)获取查询的列数，mysql_num_rows(result)获取结果集的行数，没有用到
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组,没有用到
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 通过mysql_fetch_row(result)不断获取下一行，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 将文件描述符设置为非阻塞的，Utils工具类中也有相同作用的函数
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向内核事件表注册事件，选择ET/LT模式，选择是否开启EPOLLONESHOT，Utils工具类中也有相同作用的函数
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核事件表删除描述符并关闭文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改fd上的事件为ev，并重置EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (TRIGMode == 1)
    {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    }
    else
    {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 异常关闭连接，process_write中写失败，调用它。还有一个关闭函数是timer里面的cbfunc
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化新接收的连接，会在主循环listen到新连接时在webserver的dealclientdata中通过timer调用，
// timer同时还会为新连接绑定定时器，同时插入到定时器链表中
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 将一个新的文件描述符添加到内核事件表中，即users中sockfd对应的http对象启用了
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    doc_root = root;         // 网站根本目录,文件夹里存放了请求的资源和跳转的html文件
    m_TRIGMode = TRIGMode;   // 触发组合模式
    m_close_log = close_log; // 是否关闭日志

    // 数据库相关信息
    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());
    // 初始化http对象的其他部分。在长连接时，处理完http请求也会调用这个无参的重置连接
    init();
}

// 初始化http对象的其他部分。在长连接时，处理完http请求也会调用这个无参的重置连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机,用于解析一行的内容,解析完成后返回行解析状态,有LINE_OK, LINE_OPEN, LINE_BAD
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        // 如果当前字节是回车符,说明可能遇到了一个完整的行(除消息体外,一个完整的行末尾是/r/n)
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx) // 行不完整,需要继续等待有新的数据到来
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') // 读到了一个完整的行,把/r/n都换成'\0'并把m_checked_idx移到下一行
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD; // 其他情况,说明行错误
        }
        else if (temp == '\n') // 当前字符是回车符,可能是上面不完整的行又读到了新数据,也可能遇到了一个完整的行
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') // 前一个字符是'\r',遇到了完整的行,处理方式同上
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD; // 其他情况,说明行错误
        }
    }
    return LINE_OPEN; // 其他情况说明行不完整
}

// 读取客户端发来的数据,reactor模式由工作线程调用,模拟proactor由主线程调用
// 分为LT和ET,
// LT只调用一次recv(一次recv可能读不完当前对方发送来的数据),还有数据的话,下一次调用epoll_wait还会触发EPOLLIN通知我们
// ET同一事件只会通知一次,所以我们要循环调用recv来接收当前已经送达的数据
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // recv返回0时说明对方已经关闭了连接,出错时返回-1,如果错误是EWOULDBLOCK和EAGAIN的话
    // 在ET模式下,由于会一直循环读到没数据,所以出现这种情况说明数据还没准备好,等会再来读,
    // 但在LT模式下,由于只有在有数据时才会读,所以不存在这种情况,只要是出错,就关闭连接

    // LT模式读,读一次
    if (m_TRIGMode == 0)
    {

        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
        {
            return false;
        }
        return true;
    }
    else // ET,读到缓冲区内无数据
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// http报文格式请求行+请求头+请求体
// 请求行示例: POST  /root/video.html HTTP/1.1
// 解析http请求行,获得请求方法,目标url和http版本哈
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // strpbrk检索两个字符串中首个相同字符的位置,即返回text中第一个空格或制表符的位置
    m_url = strpbrk(text, " \t");
    // 如果没有,报文格式出错
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    // 将该位置置为\0,方便取出前面的数据赋给method,判断请求方法是什么
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }

    // m_url此时跳过了第一个空格或制表符,但可能连着几个都是空格或制表符,所以要找到后面第一个不是空格或制表符的位置,也就是请求资源的开头
    // strspn返回字符串中第一个不在指定字符串中出现的字符下标
    m_url += strspn(m_url, " \t");

    // 找到之后继续向后查找空格或制表符,找到http版本号的位置,如果没找到,说明报文出错
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    // 把这个位置改为\0,用于将前面的数据取出
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    // 如果资源前面带有"http://"就去掉
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    // 同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 当url为/时，显示判断界面（首页）
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

// 解析http的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行,说明头部解析完成,如果有消息体部分,解析消息体,没有则得到了一个完整的HTTP请求
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // 解析connection头部字段,看是否是长连接
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    // 解析content-length段,看消息体长度
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 解析host字段,host作用,同一个服务器可以搭载很多网站,这些网站解析出的IP地址是相同的,那么客户想访问哪个网站就由HOST区分
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("opp!unknow header; %s", text);
    }
    return NO_REQUEST;
}

// 用来判断http消息体是否被完整读入,因为消息体不会被parse_line解析,所以m_checked_idx就停留在消息体开始的地方,m_content_length在解析头部时就得到了,
// 所以当m_read_idx(当前缓冲区字节数)大于等于m_checked_idx + m_content_length时,就说明消息体已经全部被读入读缓冲区了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        // 只有POST请求最后才有消息体,消息体部分就是用户名和密码
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机,由process函数在主线程(模拟proactor)或工作线程(reactor)将数据读入读缓冲区之后调用,来处理收到的http报文
http_conn::HTTP_CODE http_conn::process_read()
{
    // 初始化从状态机状态,判断每行是否完整以及合法
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text;

    // 这里的判断条件后一条就是正常判断一行是否读取完整,不完整跳出循环继续等待数据到来,完整的话进入循环根据当前主状态调用相应函数解析请求行或请求头
    // 前一条是指解析完其他部分后,如果m_content_length不为0的话m_check_state变为CHECK_STATE_CONTENT,来判断消息体是否读取完整
    // 此时前一条是满足的,直接进入循环体内(由于消息体内没有\r\n,parse_line是没法判断一行是不是完整的),调用pasrse_content判断是否完整,不完整退出等待,完整则得到完整请求
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        // 取一行,更新下一行的位置
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        // 主状态机逻辑转移
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            // 解析请求行
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            // 解析请求头
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            // 解析请求体
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    if (line_status == LINE_OPEN) // 数据不全
    {
        return NO_REQUEST;
    }
    else // 语法错误
    {
        return BAD_REQUEST;
    }
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // 在m_real_file前面先加上网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 先找到最后/的位置
    const char *p = strrchr(m_url, '/');

    // 如果是POST请求,即登录或者注册(cgi == 1)
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 提取用户名和密码
        // user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
        {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
        {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        // 注册
        if (*(p + 1) == '3')
        {
            // 插入新数据的sql命令字符串
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 插入前先看有没有重复的
            if (users.find(name) == users.end())
            {
                m_lock.lock();
                // 调用mysql_query插入数据
                int res = mysql_query(mysql, sql_insert);               
                // 校验成功,返回登陆页面
                if (!res)
                {
                    strcpy(m_url, "/log.html");
                    // 没问题的话更新哈希表
                    users.insert(pair<string, string>(name, password));
                    m_lock.unlock();
                }
                else
                {
                    // 校验失败，跳转注册失败页面（因为在获取锁的等待时间里，有其他用户注册了相同的用户名）
                    strcpy(m_url, "/registerError.html");
                }
            }
            else
                strcpy(m_url, "/registedError.html");
        }
        // 如果是登录,直接判断
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
            {
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    // GET请求
    if (*(p + 1) == '0') // 请求登陆界面
    {
        // 把请求资源拼接到网站根目录后面形成完整路径
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1') // 请求登陆界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5') // 图片
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6') // 视频
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '7') // 关注
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
    {
        // 否则发送根据登录或注册的结果,服务器给客户端的反馈,还包括请求的资源只有'/'时返回的首页,
        // 如果都不是上面这些,说明请求的其他文件,返回这些文件
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // 通过stat获取请求资源文件信息,成功则将信息赋给m_file_stat结构体中
    // 失败的话说明文件不存在
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }

    // 判断文件权限是否可读,不可读返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }

    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到虚拟内存中
    int fd = open(m_real_file, O_RDONLY);

    // void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset);
    // start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址。
    // length：映射区的长度。//长度单位是 以字节为单位，不足一内存页按一内存页处理
    // prot：期望的内存保护标志，不能与文件的打开模式冲突。
    // PROT_READ 表示页内容可以被读取，MAP_PRIVATE建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
    // flags：指定映射对象的类型，映射选项和映射页是否可以共享
    // fd：有效的文件描述词。一般是由open()函数返回
    // off_toffset：被映射对象内容的起点。
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    // 避免文件描述符的浪费和占用
    close(fd);

    // 表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

// 取消文件映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 将文件描述符从缓冲区写出
bool http_conn::write()
{
    int temp = 0;
    // 若要发送的数据长度为0,说明响应报文为空
    if (bytes_to_send == 0)
    {
        // 重新监听EPOLLIN
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    // 循环不断写(writev将数据写进TCP写缓冲区，有可能写满了会触发EAGAIN，此时需要等待sock写缓冲区有空闲再次触发 EPOLLOUT
    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0)
        {
            // EAGAIN发生了写阻塞
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }

            // 其他错误取消映射,返回错误
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 注意每次调用writev都从iov_base开始写iov_len长度的数据(如果有),所以每次循环需要更新位置和长度
        // 第一个iovec头部信息的数据（http写缓冲区内的）已发送完，发送第二个（映射文件）
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 第一个还没发送完，继续发送
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 判断数据是否已发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            // 重新监听EPOLLIN
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 长连接的话保持连接，重置HTTP对象
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false; // 发送完毕关闭连接
            }
        }
    }
}

// 利用可变参数列表，将报文写入http写缓冲区
bool http_conn::add_response(const char *format, ...)
{
    // 如果写入的内容超出m_write_buf,则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    // 定义可变参列表
    va_list arg_list;

    // 将format后面的可变参传给arglist
    va_start(arg_list, format);

    // 将数据按format格式写入缓冲区
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 如果写入数据长度大于写入前缓冲区剩余空间,则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list); // 清空可变参
        return false;
    }

    // 更新已写入长度
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

// 添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 根据读处理process_read返回的结果（如果不是INTERNAL_ERROR,返回结果由do_request决定）
// 由工作线程调用向http写缓冲区中写入响应报文
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR: // 内部错误500
    {
        // 状态行
        add_status_line(500, error_500_title);
        // 消息头
        add_headers(strlen(error_500_form));
        // 消息体
        if (!add_content(error_500_form))
            return false;
        break;
    }
    // 报文语法有误，404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    // 资源没有访问权限，403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    // 文件存在，200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        // 如果文件大小不为0，则消息体长度就是文件长度
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // 此时消息体部分不放在HTTP缓冲区，而是文件映射到内存的地方，用writev分块写
            // 第一个iovec指针指向HTTP写缓冲区，长度为m_write_idx,包含状态行+消息头+空行
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // 第二个指向mmap返回的文件指针，长度就是文件大小，为消息体部分
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            // 一共两块
            m_iv_count = 2;
            // 剩余发送字节数
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // 请求文件大小为0，返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }

    // 除了FILE_REQUEST，其他回应只需要一个块，指向缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池函数worker调用的run调用,处理http请求的入口函数
void http_conn::process()
{
    // 解析处理HTTP请求，并返回结果
    HTTP_CODE read_ret = process_read();
    // 结果是NO_REQUEST说明报文不完整，返回继续等待
    if (read_ret == NO_REQUEST)
    {
        // 重置读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    // 解析处理完后写
    bool write_ret = process_write(read_ret);
    // 写错误，关闭连接
    if (!write_ret)
    {
        close_conn();
    }
    // 写入成功，将监听对象换为EPOLLOUT,如果当前sockfd的写缓冲区有空，就通知主线程可以从HTTP的缓冲区写入sock缓冲区中了
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
