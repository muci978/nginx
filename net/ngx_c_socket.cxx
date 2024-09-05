#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

CSocket::CSocket()
{
    // 配置相关
    m_worker_connections = 1;      // epoll连接最大项数
    m_ListenPortCount = 1;         // 监听一个端口
    m_RecyConnectionWaitTime = 60; // 等待后才回收连接

    // epoll相关
    m_epollhandle = -1; // epoll返回的句柄

    // 一些和网络通讯有关的常用变量值，供后续频繁使用时提高效率
    m_iLenPkgHeader = sizeof(COMM_PKG_HEADER);  // 包头的sizeof值【占用的字节数】
    m_iLenMsgHeader = sizeof(STRUC_MSG_HEADER); // 消息头的sizeof值【占用的字节数】

    // 各种队列相关
    m_iSendMsgQueueCount = 0;     // 发消息队列大小
    m_totol_recyconnection_n = 0; // 待释放连接队列大小
    m_cur_size_ = 0;              // 当前计时队列尺寸
    m_timer_value_ = 0;           // 当前计时队列头部的时间值
    m_iDiscardSendPkgCount = 0;   // 丢弃的发送数据包数量

    // 在线用户相关
    m_onlineUserCount = 0; // 在线用户数量统计，先给0
    m_lastprintTime = 0;   // 上次打印统计信息的时间，先给0
    return;
}

// 初始化函数
// 成功返回true，失败返回false
bool CSocket::Initialize()
{
    ReadConf();                                // 读配置项
    if (ngx_open_listening_sockets() == false) // 打开监听端口
        return false;
    return true;
}

// 子进程中才需要执行的初始化函数
bool CSocket::Initialize_subproc()
{
    // 发消息互斥量初始化
    if (pthread_mutex_init(&m_sendMessageQueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;
    }
    // 连接相关互斥量初始化
    if (pthread_mutex_init(&m_connectionMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false;
    }
    // 连接回收队列相关互斥量初始化
    if (pthread_mutex_init(&m_recyconnqueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false;
    }
    // 和时间处理队列有关的互斥量初始化
    if (pthread_mutex_init(&m_timequeueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;
    }

    // 初始化发消息相关信号量
    if (sem_init(&m_semEventSendQueue, 0, 0) == -1)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    // 创建线程
    int err;
    ThreadItem *pSendQueue;                                      // 专门用来发送数据的线程
    m_threadVector.push_back(pSendQueue = new ThreadItem(this)); // 创建一个新线程对象并入到容器中
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread, pSendQueue);
    if (err != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_create(ServerSendQueueThread)失败.");
        return false;
    }

    ThreadItem *pRecyconn; // 专门用来回收连接的线程
    m_threadVector.push_back(pRecyconn = new ThreadItem(this));
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread, pRecyconn);
    if (err != 0)
    {
        ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_create(ServerRecyConnectionThread)失败.");
        return false;
    }

    if (m_ifkickTimeCount == 1) // 是否开启踢人时钟，1：开启   0：不开启
    {
        ThreadItem *pTimemonitor; // 专门用来处理到期不发心跳包的用户踢出的线程
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this));
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread, pTimemonitor);
        if (err != 0)
        {
            ngx_log_stderr(0, "CSocket::Initialize_subproc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
}

CSocket::~CSocket()
{
    // 释放必须的内存
    // 监听端口相关内存的释放
    std::vector<lpngx_listening_t>::iterator pos;
    for (pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos) // vector
    {
        delete (*pos);
    }
    m_ListenSocketList.clear();
    return;
}

// 关闭退出函数
void CSocket::Shutdown_subproc()
{
    // 把干活的线程停止掉
    if (sem_post(&m_semEventSendQueue) == -1) // 让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0, "CSocket::Shutdown_subproc()中sem_post(&m_semEventSendQueue)失败.");
    }

    std::vector<ThreadItem *>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL);
    }
    // 释放new出来的ThreadItem
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
            delete *iter;
    }
    m_threadVector.clear();

    // 队列相关
    clearMsgSendQueue();
    clearconnection();
    clearAllFromTimerQueue();

    // 多线程相关
    pthread_mutex_destroy(&m_connectionMutex);
    pthread_mutex_destroy(&m_sendMessageQueueMutex);
    pthread_mutex_destroy(&m_recyconnqueueMutex);
    pthread_mutex_destroy(&m_timequeueMutex);
    sem_destroy(&m_semEventSendQueue);

    ngx_log_error_core(NGX_LOG_NOTICE, 0, "%P 【worker进程】关闭成功......!", ngx_pid);
}

