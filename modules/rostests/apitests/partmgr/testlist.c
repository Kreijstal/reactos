#define STANDALONE
#include <apitest.h>

extern void func_NtfsFormatType(void);
extern void func_PartitionRelayout(void);
extern void func_StorDeviceNumber(void);

const struct test winetest_testlist[] =
{
    { "NtfsFormatType", func_NtfsFormatType },
    { "PartitionRelayout", func_PartitionRelayout },
    { "StorDeviceNumber", func_StorDeviceNumber },
    { 0, 0 }
};
