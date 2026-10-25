
#define STANDALONE
#include <apitest.h>

extern void func_CM_Get_Device_Interface_List(void);
extern void func_CM_MapCrToWin32Err(void);
extern void func_devclass(void);
extern void func_DevQuery(void);
extern void func_SetupDiCreateDeviceInfo(void);
extern void func_SetupDiDeviceProperty(void);
extern void func_SetupDiGetActualModelsSection(void);
extern void func_SetupDiInstallClassExA(void);
extern void func_SetupInstallServicesFromInfSectionEx(void);

const struct test winetest_testlist[] =
{
    { "CM_Get_Device_Interface_List", func_CM_Get_Device_Interface_List },
    { "CM_MapCrToWin32Err", func_CM_MapCrToWin32Err },
    { "devclass", func_devclass },
    { "DevQuery", func_DevQuery },
    { "SetupDiCreateDeviceInfo", func_SetupDiCreateDeviceInfo },
    { "SetupDiDeviceProperty", func_SetupDiDeviceProperty },
    { "SetupDiGetActualModelsSection", func_SetupDiGetActualModelsSection },
    { "SetupDiInstallClassExA", func_SetupDiInstallClassExA },
    { "SetupInstallServicesFromInfSectionEx", func_SetupInstallServicesFromInfSectionEx },
    { 0, 0 }
};
