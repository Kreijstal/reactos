#define STANDALONE
#include <apitest.h>

extern void func_ServerInfo(void);

const struct test winetest_testlist[] =
{
    { "ServerInfo", func_ServerInfo },
    { 0, 0 }
};
