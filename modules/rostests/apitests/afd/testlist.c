#define STANDALONE
#include <apitest.h>

extern void func_select(void);
extern void func_send(void);
extern void func_windowsize(void);

const struct test winetest_testlist[] =
{
    { "select", func_select },
    { "send", func_send },
    { "windowsize", func_windowsize },
    { 0, 0 }
};
