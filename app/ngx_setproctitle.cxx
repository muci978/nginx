#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> 
#include <string.h>

#include "ngx_global.h"

// 设置可执行程序标题相关函数：分配内存，并且把环境变量拷贝到新内存中来
void ngx_init_setproctitle()
{
    // 这里无需判断penvmen == NULL,有些编译器new会返回NULL，有些会报异常，如果在重要的地方new失败了，让程序失控崩溃
    gp_envmem = new char[g_envneedmem];
    memset(gp_envmem, 0, g_envneedmem);

    char *ptmp = gp_envmem;
    // 把原来的内存内容搬到新地方来
    for (int i = 0; environ[i]; i++)
    {
        size_t size = strlen(environ[i]) + 1;
        strcpy(ptmp, environ[i]);             // 把原环境变量内容拷贝到新地方
        environ[i] = ptmp;                    // 让新环境变量指向这段新内存
        ptmp += size;
    }
    return;
}

// 设置可执行程序标题
void ngx_setproctitle(const char *title)
{
    // 假设所有的命令行参数都不需要用到了，可以被随意覆盖了；
    // 注意：标题长度不会长到原始标题和原始环境变量都装不下，否则怕出问题，不处理

    // 计算新标题长度
    size_t ititlelen = strlen(title);

    // 计算总的原始的argv那块内存的总长度
    size_t esy = g_argvneedmem + g_envneedmem;
    if (esy <= ititlelen)
    {
        return;
    }

    // 设置后续的命令行参数为空，表示只有argv[]中只有一个元素了，防止后续argv被滥用，因为很多判断是用argv[] == NULL来做结束标记判断的
    g_os_argv[1] = NULL;

    // 把标题弄进来，原来的命令行参数都会被覆盖掉，不要再使用这些命令行参数,而且g_os_argv[1]已经被设置为NULL了
    char *ptmp = g_os_argv[0];
    strcpy(ptmp, title);
    ptmp += ititlelen;

    // 把剩余的原argv以及environ所占的内存全部清0，否则会出现在ps的cmd列可能还会残余一些没有被覆盖的内容；
    size_t cha = esy - ititlelen;
    memset(ptmp, 0, cha);
    return;
}