// 清理TCP发送消息队列
void CSocket::clearMsgSendQueue()
{
    char *sTmpMempoint;
    CMemory *p_memory = CMemory::GetInstance();

    while (!m_MsgSendQueue.empty())
    {
        sTmpMempoint = m_MsgSendQueue.front();
        m_MsgSendQueue.pop_front();
        p_memory->FreeMemory(sTmpMempoint);
    }
}

// 专门用于读各种配置项
void CSocket::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections = p_config->GetIntDefault("worker_connections", m_worker_connections);                  // epoll连接的最大项数
    m_ListenPortCount = p_config->GetIntDefault("ListenPortCount", m_ListenPortCount);                           // 取得要监听的端口数量
    m_RecyConnectionWaitTime = p_config->GetIntDefault("Sock_RecyConnectionWaitTime", m_RecyConnectionWaitTime); // 等待这么些秒后才回收连接

    m_ifkickTimeCount = p_config->GetIntDefault("Sock_WaitTimeEnable", 0);  // 是否开启踢人时钟，1：开启   0：不开启
    m_iWaitTime = p_config->GetIntDefault("Sock_MaxWaitTime", m_iWaitTime); // 多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用
    m_iWaitTime = (m_iWaitTime > 5) ? m_iWaitTime : 5;                      // 不建议低于5秒钟，因为无需太频繁
    m_ifTimeOutKick = p_config->GetIntDefault("Sock_TimeOutKick", 0);       // 当时间到达Sock_MaxWaitTime指定的时间时，直接把客户端踢出去，只有当Sock_WaitTimeEnable = 1时，本项才有用

    m_floodAkEnable = p_config->GetIntDefault("Sock_FloodAttackKickEnable", 0);   // Flood攻击检测是否开启,1：开启   0：不开启
    m_floodTimeInterval = p_config->GetIntDefault("Sock_FloodTimeInterval", 100); // 表示每次收到数据包的时间间隔是100(毫秒)
    m_floodKickCount = p_config->GetIntDefault("Sock_FloodKickCounter", 10);      // 累积多少次踢出此人

    return;
}

// 监听端口，支持多个端口
// 在创建worker进程之前就要执行这个函数；
bool CSocket::ngx_open_listening_sockets()
{
    int isock; // socket
    struct sockaddr_in serv_addr;
    int iport;
    char strinfo[100]; // 临时字符串

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    CConfig *p_config = CConfig::GetInstance();
    for (int i = 0; i < m_ListenPortCount; i++) // 监听端口数量
    {

        isock = socket(AF_INET, SOCK_STREAM, 0);
        if (isock == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中socket()失败,i=%d.", i);
            // 其实这里直接退出，那如果以往有成功创建的socket就没得到释放，当然走到这里表示程序不正常，应该整个退出，也没必要释放了
            return false;
        }

        int reuseaddr = 1; // 打开对应的设置项
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEADDR, (const void *)&reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中setsockopt(SO_REUSEADDR)失败,i=%d.", i);
            close(isock);
            return false;
        }

        // 为处理惊群问题使用reuseport
        // SO_REUSEPORT允许多个监听套接字绑定到同一个端口

        int reuseport = 1;
        if (setsockopt(isock, SOL_SOCKET, SO_REUSEPORT, (const void *)&reuseport, sizeof(int)) == -1) // 端口复用需要内核支持
        {
            // 失败就失败，失败顶多是惊群，但程序依旧可以正常运行，所以仅仅提示一下即可
            ngx_log_stderr(errno, "CSocket::Initialize()中setsockopt(SO_REUSEPORT)失败", i);
        }

        // 设置socket为非阻塞
        if (setnonblocking(isock) == false)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中setnonblocking()失败,i=%d.", i);
            close(isock);
            return false;
        }

        strinfo[0] = 0;
        sprintf(strinfo, "ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo, 10000);
        serv_addr.sin_port = htons((in_port_t)iport);

        if (bind(isock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中bind()失败,i=%d.", i);
            close(isock);
            return false;
        }

        if (listen(isock, NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno, "CSocket::Initialize()中listen()失败,i=%d.", i);
            close(isock);
            return false;
        }

        // 放到列表
        lpngx_listening_t p_listensocketitem = new ngx_listening_t;
        memset(p_listensocketitem, 0, sizeof(ngx_listening_t));
        p_listensocketitem->port = iport;
        p_listensocketitem->fd = isock;
        ngx_log_error_core(NGX_LOG_INFO, 0, "监听%d端口成功!", iport);
        m_ListenSocketList.push_back(p_listensocketitem);
    }
    if (m_ListenSocketList.size() <= 0) // 不可能一个端口都不监听
        return false;
    return true;
}

