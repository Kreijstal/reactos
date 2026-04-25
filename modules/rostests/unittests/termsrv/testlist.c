#define STANDALONE
#include <apitest.h>

extern void func_RdpPeer(void);

const struct test winetest_testlist[] =
{
    { "RdpPeer", func_RdpPeer },
    { 0, 0 }
};

