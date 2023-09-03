#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

//封装信号量，主要用来记录数据库连接池中有多少连接可用，以及list中有多少http请求
class sem
{
public:
    //初始化信号量为0
    sem()
    {
        if(sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }

    //初始化信号量为num
    sem(int num)
    {
        if(sem_init(&m_sem, 0, num) != 0)
        {
            if(sem_init(&m_sem, 0, num) != 0)
            {
                throw std::exception();
            }
        }
    }

    //析构时摧毁信号量
    ~sem()
    {
        sem_destroy(&m_sem);
    }

    //等待信号量
    bool wait()
    {
        //如果m_sem不为0，将其减一，否则原地等待其变为非0值,成功返回0，失败返回-1
        return sem_wait(&m_sem) == 0;
    }

    //释放信号量
    bool post()
    {
        //将m_sem加一，成功返回0，失败返回-1
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

//互斥锁
class locker
{
public:
    //初始化互斥锁
    locker()
    {
        if(pthread_mutex_init(&m_mutex, nullptr) != 0)
        {
            throw std::exception();
        }
    }

    //销毁锁
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    //争锁上锁
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    //解锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get()
    {
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;    
};

//条件变量， 注意要搭配互斥锁一起使用，防止  多个线程使用同一个资源
class cond
{
public:
    //初始化条件变量
    cond()
    {
        if(pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }

    //销毁条件变量
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    //阻塞等待, 在调用wait之前，调用它的线程一般会先加锁，锁 mutex 是为了保证线程从条件判断到进入 pthread_cond_wait 前，条件不被改变
    //即在线程A调用pthread_cond_wait开始，到把A放在等待队列的过程中，都持有互斥锁，其他线程无法得到互斥锁，就不能改变公有资源。
    //把调用线程放入条件变量的请求队列后再内部解锁
    //当条件变量被唤醒之后，pthread_cond_wait又会进行加锁，此时只有一个线程抢到锁，出pthread_cond_wait之后一般会再判断一下资源还在不在
    //在的话出循环然后进行资源的操作，然后解锁，让其他线程操作
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
    }

    //超时等待
    bool timewait(pthread_mutex_t* m_mutex, struct timespec t)
    {
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }

    //唤醒一个等待该条件的线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    //唤醒所有等待该条件的线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }
    
private:
    pthread_cond_t m_cond;
};

#endif