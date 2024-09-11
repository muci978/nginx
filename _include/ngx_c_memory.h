
#ifndef __NGX_MEMORY_H__
#define __NGX_MEMORY_H__

#include <stddef.h>
class CMemory
{
private:
	CMemory() {}
	~CMemory() {}
	CMemory(const CMemory&);
	CMemory& operator=(const CMemory&);

public:
	static CMemory *GetInstance()
	{
		static CMemory c;
		return &c;
	}

public:
	void *AllocMemory(int memCount, bool ifmemset);
	void FreeMemory(void *point);
};

#endif
