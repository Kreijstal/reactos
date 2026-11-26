#define STANDALONE
#include <apitest.h>

extern void func_core(void);
extern void func_windowprops(void);
extern void func_security(void);

const struct test winetest_testlist[] =
{
    { "core", func_core },
    { "windowprops", func_windowprops },
    { "security", func_security },
    { 0, 0 }
};
