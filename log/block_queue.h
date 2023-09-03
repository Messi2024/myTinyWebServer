/*************************************************************
 *循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;
 *线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
 **************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

// 使用条件变量和互斥锁实现消费者生产者模型，用于异步写日志，消费者消费者之间，消费者生产者之间，生产者生产者之间都是互斥关系
// push相当于生产，pop相当于消费
template <class T>
class block_queue
{
public:
    // 构造，初始化循环数组
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0)
        {
            exit(-1);
        }

        // 初始化循环队列最大长度
        m_max_size = max_size;

        // 申请循环数组内存
        m_array = new T[max_size];

        // 当前循环数组内元素个数
        m_size = 0;

        // 初始化队首和队尾位置
        m_front = -1;
        m_back = -1;
    }

    // 析构，释放掉占用的内存
    ~block_queue()
    {
        m_mutex.lock();
        if (m_array != nullptr)
        {
            delete[] m_array;
        }
        m_mutex.unlock();
    }

    // 清空循环数组
    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    // 判断队列是否为满
    bool full()
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty()
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 返回队首元素,存到value中
    bool front(T &value)
    {
        m_mutex.lock();
        if (m_size == 0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T &value)
    {
        m_mutex.lock();
        if (m_size == 0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    // 返回元素个数
    int size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    // 返回最大容量
    int max_size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    // 往队列中添加元素，然后将所有等待的消费者线程唤醒
    // 相当于生产者生产了一个元素
    bool push(const T &item)
    {
        m_mutex.lock();
        // 满了的话push失败并唤醒所有等待的消费者
        if (m_size >= m_max_size)
        {
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        // 不满则将新增数据添加在尾部
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        // push成功也唤醒等待的消费者
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    // 消费者，如果队列里没有元素，则等待条件变量唤醒
    bool pop(T &item)
    {
        m_mutex.lock();

        // 有多个消费者，要用while循环，防止没抢到锁的消费者在其他消费者取到资源后抢到锁，
        // 但资源已经被其他消费者取完而出错
        while (m_size <= 0)
        {
            // 进入while循环判断时，有资源直接取，没资源阻塞等待，被唤醒（唤醒+抢到锁）后m_cond.wait()
            // 返回true，再判断一次还有没有资源，有资源则不进入循环，出循环取，没资源接着阻塞等待
            if (!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }
        // 取出队首元素
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
    // 带有超时处理的pop
    bool pop(T &item, int ms_timeout)
    {
        // ns精度
        struct timespec t = {0, 0}; // 这里应该也用timeval才跟下面对的上
        // us精读
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0)
        {
            // ms_timeout（ms精度）
            t.tv_sec = now.tv_sec + ms_timeout / 1000; // 秒
            t.tv_nsec = (ms_timeout % 1000) * 1000;    // 纳秒
            // 超时等待
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        // 确认资源没有刚好被其他资源拿走
        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    // 互斥锁
    locker m_mutex;

    // 条件变量
    cond m_cond;

    // 循环数组
    T *m_array;

    // 元素个数
    int m_size;

    // 最大元素个数
    int m_max_size;

    // 队首元素前一个位置
    int m_front;

    // 队尾元素位置
    int m_back;
};
#endif
