#ifndef __NGX_C_CRC32_H__
#define __NGX_C_CRC32_H__

#include <stddef.h>

class CCRC32
{
private:
	CCRC32();
	~CCRC32();
	CCRC32(const CCRC32&);
	CCRC32& operator=(const CCRC32&);

public:
	static CCRC32 *GetInstance()
	{
		static CCRC32 c;
		return &c;
	}

public:
	void Init_CRC32_Table();
	unsigned int Reflect(unsigned int ref, char ch);

	int Get_CRC(unsigned char *buffer, unsigned int dwSize);

public:
	unsigned int crc32_table[256];
};

#endif
