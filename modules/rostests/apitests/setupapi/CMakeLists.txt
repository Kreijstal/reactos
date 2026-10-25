
list(APPEND SOURCE
    CM_Get_Device_Interface_List.c
    CM_MapCrToWin32Err.c
    devclass.c
    DevQuery.c
    SetupDiCreateDeviceInfo.c
    SetupDiDeviceProperty.c
    SetupDiInstallClassExA.c
    SetupDiGetActualModelsSection.c
    SetupInstallServicesFromInfSectionEx.c
    SwDevice.c
    testlist.c)

add_executable(setupapi_apitest ${SOURCE})
set_module_type(setupapi_apitest win32cui)
add_importlibs(setupapi_apitest advapi32 setupapi user32 msvcrt kernel32)
add_rostests_file(TARGET setupapi_apitest)
