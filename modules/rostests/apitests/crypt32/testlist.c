
#define STANDALONE
#include <apitest.h>

extern void func_CertChainSsl(void);
extern void func_CertEnumSystemStoreLocation(void);
extern void func_CertPss(void);

const struct test winetest_testlist[] =
{
    { "CertChainSsl", func_CertChainSsl },
    { "CertEnumSystemStoreLocation", func_CertEnumSystemStoreLocation },
    { "CertPss", func_CertPss },

    { 0, 0 }
};
