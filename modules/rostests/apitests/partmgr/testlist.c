#define STANDALONE
#include <apitest.h>

extern void func_NtfsFormatType(void);
extern void func_PartitionRelayout(void);
extern void func_PartitionSameIdentity(void);
extern void func_StorDeviceNumber(void);

const struct test winetest_testlist[] =
{
    { "NtfsFormatType", func_NtfsFormatType },
    { "PartitionRelayout", func_PartitionRelayout },
    { "PartitionSameIdentity", func_PartitionSameIdentity },
    { "StorDeviceNumber", func_StorDeviceNumber },
    { 0, 0 }
};
