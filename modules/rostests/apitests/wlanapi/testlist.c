
#define STANDALONE
#include <apitest.h>

extern void func_acmfallback(void);
extern void func_profileorder(void);
extern void func_profilestore(void);
extern void func_wlanapi(void);

const struct test winetest_testlist[] =
{
    { "acmfallback", func_acmfallback },
    { "profileorder", func_profileorder },
    { "profilestore", func_profilestore },
    { "wlanapi", func_wlanapi },
    { 0, 0 }
};
