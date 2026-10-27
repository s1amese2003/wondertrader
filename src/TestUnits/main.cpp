#include "gtest/gtest/gtest.h"

#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

int main(int argc, char **argv)
{
	testing::InitGoogleTest(&argc, argv);
	int ret =  RUN_ALL_TESTS();
	//CTest下运行时不能等待输入，否则会一直挂到超时
	if (getenv("WT_NO_PAUSE") == NULL && isatty(fileno(stdin)))
	{
		printf("press enter key to exit\n");
		getchar();
	}
	return ret;
}
