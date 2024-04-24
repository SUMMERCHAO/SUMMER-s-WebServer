/*半同步/半反应堆线程池的实现，模式原理图如p132所示，代码如p301所示*/
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

/*引入第十四章介绍的线程同步机制的包装类*/
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

/*线程池类，将他定义为模板类是为了代码复用。模板参数T是任务类*/
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request); //向请求队列中添加任务

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之,静态成员函数*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //标识请求队列中的请求数
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;  //数据库
};

/*线程池模板类的构造函数。用于创建线程池对象，并接受一个参数来指定线程池中的线程数量*/
template <typename T>
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool) //初始化列表
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    /*创建线程池m_threads，大小为m_thread_number*/
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    /*创建thread_number个线程，并将他们设置为脱离线程*/
    for (int i = 0; i < thread_number; ++i)
    {
        //printf("create the %dth thread\n",i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;     //释放 m_threads 数组所占用的内存
            throw std::exception();
        }
        /*将创建的线程设置为分离态，这意味着当工作线程任务执行完毕时，它的资源会被系统自动释放，而不需要其它线程调用pthread_join()来等待它结束*/
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

/*线程池模板类的析构函数。用于释放资源，防止内存泄漏和资源泄露*/
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

/*主线程 往请求队列中添加任务*/
/*通过互斥锁保证线程安全，添加完成后通过信号量提醒有任务需要处理，最后注意线程同步*/
template <typename T>
bool threadpool<T>::append(T *request)
{
    /*操作工作队列时一定要加锁，因为它被所有线程共享*/
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();         //将信号量的值加一，提醒有任务需要处理
    return true;
}

/*定义工作线程运行的函数*/
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;   //将参数强转成线程池类
    pool->run();    //调用pool结构体中的run()函数
    return pool;
}

/*工作线程从请求队列中取出某个任务进行处理，注意线程同步*/
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();     //等待一个请求队列中待处理的HTTP请求；将信号量的值减一;相当于加锁
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front(); //从请求队列中取出第一个任务，并将该任务从队列中删除
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        connectionRAII mysqlcon(&request->mysql, m_connPool);  //执行任务
        
        request->process(); //交给线程池中的空闲线程处理，实际执行任务
    }
}
#endif