// 设置socket连接为非阻塞模式
bool CSocket::setnonblocking(int sockfd)
{
    int old = fcntl(sockfd, F_GETFL);
    if ((fcntl(sockfd, F_SETFL, old | O_NONBLOCK)) == -1)
    {
        return false;
    }
    return true;
}

// 关闭socket
void CSocket::ngx_close_listening_sockets()
{
    for (int i = 0; i < m_ListenPortCount; i++)
    {
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO, 0, "关闭监听端口%d!", m_ListenSocketList[i]->port);
    }
    return;
}

// 将一个待发送消息入到发消息队列中
void CSocket::msgSend(char *psendbuf)
{
    CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_sendMessageQueueMutex);

    // 发送消息队列过大也可能给服务器带来风险
    if (m_iSendMsgQueueCount > 50000)
    {
        // 发送队列过大，比如客户端恶意不接受数据，就会导致这个队列越来越大
        // 为了服务器安全，干掉一些数据的发送，虽然有可能导致客户端出现问题，但总比服务器不稳定要好很多
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        return;
    }

    // 总体数据并无风险，不会导致服务器崩溃，要看看个体数据，找一下恶意者
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)psendbuf;
    lpngx_connection_t p_Conn = pMsgHeader->pConn;
    if (p_Conn->iSendCount > 400)
    {
        // 该用户收消息太慢或者干脆不收消息，累积的该用户的发送队列中有的数据条目数过大，认为是恶意用户，直接切断
        ngx_log_stderr(0, "CSocket::msgSend()中发现某用户%d积压了大量待发送数据包，切断与他的连接！", p_Conn->fd);
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        zdClosesocketProc(p_Conn); // 直接关闭
        return;
    }

    ++p_Conn->iSendCount; // 发送队列中有的数据条目数+1
    m_MsgSendQueue.push_back(psendbuf);
    ++m_iSendMsgQueueCount; // 原子操作

    if (sem_post(&m_semEventSendQueue) == -1) // 让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0, "CSocket::msgSend()中sem_post(&m_semEventSendQueue)失败.");
    }
    return;
}

// 主动关闭一个连接时的要做些善后的处理函数
// 这个函数是可能被多线程调用的，但是即便被多线程调用，也没关系，不影响本服务器程序的稳定性和正确运行性
void CSocket::zdClosesocketProc(lpngx_connection_t p_Conn)
{
    if (m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn); // 从时间队列中把连接干掉
    }
    if (p_Conn->fd != -1)
    {
        close(p_Conn->fd); // 调用close函数后，内核会自动将fd从epoll中删除
        p_Conn->fd = -1;
    }

    if (p_Conn->iThrowsendCount > 0)
        --p_Conn->iThrowsendCount; // 归0

    inRecyConnectQueue(p_Conn);
    return;
}

