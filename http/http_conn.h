#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    // 设置读取文件的名称m_read_file长度
    static const int FILENAME_LEN = 200;
    // 设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    // 设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;
    // 报文请求方法，本项目只用到post\get
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, // 分析请求行
        CHECK_STATE_HEADER,         // 分析请求头
        CHECK_STATE_CONTENT         // 分析消息体，仅用于解析POST请求
    };
    // 报文解析结果
    enum HTTP_CODE
    {
        NO_REQUEST,        // 请求不完整，需继续读取请求报文数据
        GET_REQUEST,       // 得到了完整的http请求
        BAD_REQUEST,       // http报文语法错误
        NO_RESOURCE,       // 请求资源不存在
        FORBIDDEN_REQUEST, // 请求资源禁止访问，没有读取权限
        FILE_REQUEST,      // 请求资源可以正常访问
        INTERNAL_ERROR,    // 服务器内部错误，该结果在主状态机switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    // 从状态机状态
    enum LINE_STATUS
    {
        LINE_OK = 0, // 读取到完整一行
        LINE_BAD,    // 报文语法有误
        LINE_OPEN    // 读取的行还不完整
    };
public:
    http_conn(){}
    ~http_conn(){}
public:
    //初始化新接收的连接，会在主循环listen到新连接时在webserver的dealclientdata中通过timer调用，
    //timer同时还会为新连接绑定定时器，同时插入到定时器链表中
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname);
    //异常关闭连接，process_write中写失败，调用它。还有一个关闭函数是timer里面的cbfunc
    void close_conn(bool real_close = true);
    //在工作线程或者主线程将数据读入缓冲区之后调用的处理函数，作用包括调用process_read,process_write
    //即从读缓冲区中读出http请求并处理，然后将响应报文写入写缓冲区中，然后关闭连接
    void process();
    //将浏览器发送来的数据读入读缓冲区
    bool read_once();
    //将写缓冲区的数据写出去
    bool write();
    sockaddr_in* get_address()
    {
        return &m_address;
    }
    //将数据库存储的用户名密码复制到本地，存入map中（所有http连接共享的）
    void initmysql_result(connection_pool* connPool);

    //标识工作线程是否将数据成功读入读缓冲区或是否成功从写缓冲区写出,
    //读或写了的话improv置为1（不管成不成功）（这样reactor模式主线程就能知道工作线程读没读写没写），失败的话timer_flag为1
    int timer_flag;
    int improv;
private:
    //初始化该http资源
    void init();
    //从读缓冲区中读取并处理报文
    HTTP_CODE process_read();
    //根据处理得到的HTTP请求写报文
    bool process_write(HTTP_CODE ret);

    //下面6个函数由process_read()调用用以分析HTTP请求
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的头部数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //分析HTTP请求是注册、登录或者请求什么资源
    HTTP_CODE do_request();
    //m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
    //此时从状态机已提前将一行的末尾字符/r/n变为/0/0，所以text可以直接取出完整的行进行解析
    char *get_line() { return m_read_buf + m_start_line; };
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    //下面9个函数process_write调用，根据相应的HTTP请求，对照响应报文格式，生成对应部分，
    //由process_write调用,通过add_response(const char* format, ...)添加到报文中
    void unmap();    
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
public:
    //静态变量epoll描述符
    static int m_epollfd;
    //静态变量，当前存在的连接数量
    static int m_user_count;
    //数据库连接
    MYSQL *mysql;
    int m_state;  //读事件为0, 写事件为1
private:
    // 连接套接字描述符
    int m_sockfd;
    // 客户地址信息
    sockaddr_in m_address;

    // 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    // 标识缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在缓冲区中的位置
    int m_checked_idx;
    // 当前正在解析的行在缓冲区中的起始位置
    int m_start_line;

    // 写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 写缓冲区中待发送的字节数
    int m_write_idx;

    // 主状态机当前所处的状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;

    // 下面为请求报文中解析出的6个变量
    char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径，其内容等于doc_root+m_url, doc_root是网站根目录
    char *m_url;                    // 客户请求的文件名
    char *m_version;                // HTTP协议版本号，仅支持HTTP1.1
    char *m_host;                   // 主机名
    int m_content_length;           // HTTP请求的消息体长度
    bool m_linger;                  // 是否是长连接

    char *m_file_address;    // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat; // 目标文件的状态，通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小信息
    struct iovec m_iv[2];    // 采用writev来执行操作，所以定义这两个成员，其中m_iv_count表示被写内存块的数量
    int m_iv_count;
    int cgi;             // 是否启用POST
    char *m_string;      // 存储POST的请求内容
    int bytes_to_send;   // 剩余发送字节数
    int bytes_have_send; // 已发送字节数
    char *doc_root;      // 网站根目录

    map<string, string> m_users; // 没用上，本来可能想用来存储从数据库读的用户名密码
    int m_TRIGMode;              // 触发组合模式
    int m_close_log;             // 是否关闭日志

    char sql_user[100];   // 登陆数据库用户名
    char sql_passwd[100]; // 登陆数据库密码
    char sql_name[100];   // 使用数据库名
};
#endif