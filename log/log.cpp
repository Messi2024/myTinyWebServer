#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    //初始化行数为0， 同步写入方式
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    //关闭日志文件
    if(m_fp != nullptr)
    {
        fclose(m_fp);
    }
}

//如果为异步写入方式，需要设置阻塞队列的长度，同步则不需要设置
bool Log::init(const char* file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    if(max_queue_size >= 1)
    {   
        //设置写入方式为异步
        m_is_async = true;
        //初始化阻塞队列
        m_log_queue = new block_queue<string>(max_queue_size);

        pthread_t tid;
        //创建一个新线程来运行flush_log_thread，用来异步写入日志
        pthread_create(&tid, nullptr, flush_log_thread, nullptr);
    }

    m_close_log = close_log;

    //输出缓冲区的长度
    m_log_buf_size = log_buf_size;
    //开辟缓冲区
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    //日志最大行数
    m_split_lines = split_lines;

    time_t t = time(nullptr);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    //从后往前找到第一个/的位置, 为了把日志名跟目录分开
    const char* p = strrchr(file_name, '/');
    //日志全名
    char log_full_name[256] = {0};
    if(p == nullptr)//意思是没有/，也就是没目录，把时间+文件名作为全名
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        //分别把文件名和目录提取出来
        strcpy(log_name, p + 1);
        //p - filename + 1是目录的长度
        strncpy(dir_name, file_name, p - file_name + 1);
        //加上时间作为全名
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    //当mode是“a”时，表示“打开文件，用于追加 (在文件尾写)。如果文件不存在就创建它。流被定位于文件的末尾”。
    m_fp = fopen(log_full_name, "a");
    if(m_fp == nullptr)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char* format, ...)
{
    //获取时间
    struct timeval now = {0,0};
    gettimeofday(&now, nullptr);
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    char s[16] = {0};
    //日志分级
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    } 

    //操作之前先上锁
    m_mutex.lock();

    //更新行数
    m_count++;
    
    //如果上次写日志不是今天或者日志已经满行，则开一个新的, 之所以是m_count % m_split_lines == 0，因为可能已经写满了多个日志文件
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
    {
        char new_log[256] = {0};
        //fflush()会强迫将缓冲区内的数据写回参数stream 指定的文件中
        //如果参数stream 为NULL，fflush()会将所有打开的文件数据更新。
        fflush(m_fp);
        //关闭旧的日志文件
        fclose(m_fp);
        char tail[16] = {0};

        //格式化时间部分
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        //如果日志是新的时间，创建今天的日志
        if(m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            //如果是超过了最大行，则在之前日志名的基础上加上后缀m_count/m_split_lines表示是今天的第几篇日志
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }

        //创建新的日志文件
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    //将format后面的可变参数地址给valst，用于格式化输出
    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入内容格式：时间 + 内容
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    //snprintf()，函数原型为int snprintf(char *str, size_t size, const char *format, ...)。
    //将可变参数 “…” 按照format的格式格式化为字符串，然后再将其拷贝至str中。
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    

    //将格式化数据从可变参数列表写入缓冲区
    //内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';

    log_str = m_buf;
    m_mutex.unlock();

    //若m_is_async为true表示异步，默认为同步
    //若异步,则将日志信息加入阻塞队列,同步则加锁向文件中写
    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强迫将缓冲区内的数据写回参数指定的文件中
    fflush(m_fp);
    m_mutex.unlock();
}