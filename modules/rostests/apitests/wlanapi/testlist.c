
#define STANDALONE
#include <apitest.h>

extern void func_acmfallback(void);
extern void func_devrestart(void);
extern void func_notifrace(void);
extern void func_profileedit(void);
extern void func_profilename(void);
extern void func_profileorder(void);
extern void func_profilestore(void);
extern void func_svclife(void);
extern void func_wlanapi(void);

const struct test winetest_testlist[] =
{
    { "acmfallback", func_acmfallback },
    { "devrestart", func_devrestart },
    { "notifrace", func_notifrace },
    { "profileedit", func_profileedit },
    { "profilename", func_profilename },
    { "profileorder", func_profileorder },
    { "profilestore", func_profilestore },
    { "svclife", func_svclife },
    { "wlanapi", func_wlanapi },
    { 0, 0 }
};
