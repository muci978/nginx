﻿#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "ngx_macro.h"		  //各种宏定义
#include "ngx_func.h"		  //各种函数声明
#include "ngx_c_conf.h"		  //和配置文件处理相关的类,名字带c_表示和类有关
#include "ngx_c_socket.h"	  //和socket通讯相关
#include "ngx_c_memory.h"	  //和内存分配释放等相关
#include "ngx_c_threadpool.h" //和多线程有关
#include "ngx_c_crc32.h"	  //和crc32校验算法有关
#include "ngx_c_slogic.h"	  //和socket通讯相关

static void freeresource();

// 和设置有关的全局量
size_t g_argvneedmem = 0; // 保存下这些argv参数所需要的内存大小
size_t g_envneedmem = 0;  // 环境变量所占内存大小
int g_os_argc;			  // 参数个数
char **g_os_argv;		  // 原始命令行参数数组,在main中会被赋值
char *gp_envmem = NULL;	  // 指向自己分配的env环境变量的内存，在ngx_init_setproctitle()函数中会被分配内存
int g_daemonized = 0;	  // 守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了

CLogicSocket g_socket;	  // socket全局对象
CThreadPool g_threadpool; // 线程池全局对象

// 和进程本身有关的全局量
pid_t ngx_pid;		 // pid
pid_t ngx_parent;	 // ppid
int ngx_process;	 // 进程类型，比如master,worker进程等
int g_stopEvent = 0; // 标志程序退出,0不退出1，退出

sig_atomic_t ngx_working_subprocess = 0; // 运行中的子进程数量

sig_atomic_t ngx_reap;
// 标记子进程状态变化(一般是子进程发来SIGCHLD信号表示退出)

int main(int argc, char *const *argv)
{
	int exitcode = 0; // 退出代码，先给0表示正常退出
	int i;			  // 临时变量

	g_stopEvent = 0; // 标记程序是否退出，0不退出

	ngx_pid = getpid();		// 取得进程pid
	ngx_parent = getppid(); // 取得父进程的id
	// 统计argv所占的内存
	g_argvneedmem = 0;
	for (i = 0; i < argc; i++)
	{
		g_argvneedmem += strlen(argv[i]) + 1; //+1是给\0留空间。
	}
	// 统计环境变量所占的内存。注意判断方法是environ[i]是否为空作为环境变量结束标记
	// environ是一个全局的外部变量，其类型为char**，存储着系统的环境变量。
	for (i = 0; environ[i]; i++)
	{
		g_envneedmem += strlen(environ[i]) + 1; //+1是因为末尾有\0
	}

	g_os_argc = argc;		   // 保存参数个数
	g_os_argv = (char **)argv; // 保存参数指针

	// 全局量有必要初始化的
	ngx_log.fd = -1;				  //-1：表示日志文件尚未打开；因为后边ngx_log_stderr要用所以这里先给-1
	ngx_process = NGX_PROCESS_MASTER; // 先标记本进程是master进程
	ngx_reap = 0;					  // 标记子进程没有发生变化

	CConfig *p_config = CConfig::GetInstance();
	if (p_config->Load("nginx.conf") == false) // 把配置文件内容载入到内存
	{
		ngx_log_init(); // 初始化日志
		ngx_log_stderr(0, "配置文件[%s]载入失败，退出!", "nginx.conf");
		// exit(0)表示程序正常, exit(1)/exit(-1)表示程序异常退出，exit(2)表示表示系统找不到指定的文件
		exitcode = 2; // 标记找不到文件
		goto lblexit;
	}
	// 内存单例类初始化
	CMemory::GetInstance();
	// crc32校验算法单例类初始化
	CCRC32::GetInstance();

	// 一些必须事先准备好的资源，先初始化
	ngx_log_init(); // 日志初始化(创建/打开日志文件)，这个需要配置项，所以必须放配置文件载入的后边

	// 一些初始化函数，准备放这里
	if (ngx_init_signals() != 0) // 信号初始化
	{
		exitcode = 1;
		goto lblexit;
	}
	g_socket.Initialize(); // 初始化socket

	ngx_init_setproctitle(); // 把环境变量搬家

	// 创建守护进程
	if (p_config->GetIntDefault("Daemon", 0) == 1)
	{
		// 按守护进程方式运行
		int cdaemonresult = ngx_daemon();
		if (cdaemonresult == -1)
		{
			exitcode = 1;
			goto lblexit;
		}
		if (cdaemonresult == 1)
		{
			// 这是原始的父进程
			freeresource(); // 只有进程退出了才goto到 lblexit，用于提醒用户进程退出了
			exitcode = 0;
			return exitcode; // 整个进程直接在这里退出
		}
		g_daemonized = 1; // 守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了
	}

	ngx_master_process_cycle(); // 不管父进程还是子进程，正常工作期间都在这个函数里循环；

lblexit:
	// 释放资源
	ngx_log_stderr(0, "程序退出，再见了!");
	freeresource(); // 一系列的main返回前的释放动作函数
	return exitcode;
}

void freeresource()
{
	// 对于因为设置可执行程序标题导致的环境变量分配的内存，应该释放
	if (gp_envmem)
	{
		delete[] gp_envmem;
		gp_envmem = NULL;
	}

	// 关闭日志文件
	if (ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)
	{
		close(ngx_log.fd); // 不用判断结果了
		ngx_log.fd = -1;   // 标记下，防止被再次close
	}
}