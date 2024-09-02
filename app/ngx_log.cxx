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

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"
#include "ngx_c_conf.h"

// 错误等级，和ngx_macro.h里定义的日志等级宏是一一对应关系
static u_char err_levels[][20] =
	{
		{"stderr"}, // 0：控制台错误
		{"emerg"},	// 1：紧急
		{"alert"},	// 2：警戒
		{"crit"},	// 3：严重
		{"error"},	// 4：错误
		{"warn"},	// 5：警告
		{"notice"}, // 6：注意
		{"info"},	// 7：信息
		{"debug"}	// 8：调试
};
ngx_log_t ngx_log;

void ngx_log_stderr(int err, const char *fmt, ...)
{
	va_list args;						  // 创建一个va_list类型变量
	u_char errstr[NGX_MAX_ERROR_STR + 1]; // 2048
	u_char *p, *last;

	memset(errstr, 0, sizeof(errstr));

	last = errstr + NGX_MAX_ERROR_STR;
	// last指向整个buffer最后去了，作为一个标记，防止输出内容超过这么长,

	p = ngx_cpymem(errstr, "nginx: ", 7); // p指向"nginx: "之后

	va_start(args, fmt);				   // 使args指向起始的参数
	p = ngx_vslprintf(p, last, fmt, args); // 组合出这个字符串保存在errstr里
	va_end(args);						   // 释放args

	if (err)
	{
		p = ngx_log_errno(p, last, err);
	}

	// 若位置不够，那换行也要硬插入到末尾，哪怕覆盖到其他内容
	if (p >= (last - 1))
	{
		p = (last - 1) - 1; // 把尾部空格留出来
	}
	*p++ = '\n';

	// 往标准错误【屏幕】输出信息
	write(STDERR_FILENO, errstr, p - errstr);

	if (ngx_log.fd > STDERR_FILENO) // 如果这是个有效的日志文件，本条件肯定成立，此时也才有意义将这个信息写到日志文件
	{
		// 因为上边已经把err信息显示出来了，所以这里就不要显示了，否则显示重复了
		err = 0; // 不要再次把错误信息弄到字符串里，否则字符串里重复了
		p--;
		*p = 0; // 把原来末尾的\n去掉，因为到ngx_log_err_core中还会加这个\n
		ngx_log_error_core(NGX_LOG_STDERR, err, (const char *)errstr);
	}
	return;
}

// 描述：给一段内存，一个错误编号，组合出一个字符串，形如：(错误编号: 错误原因)，放到给的这段内存中去
// buf：是个内存，要往这里保存数据
// last：放的数据不要超过这里
// err：错误编号，取得这个错误编号对应的错误字符串，保存到buffer中
u_char *ngx_log_errno(u_char *buf, u_char *last, int err)
{
	char *perrorinfo = strerror(err); // 返回系统错误码对应的错误信息
	size_t len = strlen(perrorinfo);

	// 然后还要插入一些字符串：(%d:)
	char leftstr[10] = {0};
	sprintf(leftstr, " (%d: ", err);
	size_t leftlen = strlen(leftstr);

	char rightstr[] = ") ";
	size_t rightlen = strlen(rightstr);

	size_t extralen = leftlen + rightlen; // 左右的额外宽度
	if ((buf + len + extralen) < last)
	{
		// 保证整个我装得下，我就装，否则我全部抛弃
		buf = ngx_cpymem(buf, leftstr, leftlen);
		buf = ngx_cpymem(buf, perrorinfo, len);
		buf = ngx_cpymem(buf, rightstr, rightlen);
	}
	return buf;
}

