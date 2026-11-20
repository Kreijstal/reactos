
#define STANDALONE
#include <apitest.h>

extern void func_InitializeSecurityContext(void);

const struct test winetest_testlist[] =
{
    { "InitializeSecurityContext", func_InitializeSecurityContext },

    { 0, 0 }
};
