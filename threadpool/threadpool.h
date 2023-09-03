#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    // 线程中运行函数worker， worker通过传入的this指针运行run，run不断从请求队列中取出请求进行处理
    // 之所以要这样处理，是因为线程创建函数pthread_create接受的函数必须是静态的，所以worker设置为静态的
    // 而静态成员函数无法访问非静态成员变量，所以要通过传入this指针，通过this指针来运行普通成员函数run来访问
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;         // 线程池中线程数
    int m_max_requests;          // 请求队列中最大请求数
    pthread_t *m_threads;        // 线程池数组，大小为m_thread_number
    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 记录请求队列中请求数的信号量
    connection_pool *m_connPool; // 数据库连接池
    int m_actor_model;           // 模型选择，reactor或模拟proactor
};

// main函数中创建线程池
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests)
    : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(nullptr), m_connPool(connPool)
{
    if(thread_number <= 0 || max_requests <= 0)
    {
        throw std::exception();
    }

    //线程id数组初始化
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads)
    {
        throw std::exception();
    }

    //创建thread_number个线程
    for(int i = 0; i < thread_number; ++i)
    {
        //pthread_create函数原型中的第三个参数，为函数指针，指向处理线程函数的地址。
        //该函数，要求为静态函数。如果处理线程函数为类成员函数时，需要将其设置为静态成员函数。
        //静态成员函数无法操作非静态类成员，所以要通过这个静态成员函数运行另一个函数run来操作
        if(pthread_create(m_threads + i, nullptr, worker, this) != 0)
        {
            delete [] m_threads;
            throw std::exception();
        }

        //线程分离的作用就是让线程自己退出时，自动释放线程资源,这样就不需要主线程等它了
        if(pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

//析构函数，释放线程池
template <typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
}

//reactor模式向队列中添加http请求，通过互斥锁保证线程安全，信号量表示用多少请求，state表示是读还是写（工作线程需要负责写事件），读的话先把数据读出来然后调用process
template <typename T>
bool threadpool<T>::append(T* request, int state)
{
    //操作前先上锁
    m_queuelocker.lock();
    
    //当前请求数已经达到上限
    if(m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    //给requests标识状态，是读还是写
    request->m_state = state;

    //添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    //信号量+1
    m_queuestat.post();
    return true;
}

//proactor模式向队列中添加http请求，主线程负责读和写，工作线程只需要调用请求的process方法
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//worker通过this调用普通私有成员函数run来取出并处理请求
template <typename T>
void* threadpool<T>::worker(void* arg)
{
    //类型强转，把参数转换为线程池类
    threadpool *pool = (threadpool* )arg;
    pool->run();
    return pool;
}

//工作线程不断从请求队列中取出并处理请求
template <typename T>
void threadpool<T>::run()
{
    while(true)
    {
        //信号量为0的话等待变为非0值，即有请求到来
        m_queuestat.wait();

        //处理前抢锁
        m_queuelocker.lock();

        //抢到锁之后先判断还有没有资源，防止已经被其他线程取完
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue; //已经被取完的话重新进入循环继续等待
        }

        //资源还在的话取出第一个任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        
        if(!request)
            continue;

        //开始处理任务
        //reactor模式，工作线程需要负责处理读和写
        if( m_actor_model == 1) 
        {
            //读事件
            if(request->m_state == 0)
            {
                if(request->read_once())
                {
                    //读成功的话将improv标志为1通知主线程
                    request->improv = 1;
                    //利用RAII机制取一条数据库连接用于处理请求
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //读取成功之后运行http请求处理函数（http请求处理的入口）
                    request->process();
                }
                else
                {
                    //读失败全置为1通知主线程
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            //写事件
            else
            {
                if(request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        //proactor
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif