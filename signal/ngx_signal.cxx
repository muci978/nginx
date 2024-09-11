#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"

// 一个信号有关的结构 ngx_signal_t
typedef struct
{
	int signo;			 // 信号对应的编号
	const char *signame; // 信号对应的中文名字
	// 信号处理函数
	void (*handler)(int signo, siginfo_t *siginfo, void *ucontext);
} ngx_signal_t;

// 信号处理函数
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext);
static void ngx_process_get_status(void); // 获取子进程的结束状态，防止子进程变成僵尸进程
static void ngx_shutdown(void);

// 数组，定义本系统处理的各种信号，取一小部分nginx中的信号，并没有全部搬移到这里
// 在实际商业代码中，能想到的要处理的信号，都弄进来
ngx_signal_t signals[] = {
	// signo      signame             handler
	{SIGHUP, "SIGHUP", ngx_signal_handler},
	{SIGINT, "SIGINT", ngx_signal_handler},
	{SIGTERM, "SIGTERM", ngx_signal_handler},
	{SIGCHLD, "SIGCHLD", ngx_signal_handler},
	{SIGQUIT, "SIGQUIT", ngx_signal_handler},
	{SIGIO, "SIGIO", ngx_signal_handler},
	{SIGSYS, "SIGSYS, SIG_IGN", NULL},
	// 把handler设置为NULL，代表要求忽略这个信号，请求操作系统不要执行缺省的该信号处理动作
	// 日后根据需要再继续增加
	{0, NULL, NULL} // 信号对应的数字至少是1，所以可以用0作为一个特殊标记
};

// 初始化信号的函数，用于注册信号处理程序
// 返回值：0成功，-1失败
int ngx_init_signals()
{
	ngx_signal_t *sig;
	struct sigaction sa;

	for (sig = signals; sig->signo != 0; sig++) // 将signo == 0作为一个标记，因为信号的编号都不为0；
	{
		memset(&sa, 0, sizeof(struct sigaction));

		if (sig->handler)
		{
			sa.sa_sigaction = sig->handler;
			sa.sa_flags = SA_SIGINFO;
			// sa_sigaction和sa_handler组成了一个联合体，作用相同，只不过前者参数列表为(int, siginfo_t*, void*);
		}
		else
		{
			sa.sa_handler = SIG_IGN;
		}

		sigemptyset(&sa.sa_mask);

		// 设置信号处理动作
		if (sigaction(sig->signo, &sa, NULL) == -1)
		{
			ngx_log_error_core(NGX_LOG_EMERG, errno, "sigaction(%s) failed", sig->signame);
			return -1;
		}
	}
	return 0;
}

// 信号处理函数
// siginfo：这个系统定义的结构中包含了信号产生原因的有关信息
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
	ngx_signal_t *sig;
	char *action; // 一个字符串，用于记录一个动作字符串以往日志文件中写

	for (sig = signals; sig->signo != 0; sig++) // 遍历信号数组
	{
		if (sig->signo == signo)
		{
			break;
		}
	}

	action = (char *)"";

	if (ngx_process == NGX_PROCESS_MASTER) // master进程
	{
		switch (signo)
		{
		case SIGCHLD:
			ngx_reap = 1; // 标记子进程状态变化，master主进程的for(;;)循环中会用到这个变量，比如重新产生一个子进程
			break;

		case SIGTERM:
			ngx_shutdown();
			break;

		default:
			break;
		}
	}
	else if (ngx_process == NGX_PROCESS_WORKER) // worker进程
	{
		switch (signo)
		{
		case SIGTERM:
			// exit(0);
			g_stopEvent = 1;
			break;

		default:
			break;
		}
	}

	// 记录一些日志信息
	if (siginfo && siginfo->si_pid) // si_pid = sending process ID【发送该信号的进程id】
	{
		ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received from %P%s", signo, sig->signame, siginfo->si_pid, action);
	}
	else
	{
		ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received %s", signo, sig->signame, action); // 没有发送该信号的进程id，所以不显示发送该信号的进程id
	}

	if (signo == SIGCHLD)
	{
		ngx_process_get_status(); // 获取子进程的结束状态
	}

	return;
}

static void ngx_process_get_status(void)
{
	pid_t pid;
	int status;
	int err;
	int one = 0; // 抄自官方nginx，应该是标记信号正常处理过一次

	for (;;)
	{
		// 第一次waitpid返回一个> 0值，表示成功
		// 第二次再循环回来，再次调用waitpid会返回一个0，表示子进程还没结束，然后这里有return来退出；
		pid = waitpid(-1, &status, WNOHANG);

		if (pid == 0) // 子进程没结束，会立即返回这个数字，但这里应该不是这个数字，因为一般是子进程退出时会执行到这个函数
		{
			return;
		}

		if (pid == -1)
		{
			// 这里处理代码抄自官方nginx，主要目的是打印一些日志。
			err = errno;
			if (err == EINTR)
			{
				continue;
			}

			if (err == ECHILD && one) // 没有子进程
			{
				return;
			}

			if (err == ECHILD) // 没有子进程
			{
				ngx_log_error_core(NGX_LOG_INFO, err, "waitpid() failed!");
				return;
			}
			ngx_log_error_core(NGX_LOG_ALERT, err, "waitpid() failed!");
			return;
		}
		--ngx_working_subprocess; // 在回收子进程时记录运行中的子进程数量减一，因为信号可能丢失
		one = 1;				  // 标记waitpid()返回了正常的返回值
		if (WTERMSIG(status))	  // 获取使子进程终止的信号编号
		{
			ngx_log_error_core(NGX_LOG_ALERT, 0, "pid = %P exited on signal %d!", pid, WTERMSIG(status)); // 获取使子进程终止的信号编号
		}
		else
		{
			ngx_log_error_core(NGX_LOG_NOTICE, 0, "pid = %P exited with code %d!", pid, WEXITSTATUS(status)); // WEXITSTATUS()获取子进程传递给exit或者_exit参数的低八位
		}
	}
	return;
}

static void ngx_shutdown(void)
{
	if (g_stopEvent == 1)
	{
		return;
	}
	g_stopEvent = 1;
	ngx_log_error_core(NGX_LOG_NOTICE, 0, "【master进程】准备退出，向所有子进程发送SIGTERM.....!");
	kill(0, SIGTERM);
	while (ngx_working_subprocess > 0)
	{
		ngx_log_error_core(NGX_LOG_NOTICE, 0, "【master进程】等待 %d 个worker进程的退出......", ngx_working_subprocess);
		ngx_process_get_status();
		sleep(1);
	}
}