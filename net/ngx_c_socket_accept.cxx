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

// 建立新连接专用函数，当新连接进入时，本函数会被ngx_epoll_process_events()所调用
void CSocket::ngx_event_accept(lpngx_connection_t oldc)
{
    struct sockaddr mysockaddr; // 远端服务器的socket地址
    socklen_t socklen;
    int err;
    int level;
    int s;
    static int use_accept4 = 1; // 先认为能够使用accept4()函数
    lpngx_connection_t newc;    // 代表连接池中的一个连接，注意这是指针

    ngx_log_error_core(NGX_LOG_NOTICE, 0, "【master进程】开始accept一个客户端连接.....");

    socklen = sizeof(mysockaddr);
    do
    {
        if (use_accept4)
        {
            // listen套接字是非阻塞的，所以即便已完成连接队列为空，accept4()也不会卡在这里
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); // 从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一个非阻塞的socket，节省一次fcntl
        }
        else
        {
            // listen套接字是非阻塞的，所以即便已完成连接队列为空，accept()也不会卡在这里
            s = accept(oldc->fd, &mysockaddr, &socklen);
        }

        if (s == -1)
        {
            err = errno;

            if (err == EAGAIN)
            {
                return;
            }
            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED) // ECONNRESET错误则发生在对方意外关闭套接字【您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)】
            {
                level = NGX_LOG_ERR;
            }
            else if (err == EMFILE || err == ENFILE) // EMFILE:进程的fd已用尽（已达到系统所允许单一进程所能打开的文件/套接字总数）
                                                     // ENFILE这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resource limits。按照常识，process-specific的resource limits，一定受限于system-wide的resource limits。
            {
                level = NGX_LOG_CRIT;
            }

            if (use_accept4 && err == ENOSYS)
            {
                use_accept4 = 0; // 标记不使用accept4()函数，改用accept()函数
                continue;        // 回去重新用accept()函数
            }

            if (err == ECONNABORTED) // 对方关闭套接字
            {
                // 这个错误可以忽略
            }

            if (err == EMFILE || err == ENFILE)
            {
                // do nothing，这个官方做法是先把读事件从listen socket上移除，然后再弄个定时器，定时器到了则继续执行该函数，但是定时器到了有个标记，会把读事件增加到listen socket上去
                // 这里目前不处理，因为上边已经写这个日志了
            }
            return;
        }

        if (m_onlineUserCount >= m_worker_connections) // 用户连接数过多，要关闭该用户socket，因为现在没分配连接，所以直接关闭即可
        {
            close(s);
            return;
        }
        // 如果某些恶意用户连上来发了1条数据就断，不断连接，会导致频繁调用ngx_get_connection()使用短时间内产生大量连接，危及本服务器安全
        if (m_connectionList.size() > (m_worker_connections * 5))
        {
            // 比如允许同时最大2048个连接，但连接池却有了 2048*5这么大的容量，这肯定是表示短时间内产生大量连接/断开，因为延迟回收机制，这里连接还在垃圾池里没有被回收
            if (m_freeconnectionList.size() < m_worker_connections)
            {
                // 整个连接池这么大了，而空闲连接却这么少了，所以认为是短时间内产生大量连接，发一个包后就断开，不可能让这种情况持续发生，所以必须断开新入用户的连接
                // 一直到m_freeconnectionList变得足够大（连接池中连接被回收的足够多）
                close(s);
                return;
            }
        }

        newc = ngx_get_connection(s); // 这是分配给新的客户连接的连接
        if (newc == NULL)
        {
            // 连接池中连接不够用，那么就把这个socekt直接关闭并返回，因为在ngx_get_connection()中已经写日志了，所以这里不需要写日志了
            if (close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocket::ngx_event_accept()中close(%d)失败!", s);
            }
            return;
        }

        // 成功的拿到了连接池中的一个连接
        memcpy(&newc->s_sockaddr, &mysockaddr, socklen); // 拷贝客户端地址到连接对象

        if (!use_accept4)
        {
            // 如果不是用accept4()取得的socket，那么就要设置为非阻塞（因为用accept4()的已经被accept4()设置为非阻塞了）
            if (setnonblocking(s) == false)
            {
                ngx_close_connection(newc); // 关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟
                return;
            }
        }

        newc->listening = oldc->listening; // 连接对象和监听对象关联，方便通过连接对象找监听对象

        newc->rhandler = &CSocket::ngx_read_request_handler;  // 设置数据来时的读处理函数
        newc->whandler = &CSocket::ngx_write_request_handler; // 设置数据发送时的写处理函数

        // 客户端应该主动发送第一次的数据，将读事件加入epoll监控
        if (ngx_epoll_oper_event(
                s,                    
                EPOLL_CTL_ADD,        
                EPOLLIN | EPOLLRDHUP, 
                0,                    
                newc              
                ) == -1)
        {
            ngx_close_connection(newc); // 关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟
            return;  
        }
        if (m_ifkickTimeCount == 1)
        {
            AddToTimerQueue(newc);
        }
        ++m_onlineUserCount; // 连入用户数量+1
        break;               // 一般就是循环一次就跳出去
    } while (1);

    return;
}
