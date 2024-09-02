// 和开启子进程相关
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

// 处理网络事件和定时器事件，遵照nginx引入这个同名函数
void ngx_process_events_and_timers()
{
    g_socket.ngx_epoll_process_events(-1); //-1表示等待

    // 统计信息打印，考虑到测试的时候总会收到各种数据信息，所以上边的函数调用一般都不会卡住等待收数据
    g_socket.printTDInfo();
}
