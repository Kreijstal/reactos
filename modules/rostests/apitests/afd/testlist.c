#define STANDALONE
#include <apitest.h>

extern void func_requestlength(void);
extern void func_select(void);
extern void func_send(void);
extern void func_userbuffer(void);
extern void func_windowsize(void);

const struct test winetest_testlist[] =
{
    { "requestlength", func_requestlength },
    { "select", func_select },
    { "send", func_send },
    { "userbuffer", func_userbuffer },
    { "windowsize", func_windowsize },
    { 0, 0 }
};
