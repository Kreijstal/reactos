
#define STANDALONE
#include <apitest.h>

extern void func_profileorder(void);
extern void func_wlanapi(void);

const struct test winetest_testlist[] =
{
    { "profileorder", func_profileorder },
    { "wlanapi", func_wlanapi },
    { 0, 0 }
};
