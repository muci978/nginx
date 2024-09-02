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
#include <pthread.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"
// 来数据时候的处理，当连接上有数据来的时候，本函数会被ngx_epoll_process_events()所调用
void CSocket::ngx_read_request_handler(lpngx_connection_t pConn)
{
    bool isflood = false; // 是否flood攻击；

    // 收包，注意用的第二个和第三个参数，用的始终是这两个参数，因此必须保证 c->precvbuf指向正确的收包位置，保证c->irecvlen指向正确的收包宽度
    ssize_t reco = recvproc(pConn, pConn->precvbuf, pConn->irecvlen);
    if (reco <= 0)
    {
        return; // 该处理的上边这个recvproc()函数处理过了，<=0直接return
    }

    // 成功收到了一些字节（>0），开始判断收到了多少数据
    if (pConn->curStat == _PKG_HD_INIT) // 连接建立起来时肯定是这个状态，因为在ngx_get_connection()中已经把curStat成员赋值成_PKG_HD_INIT了
    {
        if (reco == m_iLenPkgHeader) // 正好收到完整包头，这里拆解包头
        {
            ngx_wait_request_handler_proc_p1(pConn, isflood); // 调用专门针对包头处理完整的函数去处理
        }
        else
        {
            // 收到的包头不完整--不能预料每个包的长度，也不能预料各种拆包/粘包情况，所以收到不完整包头【也算是缺包】是很可能的
            pConn->curStat = _PKG_HD_RECVING;         // 接收包头中，包头不完整，继续接收包头中
            pConn->precvbuf = pConn->precvbuf + reco; // 注意收后续包的内存往后走
            pConn->irecvlen = pConn->irecvlen - reco; // 要收的内容当然要减少，以确保只收到完整的包头先
        }
    }
    else if (pConn->curStat == _PKG_HD_RECVING) // 接收包头中，包头不完整，继续接收中，这个条件才会成立
    {
        if (pConn->irecvlen == reco) // 要求收到的宽度和我实际收到的宽度相等
        {
            // 包头收完整了
            ngx_wait_request_handler_proc_p1(pConn, isflood); // 调用专门针对包头处理完整的函数去处理
        }
        else
        {
            pConn->precvbuf = pConn->precvbuf + reco; // 注意收后续包的内存往后走
            pConn->irecvlen = pConn->irecvlen - reco; // 要收的内容当然要减少，以确保只收到完整的包头先
        }
    }
    else if (pConn->curStat == _PKG_BD_INIT)
    {
        // 包头刚好收完，准备接收包体
        if (reco == pConn->irecvlen)
        {
            // 收到的宽度等于要收的宽度，包体也收完整了
            if (m_floodAkEnable == 1)
            {
                // Flood攻击检测是否开启
                isflood = TestFlood(pConn);
            }
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 收到的宽度小于要收的宽度
            pConn->curStat = _PKG_BD_RECVING;
            pConn->precvbuf = pConn->precvbuf + reco;
            pConn->irecvlen = pConn->irecvlen - reco;
        }
    }
    else if (pConn->curStat == _PKG_BD_RECVING)
    {
        // 接收包体中，包体不完整，继续接收中
        if (pConn->irecvlen == reco)
        {
            // 包体收完整了
            if (m_floodAkEnable == 1)
            {
                // Flood攻击检测是否开启
                isflood = TestFlood(pConn);
            }
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 包体没收完整，继续收
            pConn->precvbuf = pConn->precvbuf + reco;
            pConn->irecvlen = pConn->irecvlen - reco;
        }
    }

    if (isflood == true)
    {
        // 客户端flood服务器，则直接把客户端踢掉
        zdClosesocketProc(pConn);
    }

    return;
}

ssize_t CSocket::recvproc(lpngx_connection_t pConn, char *buff, ssize_t buflen)
{
    ssize_t n;

    n = recv(pConn->fd, buff, buflen, 0);
    if (n == 0)
    {

        zdClosesocketProc(pConn);
        return -1;
    }
    // 客户端没断
    if (n < 0) // 这被认为有错误发生
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            ngx_log_stderr(errno, "CSocket::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎我意料！"); // epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1;
        }
        if (errno == EINTR)
        {
            ngx_log_stderr(errno, "CSocket::recvproc()中errno == EINTR成立，出乎我意料！");
            return -1;
        }

        // 所有从这里走下来的错误，都认为异常：意味着要关闭客户端套接字要回收连接池中连接

        zdClosesocketProc(pConn);
        return -1;
    }

    // 能走到这里的，就认为收到了有效数据
    return n; // 返回收到的字节数
}

