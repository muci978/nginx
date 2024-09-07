#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"
#include "ngx_c_slogic.h"

static void ngx_start_worker_processes(int threadnums);
static int ngx_spawn_process(int threadnums, const char *pprocname);
static void ngx_worker_process_cycle(int inum, const char *pprocname);
static void ngx_worker_process_init(int inum);

static u_char master_process[] = "master process";

// 描述：创建worker子进程
void ngx_master_process_cycle()
{
    sigset_t set;

    sigemptyset(&set);

    // 下列这些信号在执行本函数期间不希望收到（保护不希望由信号中断的代码临界区）
    // fork()子进程防止信号的干扰；
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGWINCH);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGQUIT);

    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_master_process_cycle()中sigprocmask()失败!");
    }
    // sigprocmask失败，程序流程也继续往下走

    // 设置主进程标题
    size_t size;
    int i;
    size = sizeof(master_process);
    size += g_argvneedmem;
    if (size < 1000) // 长度小于这个才设置标题
    {
        char title[1000] = {0};
        strcpy(title, (const char *)master_process); //"master process"
        strcat(title, " ");                          // 跟一个空格分开一些，清晰    //"master process "
        for (i = 0; i < g_os_argc; i++)              //"master process ./nginx"
        {
            strcat(title, g_os_argv[i]);
        }
        ngx_setproctitle(title); // 设置标题
        ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P 【master进程】启动并开始运行......!", title, ngx_pid);
    }

    // 从配置文件中读取要创建的worker进程数量
    CConfig *p_config = CConfig::GetInstance();                      // 单例类
    int workprocess = p_config->GetIntDefault("WorkerProcesses", 1); // 从配置文件中得到要创建的worker进程数量
    ngx_start_worker_processes(workprocess);                         // 这里要创建worker子进程

    // 创建子进程后，父进程的执行流程会返回到这里，子进程不会走进来
    sigemptyset(&set); // 信号屏蔽字为空，表示不屏蔽任何信号

    for (;;)
    {
        // 用于临时替换当前的信号屏蔽字并挂起进程，直到捕捉到一个信号。它常用于实现原子信号等待，即在等待信号时不会丢失任何信号。
        sigsuspend(&set); // 阻塞在这里，等待一个信号，此时进程是挂起的，不占用cpu时间，只有收到信号才会被唤醒（返回）；

        // master进程关闭，退出到main函数回收资源
        if (g_stopEvent == 1)
        {
            break;
        }

        // 当master进程不退出并且有子进程结束时，重新创建子进程补上
        if (ngx_reap == 1)
        {
            // 需要重启的子进程个数
            int restart = workprocess - ngx_working_subprocess;
            ngx_log_error_core(NGX_LOG_NOTICE, 0, "【master进程】重新启动 %d 个worker进程......!", restart);
            ngx_start_worker_processes(restart);
            ngx_reap = 0;
        }
    }
    return;
}

// 描述：根据给定的参数创建指定数量的子进程，因为以后可能要扩展功能，增加参数，所以单独写成一个函数
// threadnums:要创建的子进程数量
static void ngx_start_worker_processes(int threadnums)
{
    int i;
    for (i = 0; i < threadnums; i++)
    {
        ngx_spawn_process(i, "worker process");
        if (ngx_process == NGX_PROCESS_WORKER)
        {
            exit(0);
        }
    }
    return;
}

// 描述：产生一个子进程
// inum：进程编号【0开始】
// pprocname：子进程名字"worker process"
static int ngx_spawn_process(int inum, const char *pprocname)
{
    pid_t pid;

    pid = fork();
    switch (pid)
    {
    case -1:
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_spawn_process()fork()产生子进程num=%d,procname=\"%s\"失败!", inum, pprocname);
        return -1;

    case 0:
        ngx_parent = ngx_pid;
        ngx_pid = getpid();
        usleep(100);
        ngx_worker_process_cycle(inum, pprocname); // 所有worker子进程在这个函数里不断循环
        break;

    default:
        ++ngx_working_subprocess;
        break;
    }
    return pid;
}

// 描述：worker子进程的功能函数，每个woker子进程就在这里无限循环（处理网络事件和定时器事件以对外提供web服务）
// inum：进程编号，0开始
static void ngx_worker_process_cycle(int inum, const char *pprocname)
{
    ngx_process = NGX_PROCESS_WORKER; // 设置进程的类型，是worker进程

    // 重新为子进程设置进程名
    ngx_worker_process_init(inum);
    ngx_setproctitle(pprocname); // 设置标题
    ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P 【worker进程】启动并开始运行, 父进程是 %P", pprocname, ngx_pid, ngx_parent);
    for (;;)
    {
        ngx_process_events_and_timers(); // 处理网络事件和定时器事件
        if (g_stopEvent == 1)
        {
            break;
        }
    }
    // 如果从这个循环跳出来
    g_threadpool.StopAll();      // 考虑在这里停止线程池；
    g_socket.Shutdown_subproc(); // socket需要释放的东西考虑释放
    return;
}

// 描述：子进程创建时调用本函数进行一些初始化工作
static void ngx_worker_process_init(int inum)
{
    sigset_t set; // 信号集

    sigemptyset(&set);                              // 清空信号集
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) // 原来是屏蔽那10个信号，现在不再屏蔽任何信号
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_worker_process_init()中sigprocmask()失败!");
    }

    // 线程池代码，率先创建，至少要比和socket相关的内容优先
    // 线程池里的代码是用于处理业务
    CConfig *p_config = CConfig::GetInstance();
    int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount", 5); // 处理接收到的消息的线程池中线程数量
    if (g_threadpool.Create(tmpthreadnums) == false)                              // 创建线程池中线程
    {
        exit(-2);
    }
    sleep(1);
    if (g_socket.Initialize_subproc() == false) // 初始化子进程需要具备的一些多线程能力相关的信息
    {
        exit(-2);
    }

    if (g_socket.ngx_open_listening_sockets() == false) // 打开监听端口
    {
        exit(-2);
    }
    g_socket.ngx_epoll_init(); // 初始化epoll相关内容，同时往监听socket上增加监听事件，从而开始让监听端口履行其职责
    return;
}
