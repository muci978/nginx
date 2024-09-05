
#include <stdarg.h>
#include <unistd.h> //usleep

#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_memory.h"
#include "ngx_macro.h"

pthread_mutex_t CThreadPool::m_pthreadMutex = PTHREAD_MUTEX_INITIALIZER; 
pthread_cond_t CThreadPool::m_pthreadCond = PTHREAD_COND_INITIALIZER;
bool CThreadPool::m_shutdown = false;                                    // 刚开始标记整个线程池的线程是不退出的

CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0; // 正在运行的线程，开始给个0，注意原子的对象给0也可以直接赋值，当整型变量来用
    m_iLastEmgTime = 0;      // 上次报告线程不够用了的时间；
    m_iRecvMsgQueueCount = 0; // 收消息队列
}

CThreadPool::~CThreadPool()
{
    // 资源释放在StopAll()里统一进行

    // 接收消息队列中内容释放
    clearMsgRecvQueue();
}

// 各种清理函数
// 清理接收消息队列
void CThreadPool::clearMsgRecvQueue()
{
    char *sTmpMempoint;
    CMemory *p_memory = CMemory::GetInstance();

    // 尾声阶段，不需要互斥
    while (!m_MsgRecvQueue.empty())
    {
        sTmpMempoint = m_MsgRecvQueue.front();
        m_MsgRecvQueue.pop_front();
        p_memory->FreeMemory(sTmpMempoint);
    }
}

// 创建线程池中的线程，手工调用，不在构造函数里调用
// 返回值：所有线程都创建成功则返回true，出现错误则返回false
bool CThreadPool::Create(int threadNum)
{
    ThreadItem *pNew;
    int err;

    m_iThreadNum = threadNum; // 保存要创建的线程数量

    for (int i = 0; i < m_iThreadNum; ++i)
    {
        m_threadVector.push_back(pNew = new ThreadItem(this));        // 创建一个新线程对象并入到容器中
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew); 
        if (err != 0)
        {
            ngx_log_stderr(err, "CThreadPool::Create()创建线程%d失败，返回的错误码为%d!", i, err);
            return false;
        }
    }

    // 必须保证每个线程都启动并运行到pthread_cond_wait()，本函数才返回，只有这样，这几个线程才能进行后续的正常工作
    std::vector<ThreadItem *>::iterator iter;
lblfor:
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if ((*iter)->ifrunning == false) // 这个条件保证所有线程完全启动起来，以保证整个线程池中的线程正常工作；
        {
            // 说明有没有启动完全的线程
            usleep(100 * 1000); // 单位是微秒,又因为1毫秒=1000微秒，所以 100 *1000 = 100毫秒
            goto lblfor;
        }
    }
    return true;
}

// 这个是静态成员函数
void *CThreadPool::ThreadFunc(void *threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
    CThreadPool *pThreadPoolObj = pThread->_pThis;

    CMemory *p_memory = CMemory::GetInstance();
    int err;

    pthread_t tid = pthread_self();
    while (true)
    {
        err = pthread_mutex_lock(&m_pthreadMutex);
        if (err != 0)
            ngx_log_stderr(err, "CThreadPool::ThreadFunc()中pthread_mutex_lock()失败，返回的错误码为%d!", err);

        while ((pThreadPoolObj->m_MsgRecvQueue.size() == 0) && m_shutdown == false)
        {
            if (pThread->ifrunning == false)
                pThread->ifrunning = true;                      // 标记为true了才允许调用StopAll()：测试中发现如果Create()和StopAll()紧挨着调用，就会导致线程混乱，所以每个线程必须执行到这里，才认为是启动成功了
            //pthread_cond_wait()函数一进入wait状态就会自动release mutex。当其他线程通过pthread_cond_signal()或pthread_cond_broadcast，把该线程唤醒，使pthread_cond_wait()通过（返回）时，该线程又自动获得该mutex
            pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex); // 整个服务器程序刚初始化的时候，所有线程必然是卡在这里等待的
        }

        if (m_shutdown)
        {   
            pthread_mutex_unlock(&m_pthreadMutex); 
            break;
        }

        // 取得消息进行处理, 注意，目前还是互斥
        char *jobbuf = pThreadPoolObj->m_MsgRecvQueue.front();
        pThreadPoolObj->m_MsgRecvQueue.pop_front();            
        --pThreadPoolObj->m_iRecvMsgQueueCount;                

        err = pthread_mutex_unlock(&m_pthreadMutex);
        if (err != 0)
            ngx_log_stderr(err, "CThreadPool::ThreadFunc()中pthread_mutex_unlock()失败，返回的错误码为%d!", err); 

        // 开始处理
        ++pThreadPoolObj->m_iRunningThreadNum; // 记录正在干活的线程数量增加1（原子性，比加锁快）

        g_socket.threadRecvProcFunc(jobbuf); // 处理消息队列中来的消息

        p_memory->FreeMemory(jobbuf);          // 释放消息内存
        --pThreadPoolObj->m_iRunningThreadNum; // 记录正在干活的线程数量减少1

    }

    return (void *)0;
}

// 停止所有线程
void CThreadPool::StopAll()
{
    // 已经调用过，就不要重复调用了
    if (m_shutdown == true)
    {
        return;
    }
    m_shutdown = true;

    // 唤醒所有等待中的线程，通过m_shutdown退出循环
    int err = pthread_cond_broadcast(&m_pthreadCond);
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::StopAll()中pthread_cond_broadcast()失败，返回的错误码为%d!", err);
        return;
    }

    // 阻塞等待所有线程
    std::vector<ThreadItem *>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL);
    }

    pthread_mutex_destroy(&m_pthreadMutex);
    pthread_cond_destroy(&m_pthreadCond);

    // 释放new出来的ThreadItem
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
            delete *iter;
    }
    m_threadVector.clear();

    ngx_log_stderr(0, "CThreadPool::StopAll()成功返回，线程池中线程全部正常结束!");
    return;
}

// 收到一个完整消息后，入消息队列，并触发线程池中线程来处理该消息
void CThreadPool::inMsgRecvQueueAndSignal(char *buf)
{
    int err = pthread_mutex_lock(&m_pthreadMutex);
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::inMsgRecvQueueAndSignal()pthread_mutex_lock()失败，返回的错误码为%d!", err);
    }

    m_MsgRecvQueue.push_back(buf); 
    ++m_iRecvMsgQueueCount; 
    err = pthread_mutex_unlock(&m_pthreadMutex);
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::inMsgRecvQueueAndSignal()pthread_mutex_unlock()失败，返回的错误码为%d!", err);
    }

    // 激发一个线程
    Call();
    return;
}

// 调一个线程池中的线程干活
void CThreadPool::Call()
{
    int err = pthread_cond_signal(&m_pthreadCond);
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::Call()中pthread_cond_signal()失败，返回的错误码为%d!", err);
    }

    if (m_iThreadNum == m_iRunningThreadNum) // 线程池中线程总量跟当前正在干活的线程数量一样，说明所有线程都忙碌起来，线程不够用了
    {

        time_t currtime = time(NULL);
        if (currtime - m_iLastEmgTime > 10) // 最少间隔10秒钟才报一次线程池中线程不够用的问题；
        {
            m_iLastEmgTime = currtime; // 更新时间
            ngx_log_stderr(0, "CThreadPool::Call()中发现线程池中当前空闲线程数量为0，要考虑扩容线程池了!");
        }
    }

    return;
}