// 包头收完整后的处理，称为包处理阶段1【p1】：写成函数，方便复用
// 注意参数isflood是个引用
void CSocket::ngx_wait_request_handler_proc_p1(lpngx_connection_t pConn, bool &isflood)
{
    CMemory *p_memory = CMemory::GetInstance();

    LPCOMM_PKG_HEADER pPkgHeader;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo; // 正好收到包头时，包头信息肯定是在dataHeadInfo里

    unsigned short e_pkgLen;
    e_pkgLen = ntohs(pPkgHeader->pkgLen);
    // 恶意包或者错误包的判断
    if (e_pkgLen < m_iLenPkgHeader)
    {
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else if (e_pkgLen > (_PKG_MAX_LENGTH - 1000)) // 客户端发来包居然说包长度 > 29000，肯定是恶意包
    {
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else
    {
        // 合法的包头，继续处理
        // 现在要分配内存开始收包体，因为包体长度并不是固定的，所以内存要new出来；
        char *pTmpBuffer = (char *)p_memory->AllocMemory(m_iLenMsgHeader + e_pkgLen, false); // 分配内存【长度是 消息头长度 + 包头长度 + 包体长度】，最后参数先给false，表示内存不需要memset
        pConn->precvMemPointer = pTmpBuffer;                                                 // 内存开始指针

        // 先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; // 收到包时的连接池中连接序号记录到消息头里来，以备将来用
        // 再填写包头内容
        pTmpBuffer += m_iLenMsgHeader;                   // 往后跳，跳过消息头，指向包头
        memcpy(pTmpBuffer, pPkgHeader, m_iLenPkgHeader); // 直接把收到的包头拷贝进来
        if (e_pkgLen == m_iLenPkgHeader)
        {
            // 相当于收完整了，则直接入消息队列待后续业务逻辑线程去处理
            if (m_floodAkEnable == 1)
            {
                // Flood攻击检测是否开启
                isflood = TestFlood(pConn);
            }
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            pConn->curStat = _PKG_BD_INIT;                  // 当前状态发生改变，包头刚好收完，准备接收包体
            pConn->precvbuf = pTmpBuffer + m_iLenPkgHeader; // pTmpBuffer指向包头，+ m_iLenPkgHeader后指向包体
            pConn->irecvlen = e_pkgLen - m_iLenPkgHeader;   // e_pkgLen是整个包【包头+包体】大小，- m_iLenPkgHeader【包头】= 包体
        }
    }

    return;
}

// 注意参数isflood是个引用
void CSocket::ngx_wait_request_handler_proc_plast(lpngx_connection_t pConn, bool &isflood)
{

    if (isflood == false)
    {
        g_threadpool.inMsgRecvQueueAndSignal(pConn->precvMemPointer); // 入消息队列并触发线程处理消息
    }
    else
    {
        // 对于有攻击倾向的，先把他的包丢掉
        CMemory *p_memory = CMemory::GetInstance();
        p_memory->FreeMemory(pConn->precvMemPointer); // 直接释放掉内存，不往消息队列入
    }

    pConn->precvMemPointer = NULL;
    pConn->curStat = _PKG_HD_INIT;         // 收包状态机的状态恢复为原始态，为收下一个包做准备
    pConn->precvbuf = pConn->dataHeadInfo; // 设置好收包的位置
    pConn->irecvlen = m_iLenPkgHeader;     // 设置好要接收数据的大小
    return;
}

// 发送数据专用函数，返回本次发送的字节数
// 返回 > 0，成功发送了一些字节
//=0，估计对方断了
//-1，errno == EAGAIN ，本方发送缓冲区满了
//-2，errno != EAGAIN != EWOULDBLOCK != EINTR ，一般认为都是对端断开的错误
ssize_t CSocket::sendproc(lpngx_connection_t c, char *buff, ssize_t size)
{
    ssize_t n;

    for (;;)
    {
        n = send(c->fd, buff, size, 0);
        if (n > 0) // 成功发送了一些数据
        {
            return n; // 返回本次发送的字节数
        }

        if (n == 0)
        {
            return 0;
        }

        if (errno == EAGAIN)
        {
            return -1; // 表示发送缓冲区满了
        }

        if (errno == EINTR)
        {
            ngx_log_stderr(errno, "CSocket::sendproc()中send()失败.");
        }
        else
        {
            // 走到这里表示是其他错误码，都表示错误，错误也不断开socket，依然等待recv()来统一处理断开，因为是多线程，send()也处理断开，recv()也处理断开
            return -2;
        }
    }
}

// 数据没法送完毕，要继续发送
void CSocket::ngx_write_request_handler(lpngx_connection_t pConn)
{
    CMemory *p_memory = CMemory::GetInstance();

    ssize_t sendsize = sendproc(pConn, pConn->psendbuf, pConn->isendlen);

    if (sendsize > 0 && sendsize != pConn->isendlen)
    {
        pConn->psendbuf = pConn->psendbuf + sendsize;
        pConn->isendlen = pConn->isendlen - sendsize;
        return;
    }
    else if (sendsize == -1)
    {
        ngx_log_stderr(errno, "CSocket::ngx_write_request_handler()时if(sendsize == -1)成立，这很怪异。");
    }

    if (sendsize > 0 && sendsize == pConn->isendlen)
    {
        // 如果是成功的发送完毕数据，则把写事件通知从epoll中删除；其他情况就是断线了，等着系统内核把连接从红黑树中干掉即可
        if (ngx_epoll_oper_event(
                pConn->fd,
                EPOLL_CTL_MOD,
                EPOLLOUT,
                1,
                pConn) == -1)
        {

            ngx_log_stderr(errno, "CSocket::ngx_write_request_handler()中ngx_epoll_oper_event()失败。");
        }
    }

    // 要么数据发送完毕了，要么对端断开了，执行收尾工作

    // 数据发送完毕，或者把需要发送的数据干掉，都说明发送缓冲区可能有地方了，让发送线程往下走判断能否发送新数据
    if (sem_post(&m_semEventSendQueue) == -1)
        ngx_log_stderr(0, "CSocket::ngx_write_request_handler()中sem_post(&m_semEventSendQueue)失败.");

    p_memory->FreeMemory(pConn->psendMemPointer);
    pConn->psendMemPointer = NULL;
    --pConn->iThrowsendCount;
    return;
}

// 消息本身格式（消息头+包头+包体）
void CSocket::threadRecvProcFunc(char *pMsgBuf)
{
    return;
}
