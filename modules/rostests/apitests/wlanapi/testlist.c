
#define STANDALONE
#include <apitest.h>

extern void func_acmfallback(void);
extern void func_profileorder(void);
extern void func_wlanapi(void);

const struct test winetest_testlist[] =
{
    { "acmfallback", func_acmfallback },
    { "profileorder", func_profileorder },
    { "wlanapi", func_wlanapi },
    { 0, 0 }
};