// 往日志文件中写日志，代码中有自动加换行符，所以调用时字符串不用刻意加\n；
// 重定向为标准错误，直接往屏幕上写日志，比如日志文件打不开，则会直接定位到标准错误，此时日志就打印到屏幕上
// level:一个等级数字，把日志分成一些等级
// err：是个错误代码，如果不是0，就应该转换成显示对应的错误信息,一起写到日志文件中，
// ngx_log_error_core(5,8,"这个XXX工作的有问题,显示的结果是=%s","YYYY");
void ngx_log_error_core(int level, int err, const char *fmt, ...)
{
	u_char *last;
	u_char errstr[NGX_MAX_ERROR_STR + 1];

	memset(errstr, 0, sizeof(errstr));
	last = errstr + NGX_MAX_ERROR_STR;

	struct timeval tv;
	struct tm tm;
	time_t sec; // 秒
	u_char *p;	// 指向当前要拷贝数据到其中的内存位置
	va_list args;

	memset(&tv, 0, sizeof(struct timeval));
	memset(&tm, 0, sizeof(struct tm));

	gettimeofday(&tv, NULL); // 获取当前时间，返回自1970-01-01 00:00:00到现在经历的秒数

	sec = tv.tv_sec;		// 秒
	localtime_r(&sec, &tm); // 把参数1的time_t转换为本地时间，保存到参数2中去
	tm.tm_mon++;			// 月份要调整下正常
	tm.tm_year += 1900;		// 年份要调整下才正常

	u_char strcurrtime[40] = {0}; // 先组合出一个当前时间字符串
	ngx_slprintf(strcurrtime,
				 (u_char *)-1,					 // 若用一个u_char *接一个 (u_char *)-1,则 得到的结果是 0xffffffff....，这个值足够大
				 "%4d/%02d/%02d %02d:%02d:%02d", // 格式是 年/月/日 时:分:秒
				 tm.tm_year, tm.tm_mon,
				 tm.tm_mday, tm.tm_hour,
				 tm.tm_min, tm.tm_sec);
	p = ngx_cpymem(errstr, strcurrtime, strlen((const char *)strcurrtime)); // 日期增加进来
	p = ngx_slprintf(p, last, " [%s] ", err_levels[level]);					// 日志级别增加进来
	p = ngx_slprintf(p, last, "%P: ", ngx_pid);								// 支持%P格式，进程id增加进来

	va_start(args, fmt);				   // 使args指向起始的参数
	p = ngx_vslprintf(p, last, fmt, args); // 把fmt和args参数弄进去，组合出来这个字符串
	va_end(args);						   // 释放args

	if (err)
	{
		p = ngx_log_errno(p, last, err);
	}
	// 若位置不够，那换行也要硬插入到末尾，哪怕覆盖到其他内容
	if (p >= (last - 1))
	{
		p = (last - 1) - 1; // 把尾部空格留出来
	}
	*p++ = '\n'; // 增加个换行符

	ssize_t n;
	while (1)
	{
		if (level > ngx_log.log_level)
		{
			// 等级数字太大，比配置文件中的数字大
			// 这种日志就不打印了
			break;
		}

		// 写日志文件
		n = write(ngx_log.fd, errstr, p - errstr);
		if (n == -1)
		{
			// 写失败有问题
			if (errno == ENOSPC) // 写失败，且原因是磁盘没空间了
			{
				// 磁盘没空间了
				// do nothing
			}
			else
			{
				// 这是有其他错误，考虑把这个错误显示到标准错误设备
				if (ngx_log.fd != STDERR_FILENO) // 当前是定位到文件的，则条件成立
				{
					n = write(STDERR_FILENO, errstr, p - errstr);
				}
			}
		}
		break;
	}
	return;
}

// 描述：日志初始化
void ngx_log_init()
{
	u_char *plogname = NULL;
	size_t nlen;

	// 从配置文件中读取和日志相关的配置信息
	CConfig *p_config = CConfig::GetInstance();
	plogname = (u_char *)p_config->GetString("Log");
	if (plogname == NULL)
	{
		// 没读到就要给个缺省的路径文件名
		plogname = (u_char *)NGX_ERROR_LOG_PATH; //"logs/error.log" ,logs目录需要提前建立出来
	}
	ngx_log.log_level = p_config->GetIntDefault("LogLevel", NGX_LOG_NOTICE); // 缺省日志等级为6，如果读失败，就给缺省日志等级

	ngx_log.fd = open((const char *)plogname, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (ngx_log.fd == -1) // 如果有错误，则直接定位到 标准错误上去
	{
		ngx_log_stderr(errno, "[alert] could not open error log file: open() \"%s\" failed", plogname);
		ngx_log.fd = STDERR_FILENO; // 直接定位到标准错误去了
	}
	return;
}