#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "ngx_func.h"	
#include "ngx_c_conf.h"

CConfig *CConfig::m_instance = NULL;

CConfig::CConfig()
{
}

CConfig::~CConfig()
{
	std::vector<LPCConfItem>::iterator pos;
	for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{
		delete (*pos);
	}
	m_ConfigItemList.clear();
	return;
}

// 装载配置文件
bool CConfig::Load(const char *pconfName)
{
	FILE *fp;
	fp = fopen(pconfName, "r");
	if (fp == NULL)
		return false;

	// 每一行配置文件读出来都放这里
	char linebuf[501]; // 每行配置保持<500字符，防止出现问题

	while (!feof(fp)) // 检查文件是否结束
	{
		if (fgets(linebuf, 500, fp) == NULL) // 从文件中读数据，每次读一行，一行最多不要超过500个字符
			continue;

		if (linebuf[0] == 0)
			continue;

		// 处理注释行
		if (*linebuf == ';' || *linebuf == ' ' || *linebuf == '#' || *linebuf == '\t' || *linebuf == '\n')
			continue;

	lblprocstring:
		// 截取行尾的换行，回车，空格
		if (strlen(linebuf) > 0)
		{
			if (linebuf[strlen(linebuf) - 1] == 10 || linebuf[strlen(linebuf) - 1] == 13 || linebuf[strlen(linebuf) - 1] == 32)
			{
				linebuf[strlen(linebuf) - 1] = 0;
				goto lblprocstring;
			}
		}
		if (linebuf[0] == 0)
			continue;
		if (*linebuf == '[') //[开头的也不处理
			continue;

		//"xxx=xxx"
		char *ptmp = strchr(linebuf, '=');
		if (ptmp != NULL)
		{
			// 存储一个设置的名和值
			LPCConfItem p_confitem = new CConfItem;
			memset(p_confitem, 0, sizeof(CConfItem));
			strncpy(p_confitem->ItemName, linebuf, (int)(ptmp - linebuf));
			strcpy(p_confitem->ItemContent, ptmp + 1);

			Rtrim(p_confitem->ItemName);
			Ltrim(p_confitem->ItemName);
			Rtrim(p_confitem->ItemContent);
			Ltrim(p_confitem->ItemContent);

			m_ConfigItemList.push_back(p_confitem);
		}
	}

	fclose(fp);
	return true;
}

// 根据ItemName获取配置信息字符串，不修改不用互斥
const char *CConfig::GetString(const char *p_itemname)
{
	std::vector<LPCConfItem>::iterator pos;
	for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{
		if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
			return (*pos)->ItemContent;
	}
	return NULL;
}
// 根据ItemName获取数字类型配置信息，不修改不用互斥
int CConfig::GetIntDefault(const char *p_itemname, const int def)
{
	std::vector<LPCConfItem>::iterator pos;
	for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{
		if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
			return atoi((*pos)->ItemContent);
	}
	return def;
}