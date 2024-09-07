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

// 设置踢出时钟(向multimap表中增加内容)，用户三次握手成功连入，并且踢人开关Sock_WaitTimeEnable = 1，那么本函数被调用
void CSocket::AddToTimerQueue(lpngx_connection_t pConn)
{
	CMemory *p_memory = CMemory::GetInstance();

	// 超时时间
	time_t futtime = time(NULL);
	futtime += m_iWaitTime; // 20秒之后的时间

	CLock lock(&m_timequeueMutex); // 互斥，因为要操作m_timeQueuemap了
	LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(m_iLenMsgHeader, false);
	tmpMsgHeader->pConn = pConn;
	tmpMsgHeader->iCurrsequence = pConn->iCurrsequence;
	m_timerQueuemap.insert(std::make_pair(futtime, tmpMsgHeader)); // 按键 自动排序 小->大
	m_cur_size_++;												   // 计时队列尺寸+1
	m_timer_value_ = GetEarliestTime();							   // 计时队列头部时间值保存到m_timer_value_里
	return;
}

// 从multimap中取得最早的时间返回，调用者负责互斥，所以本函数不用互斥，调用者确保m_timeQueuemap中一定不为空
time_t CSocket::GetEarliestTime()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;
	pos = m_timerQueuemap.begin();
	return pos->first;
}

// 从m_timeQueuemap移除最早的时间，并把最早这个时间所在的项的值所对应的指针返回，调用者负责互斥，所以本函数不用互斥
LPSTRUC_MSG_HEADER CSocket::RemoveFirstTimer()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;
	LPSTRUC_MSG_HEADER p_tmp;
	if (m_cur_size_ <= 0)
	{
		return NULL;
	}
	pos = m_timerQueuemap.begin();
	p_tmp = pos->second;
	m_timerQueuemap.erase(pos);
	--m_cur_size_;
	return p_tmp;
}

// 根据给的当前时间，从m_timeQueuemap找到比这个时间更早的一个节点返回去，这些节点都是时间超过了，要处理的节点
// 调用者负责互斥，所以本函数不用互斥
LPSTRUC_MSG_HEADER CSocket::GetOverTimeTimer(time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	LPSTRUC_MSG_HEADER ptmp;

	if (m_cur_size_ == 0 || m_timerQueuemap.empty())
		return NULL;

	time_t earliesttime = GetEarliestTime();
	if (earliesttime <= cur_time)
	{
		// 超时的节点
		ptmp = RemoveFirstTimer(); // 把这个超时的节点从 m_timerQueuemap 删掉，并把这个节点的消息头返回

		// 如果不是要求超时就立马踢出才做这里的事
		// 因为下次超时的时间也依然要判断，所以还要把这个节点加回来
		if (m_ifTimeOutKick != 1)
		{
			// 在一个等待时间后再次检查
			// 若上一次ping的时间到当前时间差值大于最大等待时间时会将连接踢出，在关闭线程时从队列删除
			time_t newinqueutime = cur_time + (m_iWaitTime);
			LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER), false);
			tmpMsgHeader->pConn = ptmp->pConn;
			tmpMsgHeader->iCurrsequence = ptmp->iCurrsequence;
			m_timerQueuemap.insert(std::make_pair(newinqueutime, tmpMsgHeader));
			m_cur_size_++;
		}

		if (m_cur_size_ > 0)
		{
			m_timer_value_ = GetEarliestTime(); // 计时队列头部时间值保存到m_timer_value_里
		}
		return ptmp;
	}
	return NULL;
}

// 把指定用户tcp连接从timer表中删除
void CSocket::DeleteFromTimerQueue(lpngx_connection_t pConn)
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos, posend;
	CMemory *p_memory = CMemory::GetInstance();

	CLock lock(&m_timequeueMutex);

	// 如果使用立即踢出，则在已经删除了，否则在此删除
	// 因为实际情况可能比较复杂，将来可能还扩充代码等等，所以遍历整个队列找一圈，而不是找到一次，以免出现遗漏
lblMTQM:
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	for (; pos != posend; ++pos)
	{
		if (pos->second->pConn == pConn)
		{
			p_memory->FreeMemory(pos->second);
			m_timerQueuemap.erase(pos);
			--m_cur_size_;
			goto lblMTQM;
		}
	}
	if (m_cur_size_ > 0)
	{
		m_timer_value_ = GetEarliestTime();
	}
	return;
}

// 清理时间队列中所有内容
void CSocket::clearAllFromTimerQueue()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos, posend;

	CMemory *p_memory = CMemory::GetInstance();
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	for (; pos != posend; ++pos)
	{
		p_memory->FreeMemory(pos->second);
		--m_cur_size_;
	}
	m_timerQueuemap.clear();
}

// 时间队列监视和处理线程，处理到期不发心跳包的用户踢出的线程
void *CSocket::ServerTimerQueueMonitorThread(void *threadData)
{
	ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
	CSocket *pSocketObj = pThread->_pThis;

	time_t absolute_time, cur_time;
	int err;

	while (g_stopEvent == 0) // 不退出
	{	
		// 只有加入的时候和拿取的时候会访问，若为0不会操作，可以省去一个互斥
		// 没互斥判断，只是个初级判断，目的是减少队列为空时避免系统损耗
		if (pSocketObj->m_cur_size_ > 0) // 队列一定有数据
		{
			// 时间队列中最近发生事情的时间放到 absolute_time里；
			absolute_time = pSocketObj->m_timer_value_; // 这个省了个互斥
			cur_time = time(NULL);
			if (absolute_time < cur_time)
			{
				// 时间到了，可以处理了
				std::list<LPSTRUC_MSG_HEADER> m_lsIdleList; // 保存要处理的内容
				LPSTRUC_MSG_HEADER result;

				err = pthread_mutex_lock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
					ngx_log_stderr(err, "CSocket::ServerTimerQueueMonitorThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);
				while ((result = pSocketObj->GetOverTimeTimer(cur_time)) != NULL) // 一次性的把所有超时节点都拿过来
				{
					m_lsIdleList.push_back(result);
				}
				err = pthread_mutex_unlock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
					ngx_log_stderr(err, "CSocket::ServerTimerQueueMonitorThread()pthread_mutex_unlock()失败，返回的错误码为%d!", err); // 有问题，要及时报告
				LPSTRUC_MSG_HEADER tmpmsg;
				while (!m_lsIdleList.empty())
				{
					tmpmsg = m_lsIdleList.front();
					m_lsIdleList.pop_front();
					pSocketObj->procPingTimeOutChecking(tmpmsg, cur_time); // 这里需要检查心跳超时问题
				}
			}
		}

		usleep(500 * 1000); // 为简化问题，每次休息500毫秒
	}

	return (void *)0;
}

// 心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数只是把内存释放，子类应该重新实现该函数以实现具体的判断动作
void CSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	p_memory->FreeMemory(tmpmsg);
}