// 测试是否flood攻击成立，成立则返回true，否则返回false
bool CSocket::TestFlood(lpngx_connection_t pConn)
{
    struct timeval sCurrTime;
    uint64_t iCurrTime; // 当前时间（单位：毫秒）
    bool reco = false;

    gettimeofday(&sCurrTime, NULL);                                   // 取得当前时间
    iCurrTime = (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000); // 毫秒
    if ((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval) // 两次收到包的时间 < 100毫秒
    {
        // 发包太频繁记录
        pConn->FloodAttackCount++;
        pConn->FloodkickLastTime = iCurrTime;
    }
    else
    {
        // 既然发布不这么频繁，则恢复计数值
        pConn->FloodAttackCount = 0;
        pConn->FloodkickLastTime = iCurrTime;
    }

    if (pConn->FloodAttackCount >= m_floodKickCount)
    {
        // 可以踢此人的标志
        reco = true;
    }
    return reco;
}

// 打印统计信息
void CSocket::printTDInfo()
{
    time_t currtime = time(NULL);
    if ((currtime - m_lastprintTime) > 10)
    {
        // 超过10秒打印一次
        int tmprmqc = g_threadpool.getRecvMsgQueueCount(); // 收消息队列

        m_lastprintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    // atomic做个中转，直接打印atomic类型报错；
        int tmpsmqc = m_iSendMsgQueueCount; // atomic做个中转，直接打印atomic类型报错；
        ngx_log_stderr(0, "------------------------------------begin--------------------------------------");
        ngx_log_stderr(0, "当前在线人数/总人数(%d/%d)。", tmpoLUC, m_worker_connections);
        ngx_log_stderr(0, "连接池中空闲连接/总连接/要释放的连接(%d/%d/%d)。", m_freeconnectionList.size(), m_connectionList.size(), m_recyconnectionList.size());
        ngx_log_stderr(0, "当前时间队列大小(%d)。", m_timerQueuemap.size());
        ngx_log_stderr(0, "当前收消息队列/发消息队列大小分别为(%d/%d)，丢弃的待发送数据包数量为%d。", tmprmqc, tmpsmqc, m_iDiscardSendPkgCount);
        if (tmprmqc > 100000)
        {
            // 接收队列过大，报一下，这个应该引起警觉，考虑限速等等手段
            ngx_log_stderr(0, "接收队列条目数量过大(%d)，要考虑限速或者增加处理线程数量了！！！！！！", tmprmqc);
        }
        ngx_log_stderr(0, "-------------------------------------end---------------------------------------");
    }
    return;
}

// epoll功能初始化，子进程中进行，本函数被ngx_worker_process_init()所调用
int CSocket::ngx_epoll_init()
{
    m_epollhandle = epoll_create(m_worker_connections); // 直接以epoll连接的最大项数为参数
    if (m_epollhandle == -1)
    {
        ngx_log_stderr(errno, "CSocket::ngx_epoll_init()中epoll_create()失败.");
        exit(2);
    }

    // 创建连接池
    initconnection();

    // 遍历所有监听socket，为每个监听socket增加一个连接池中的连接
    std::vector<lpngx_listening_t>::iterator pos;
    for (pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd); // 从连接池中获取一个空闲连接对象
        if (p_Conn == NULL)
        {
            ngx_log_stderr(errno, "CSocket::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2);
        }
        p_Conn->listening = (*pos);  // 连接对象和监听对象关联，方便通过连接对象找监听对象
        (*pos)->connection = p_Conn; // 监听对象和连接对象关联，方便通过监听对象找连接对象

        // 对监听端口的读事件设置处理方法，因为监听端口是用来等对方连接的发送握手的，所以监听端口关心的就是读事件
        p_Conn->rhandler = &CSocket::ngx_event_accept;

        // 往监听socket上增加监听事件

        if (ngx_epoll_oper_event(
                (*pos)->fd,
                EPOLL_CTL_ADD,
                EPOLLIN | EPOLLRDHUP,
                0,
                p_Conn) == -1)
        {
            exit(2);
        }
    }
    return 1;
}

// 对epoll事件的具体操作
// 返回值：成功返回1，失败返回-1；
int CSocket::ngx_epoll_oper_event(
    int fd,                  // socket
    uint32_t eventtype,      // 事件类型
    uint32_t flag,           // 标志，具体含义取决于eventtype
    int bcaction,            // 补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖 eventtype是EPOLL_CTL_MOD时这个参数有用
    lpngx_connection_t pConn // pConn：一个连接的指针
)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if (eventtype == EPOLL_CTL_ADD)
    {
        ev.events = flag;
        pConn->events = flag;
    }
    else if (eventtype == EPOLL_CTL_MOD)
    {
        ev.events = pConn->events;
        if (bcaction == 0)
        {
            // 增加某个标记
            ev.events |= flag;
        }
        else if (bcaction == 1)
        {
            // 去掉某个标记
            ev.events &= ~flag;
        }
        else
        {
            // 完全覆盖某个标记
            ev.events = flag;
        }
        pConn->events = ev.events;
    }
    else
    {
        return 1;
    }

    // EPOLL_CTL_MOD似乎会破坏掉.data.ptr，因此不管是EPOLL_CTL_ADD，还是EPOLL_CTL_MOD，都重新设置
    ev.data.ptr = (void *)pConn;

    if (epoll_ctl(m_epollhandle, eventtype, fd, &ev) == -1)
    {
        ngx_log_stderr(errno, "CSocket::ngx_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.", fd, eventtype, flag, bcaction);
        return -1;
    }
    return 1;
}

