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
#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"

// 定义成员函数指针
typedef bool (CLogicSocket::*handler)(lpngx_connection_t pConn,      // 连接池中连接的指针
                                      LPSTRUC_MSG_HEADER pMsgHeader, // 消息头指针
                                      char *pPkgBody,                // 包体指针
                                      unsigned short iBodyLength);   // 包体长度

// 用来保存成员函数指针的数组
static const handler statusHandler[] =
    {
        // 数组前5个元素，保留，以备将来增加一些基本服务器功能
        &CLogicSocket::_HandlePing, // 【0】：心跳包的实现
        NULL,
        NULL,
        NULL,
        NULL,

        // 开始处理具体的业务逻辑
        &CLogicSocket::_HandleRegister, // 【5】：实现具体的注册功能
        &CLogicSocket::_HandleLogIn,    // 【6】：实现具体的登录功能
                                        // 其他待扩展

};
#define AUTH_TOTAL_COMMANDS sizeof(statusHandler) / sizeof(handler) // 整个命令有多少个，编译时即可知道

CLogicSocket::CLogicSocket()
{
}

CLogicSocket::~CLogicSocket()
{
}

// 初始化函数【fork()子进程之前干这个事】
// 成功返回true，失败返回false
bool CLogicSocket::Initialize()
{
    // 本类相关的初始化工作
    // 根据需要扩展
    bool bParentInit = CSocket::Initialize();
    return bParentInit;
}

// 处理收到的数据包，由线程池来调用本函数
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                   // 消息头
    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + m_iLenMsgHeader); // 包头
    void *pPkgBody = NULL;                                                                // 指向包体的指针
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);                             // 客户端指明的包宽度【包头+包体】

    if (m_iLenPkgHeader == pkglen)
    {
        // 没有包体，只有包头
        if (pPkgHeader->crc32 != 0) // 只有包头的crc值为0
        {
            return; // crc错，直接丢弃
        }
        pPkgBody = NULL;
    }
    else
    {
        // 有包体
        pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);
        pPkgBody = (void *)(pMsgBuf + m_iLenMsgHeader + m_iLenPkgHeader); // 跳过消息头以及包头，指向包体

        int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody, pkglen - m_iLenPkgHeader); // 计算纯包体的crc值
        if (calccrc != pPkgHeader->crc32)                                                                  // 服务器端根据包体计算crc值，和客户端传递过来的包头中的crc32信息比较
        {
            ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中CRC错误[服务器:%d/客户端:%d]，丢弃数据!", calccrc, pPkgHeader->crc32);
            return; // crc错，直接丢弃
        }
    }

    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); // 消息代码拿出来
    lpngx_connection_t p_Conn = pMsgHeader->pConn;        // 消息头中藏着连接池中连接的指针

    // 连接的序号发生了改变，说明连接发生了变动，原连接已经断开了
    if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
    {
        return; // 丢弃包
    }

    if (imsgCode >= AUTH_TOTAL_COMMANDS) // 无符号数不可能<0
    {
        ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!", imsgCode); // 这种有恶意倾向或者错误倾向的包，希望打印出来看看是谁干的
        return;                                                                                    // 丢弃包，恶意包或者错误包
    }

    if (statusHandler[imsgCode] == NULL)
    {
        ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!", imsgCode);
        return;
    }

    (this->*statusHandler[imsgCode])(p_Conn, pMsgHeader, (char *)pPkgBody, pkglen - m_iLenPkgHeader);
    return;
}

// 心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数是子类函数，实现具体的判断动作
void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();

    if (tmpmsg->iCurrsequence == tmpmsg->pConn->iCurrsequence) // 此连接没断
    {
        lpngx_connection_t p_Conn = tmpmsg->pConn;

        if (m_ifTimeOutKick == 1)
        {
            zdClosesocketProc(p_Conn);
        }
        else if ((cur_time - p_Conn->lastPingTime) > (m_iWaitTime * 3 + 10)) // 超时踢的判断标准就是每次检查的时间间隔*3，超过这个时间没发送心跳包，就踢出
        {
            // 踢出去，如果此时此刻该用户正好断线，则这个socket可能立即被后续上来的连接复用，如果赶上这个点了，那么可能错踢，错踢就错踢
            zdClosesocketProc(p_Conn);
        }

        p_memory->FreeMemory(tmpmsg);
    }
    else // 此连接断了
    {
        p_memory->FreeMemory(tmpmsg);
    }
    return;
}

