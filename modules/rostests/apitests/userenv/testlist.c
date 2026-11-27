
#define STANDALONE
#include <apitest.h>

extern void func_DeleteProfile(void);
extern void func_GetProfileDirs(void);
extern void func_LoadUserProfile(void);
extern void func_ShellFolders(void);

const struct test winetest_testlist[] =
{
    { "DeleteProfile", func_DeleteProfile },
    { "GetProfileDirs", func_GetProfileDirs },
    { "LoadUserProfile", func_LoadUserProfile },
    { "ShellFolders", func_ShellFolders },
    { 0, 0 }
};