// 开始获取发生的事件消息
// 参数int timer：epoll_wait()阻塞的时长，单位是毫秒
// 返回值，1：正常返回 ,0：有问题返回，不管是正常还是问题返回，都应该保持进程继续运行
// 本函数被ngx_process_events_and_timers()调用，而ngx_process_events_and_timers()是在子进程的死循环中被反复调用
int CSocket::ngx_epoll_process_events(int timer)
{
    int events = epoll_wait(m_epollhandle, m_events, NGX_MAX_EVENTS, timer);

    if (events == -1)
    {
        // #define EINTR  4，EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
        if (errno == EINTR)
        {
            ngx_log_error_core(NGX_LOG_INFO, errno, "CSocket::ngx_epoll_process_events()中epoll_wait()失败!");
            return 1; // 正常返回
        }
        else
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocket::ngx_epoll_process_events()中epoll_wait()失败!");
            return 0; // 非正常返回
        }
    }

    if (events == 0)
    {
        if (timer != -1)
        {
            // 要求epoll_wait阻塞一定的时间而不是一直阻塞，这属于阻塞到时间了，则正常返回
            return 1;
        }
        // 无限等待，但却没返回任何事件，这应该有问题
        ngx_log_error_core(NGX_LOG_ALERT, 0, "CSocket::ngx_epoll_process_events()中epoll_wait()没超时却没返回任何事件!");
        return 0; // 非正常返回
    }

    lpngx_connection_t p_Conn;
    uint32_t revents;
    for (int i = 0; i < events; ++i)
    {
        p_Conn = (lpngx_connection_t)(m_events[i].data.ptr); // ngx_epoll_add_event()给进去的，这里能取出来

        // epoll可能连续收到两个绑定到同一fd的事件，事件1因为业务原因将连接关闭了，回收连接时会将fd设置为-1
        // 事件2通过fd得知连接已经断开了，不处理
        if (p_Conn->fd == -1)
        {
            ngx_log_error_core(NGX_LOG_DEBUG, 0, "CSocket::ngx_epoll_process_events()中遇到了fd=-1的过期事件: %p", p_Conn);
            continue;
        }

        revents = m_events[i].events;

        if (revents & EPOLLIN)
        {

            (this->*(p_Conn->rhandler))(p_Conn); // 这是一个成员函数指针
        }

        if (revents & EPOLLOUT) // 对方关闭连接也触发这个
        {
            if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) // 客户端关闭，如果服务器端挂着一个写通知事件，则这里个条件是可能成立的
            {
                --p_Conn->iThrowsendCount;
            }
            else
            {
                (this->*(p_Conn->whandler))(p_Conn); // 如果有数据没有发送完毕，由系统驱动来发送，则这里执行的应该是 CSocket::ngx_write_request_handler()
            }
        }
    }
    return 1;
}

