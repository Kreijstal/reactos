
#define STANDALONE
#include <apitest.h>

extern void func_InitializeSecurityContext(void);
extern void func_QueryContextAttributes(void);

const struct test winetest_testlist[] =
{
    { "InitializeSecurityContext", func_InitializeSecurityContext },
    { "QueryContextAttributes", func_QueryContextAttributes },

    { 0, 0 }
};
