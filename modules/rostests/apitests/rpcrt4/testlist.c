#define STANDALONE
#include <apitest.h>

extern void func_RpcMgmtWaitServerListen(void);

const struct test winetest_testlist[] =
{
    { "RpcMgmtWaitServerListen", func_RpcMgmtWaitServerListen },
    { 0, 0 }
};