// 发送没有包体的数据包给客户端
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader, unsigned short iMsgCode)
{
    CMemory *p_memory = CMemory::GetInstance();

    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false);
    char *p_tmpbuf = p_sendbuf;

    memcpy(p_tmpbuf, pMsgHeader, m_iLenMsgHeader);
    p_tmpbuf += m_iLenMsgHeader;

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf; // 指向的是要发送出去的包的包头
    pPkgHeader->msgCode = htons(iMsgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader);
    pPkgHeader->crc32 = 0;
    msgSend(p_sendbuf);
    return;
}

// 处理各种业务逻辑，以下为一些通用操作，不属于脚手架内容
bool CLogicSocket::_HandleRegister(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    // 判断包体的合法性
    if (pPkgBody == NULL) // 具体看客户端服务器约定，如果约定这个命令[msgCode]必须带包体，那么如果不带包体，就认为是恶意包，不处理
    {
        return false;
    }

    int iRecvLen = sizeof(STRUCT_REGISTER);
    if (iRecvLen != iBodyLength) // 发送过来的结构大小不对，认为是恶意包，不处理
    {
        return false;
    }

    // 凡是和本用户有关的访问都互斥，因为如果同一个客户端发送多个请求，可能会出现几个线程同时处理的情况
    CLock lock(&pConn->logicPorcMutex); 

    // 取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;
    p_RecvInfo->iType = ntohl(p_RecvInfo->iType);               // 所有数值型,short,int,long,uint64_t,int64_t传输和接收都要转化字节序
    p_RecvInfo->username[sizeof(p_RecvInfo->username) - 1] = 0; // 防止客户端发送过来畸形包，导致服务器直接使用这个数据出现错误
    p_RecvInfo->password[sizeof(p_RecvInfo->password) - 1] = 0; // 防止客户端发送过来畸形包，导致服务器直接使用这个数据出现错误

    // 给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商，这里就给客户端也返回同样的 STRUCT_REGISTER
    LPCOMM_PKG_HEADER pPkgHeader;
    CMemory *p_memory = CMemory::GetInstance();
    CCRC32 *p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_REGISTER);
    // 分配要发送出去的包的内存
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iSendLen, false); // 准备发送的格式，这里是 消息头+包头+包体
    // 填充消息头
    memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader); // 消息头直接拷贝到这里来
    // 填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + m_iLenMsgHeader); // 指向包头
    pPkgHeader->msgCode = _CMD_REGISTER;                           // 消息代码，可以统一在ngx_logiccomm.h中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);              // htons主机序转网络序
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader + iSendLen);        // 整个包的尺寸，包头+包体尺寸
    // 填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader); // 跳过消息头，跳过包头，就是包体了

    // 包体内容全部确定好后，计算包体的crc32值
    pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char *)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);

    // 发送数据包
    msgSend(p_sendbuf);

    return true;
}
bool CLogicSocket::_HandleLogIn(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    if (pPkgBody == NULL)
    {
        return false;
    }
    int iRecvLen = sizeof(STRUCT_LOGIN);
    if (iRecvLen != iBodyLength)
    {
        return false;
    }
    CLock lock(&pConn->logicPorcMutex);

    LPSTRUCT_LOGIN p_RecvInfo = (LPSTRUCT_LOGIN)pPkgBody;
    p_RecvInfo->username[sizeof(p_RecvInfo->username) - 1] = 0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password) - 1] = 0;

    LPCOMM_PKG_HEADER pPkgHeader;
    CMemory *p_memory = CMemory::GetInstance();
    CCRC32 *p_crc32 = CCRC32::GetInstance();

    int iSendLen = sizeof(STRUCT_LOGIN);
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iSendLen, false);
    memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader);
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + m_iLenMsgHeader);
    pPkgHeader->msgCode = _CMD_LOGIN;
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader + iSendLen);
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader);
    pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char *)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);
    msgSend(p_sendbuf);
    return true;
}

// 接收并处理发过来的ping包
bool CLogicSocket::_HandlePing(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    if (iBodyLength != 0) // 有包体则认为是非法包
        return false;

    CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都考虑用互斥，以免该用户同时发送过来两个命令达到各种作弊目的
    pConn->lastPingTime = time(NULL);   // 更新该变量

    // 服务器也发送 一个只有包头的数据包给客户端，作为返回的数据
    SendNoBodyPkgToClient(pMsgHeader, _CMD_PING);

    return true;
}