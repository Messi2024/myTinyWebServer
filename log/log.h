#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

// 使用条件变量和互斥锁实现消费者生产者模型，消费者消费者之间，消费者生产者之间，生产者生产者之间都是互斥关系
// 使用了单例懒汉模式,保证只有一个日志类，在第一次使用时才会实例化一个对象，该对象是static的，所以会一直存在，
//下一次在调用还是同一个对象，同时C++11之后保证了静态局部变量的线程安全，所以使用它时不用加锁
class Log
{
public:
    //静态局部变量存放在全局数据区，声称的对象在函数结束也不会消失，再次调用返回的还是同一个对象
    static Log* get_instance()
    {
        static Log instance;
        return &instance;
    }

    //异步写日志方法，因为要传到pthread_create里，而pthread_create只接受静态函数，所以要声明为静态的,
    //同时静态成员函数是没法访问非静态成员的，所以内部通过get_instance()获得一个对象，来访问非静态成员
    static void* flush_log_thread(void* arg)
    {
        Log::get_instance()->async_write_log();
    }

    //初始化，可以设置日志文件名、日志缓冲区大小、最大行数等
    bool init(const char* file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    //将输出内容按照标准格式整理，并根据是同步还是异步读写选择相应的写方法
    void write_log(int level, const char* format, ...);

    //强制刷新缓冲区
    //内部调用的fflush()会强迫将缓冲区内的数据写回参数指定的文件中
    void flush(void);
private:
    //构造和析构定义成私有的，这样保证了只会有一个实例
    Log();
    virtual ~Log();

    //供异步写调用的私有方法
    void* async_write_log()
    {
        string single_log;

        //从阻塞队列中取出一个日志string，写入文件中
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            //输出到文件中
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }
private:

    char dir_name[128];               // 输出路径名
    char log_name[128];               // 输出文件名
    int m_split_lines;                // 日志最大行数
    int m_log_buf_size;               // 日志缓冲区大小
    long long m_count;                // 日志行数
    int m_today;                      // 记录当前是哪一天
    FILE* m_fp;                       // 打开log的文件指针
    char *m_buf;                      // 要输出的内容
    block_queue<string> *m_log_queue; // 循环阻塞队列
    bool m_is_async;                  // 异步还是同步输出
    locker m_mutex;                   // 互斥锁
    int m_close_log;                  // 是否关闭日志
};

//通过可变参数宏格式化输出, format是格式字符串
//__VA_ARGS__用来接收可变的参数，前面两个##是为了在可变参数个数为0时去掉前面的“，”否则会编译出错
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#endif