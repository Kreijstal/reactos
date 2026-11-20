
#define STANDALONE
#include <apitest.h>

extern void func_power(void);
extern void func_scheme(void);

const struct test winetest_testlist[] =
{
    { "power", func_power },
    { "scheme", func_scheme },
    { 0, 0 }
};