// 处理发送消息队列的线程
void *CSocket::ServerSendQueueThread(void *threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
    CSocket *pSocketObj = pThread->_pThis;
    int err;
    std::list<char *>::iterator pos, pos2, posend;

    char *pMsgBuf;
    LPSTRUC_MSG_HEADER pMsgHeader;
    LPCOMM_PKG_HEADER pPkgHeader;
    lpngx_connection_t p_Conn;
    unsigned short itmp;
    ssize_t sendsize;

    CMemory *p_memory = CMemory::GetInstance();

    while (g_stopEvent == 0) // 不退出
    {
        // 为了让信号量值+1，可以在其他线程调用sem_post达到，实际上在CSocekt::msgSend()调用sem_post就达到了让这里sem_wait走下去的目的
        // 如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR；
        // 整个程序退出之前，也要sem_post()一下，确保如果本线程卡在sem_wait()，也能走下去从而让本线程成功返回
        if (sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            if (errno != EINTR)
                ngx_log_stderr(errno, "CSocket::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");
        }

        // 需要处理数据收发
        if (g_stopEvent != 0) // 要求整个进程退出
            break;

        if (pSocketObj->m_iSendMsgQueueCount > 0) // 原子的
        {
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex);
            if (err != 0)
                ngx_log_stderr(err, "CSocket::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);

            pos = pSocketObj->m_MsgSendQueue.begin();
            posend = pSocketObj->m_MsgSendQueue.end();

            while (pos != posend)
            {
                pMsgBuf = (*pos);                                                        // 拿到的每个消息都是 消息头+包头+包体，但要注意不发送消息头给客户端
                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                                // 指向消息头
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + pSocketObj->m_iLenMsgHeader); // 指向包头
                p_Conn = pMsgHeader->pConn;

                if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
                {
                    // 本包中保存的序列号与p_Conn中实际的序列号已经不同，丢弃此消息
                    pos++;
                    pSocketObj->m_MsgSendQueue.erase(pos2);
                    --pSocketObj->m_iSendMsgQueueCount;
                    p_memory->FreeMemory(pMsgBuf);
                    continue;
                }

                if (p_Conn->iThrowsendCount > 0)
                {
                    // 靠系统驱动来发送消息，所以这里不能再发送
                    pos++;
                    continue;
                }

                --p_Conn->iSendCount;

                // 走到这里，可以发送消息，一些必须的信息记录，要发送的东西也要从发送队列里干掉
                p_Conn->psendMemPointer = pMsgBuf; // 发送后释放用的，因为这段内存是new出来的
                pos2 = pos;
                pos++;
                pSocketObj->m_MsgSendQueue.erase(pos2);
                --pSocketObj->m_iSendMsgQueueCount;
                p_Conn->psendbuf = (char *)pPkgHeader; // 要发送的数据的缓冲区指针，因为发送数据不一定全部都能发送出去，要记录数据发送到了哪里，需要知道下次数据从哪里开始发送
                itmp = ntohs(pPkgHeader->pkgLen);      // 包头+包体长度 ，打包时用了htons
                p_Conn->isendlen = itmp;               // 要发送多少数据，因为发送数据不一定全部都能发送出去，需要知道剩余有多少数据还没发送

                sendsize = pSocketObj->sendproc(p_Conn, p_Conn->psendbuf, p_Conn->isendlen);
                if (sendsize > 0)
                {
                    if (sendsize == p_Conn->isendlen) // 成功发送出去了数据
                    {
                        // 成功发送的和要求发送的数据相等，说明数据全部发完
                        p_memory->FreeMemory(p_Conn->psendMemPointer);
                        p_Conn->psendMemPointer = NULL;
                        p_Conn->iThrowsendCount = 0;
                    }
                    else // 没有全部发送完毕(EAGAIN)，数据只发出去了一部分，肯定是因为发送缓冲区满了
                    {
                        // 发送到了哪里，剩余多少，记录下来，方便下次sendproc()时使用
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
                        p_Conn->isendlen = p_Conn->isendlen - sendsize;
                        // 因为发送缓冲区满了，所以现在要依赖系统通知来发送数据
                        ++p_Conn->iThrowsendCount; // 标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送，原子+1，且不可写成p_Conn->iThrowsendCount = p_Conn->iThrowsendCount +1 ，这种写法不是原子+1
                        // 投递此事件后，依靠epoll调用ngx_write_request_handler()函数发送数据
                        if (pSocketObj->ngx_epoll_oper_event(
                                p_Conn->fd,
                                EPOLL_CTL_MOD,
                                EPOLLOUT,
                                0,
                                p_Conn) == -1)
                        {
                            ngx_log_stderr(errno, "CSocket::ServerSendQueueThread()ngx_epoll_oper_event()失败.");
                        }
                    }
                    continue;
                }

                // 能走到这里应该有问题
                else if (sendsize == 0)
                {

                    p_memory->FreeMemory(p_Conn->psendMemPointer); // 释放内存
                    p_Conn->psendMemPointer = NULL;
                    continue;
                }

                // 继续处理问题
                else if (sendsize == -1)
                {
                    // 一个字节都没发出去，说明发送缓冲区当前正好是满的
                    ++p_Conn->iThrowsendCount; // 标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                    // 投递此事件后，依靠epoll调用ngx_write_request_handler()函数发送数据
                    if (pSocketObj->ngx_epoll_oper_event(
                            p_Conn->fd,
                            EPOLL_CTL_MOD,
                            EPOLLOUT,
                            0,
                            p_Conn) == -1)
                    {
                        ngx_log_stderr(errno, "CSocket::ServerSendQueueThread()中ngx_epoll_add_event()_2失败.");
                    }
                    continue;
                }

                else
                {
                    // 能走到这里的，返回-2，一般认为对端断开了，等待recv()来做断开socket以及回收资源
                    p_memory->FreeMemory(p_Conn->psendMemPointer);
                    p_Conn->psendMemPointer = NULL;
                    continue;
                }
            }

            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex);
            if (err != 0)
                ngx_log_stderr(err, "CSocket::ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!", err);
        }
    }

    return (void *)0;
}
