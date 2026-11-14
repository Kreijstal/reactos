#define STANDALONE
#include <apitest.h>

extern void func_attrib(void);
extern void func_attrib_link(void);
extern void func_cd(void);
extern void func_copy(void);
extern void func_echo(void);
extern void func_exit(void);
extern void func_fc(void);
extern void func_pushd(void);
extern void func_quote(void);
extern void func_ren(void);
extern void func_robocopy(void);

const struct test winetest_testlist[] =
{
    { "attrib", func_attrib },
    { "attrib_link", func_attrib_link },
    { "cd", func_cd },
    { "copy", func_copy },
    { "echo", func_echo },
    { "exit", func_exit },
    { "fc", func_fc },
    { "pushd", func_pushd },
    { "quote", func_quote },
    { "ren", func_ren },
    { "robocopy", func_robocopy },
    { 0, 0 }
};
