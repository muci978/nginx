#ifndef __NGX_CONF_H__
#define __NGX_CONF_H__

#include <vector>

#include "ngx_global.h"

class CConfig
{
private:
	CConfig();
	CConfig(const CConfig &);
	CConfig &operator=(const CConfig &);
	~CConfig();

public:
	static CConfig *GetInstance()
	{
		static CConfig c;
		return &c;
	}

public:
	bool Load(const char *pconfName);
	const char *GetString(const char *p_itemname);
	int GetIntDefault(const char *p_itemname, const int def);

public:
	std::vector<LPCConfItem> m_ConfigItemList;
};

#endif
