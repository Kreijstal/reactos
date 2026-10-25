/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for CM_MapCrToWin32Err
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <cfgmgr32.h>

START_TEST(CM_MapCrToWin32Err)
{
    DWORD (WINAPI *pCM_MapCrToWin32Err)(CONFIGRET, DWORD);
    static const struct { CONFIGRET Cr; DWORD Error; } Map[] =
    {
        { CR_SUCCESS, ERROR_SUCCESS },
        { CR_DEFAULT, 0xdead },
        { CR_OUT_OF_MEMORY, ERROR_NOT_ENOUGH_MEMORY },
        { CR_INVALID_POINTER, ERROR_INVALID_USER_BUFFER },
        { CR_INVALID_FLAG, ERROR_INVALID_FLAGS },
        { CR_INVALID_DEVNODE, ERROR_INVALID_DATA },
        { CR_INVALID_RES_DES, 0xdead },
        { CR_NO_SUCH_DEVNODE, ERROR_NOT_FOUND },
        { CR_ALREADY_SUCH_DEVNODE, ERROR_ALREADY_EXISTS },
        { CR_FAILURE, 0xdead },
        { CR_BUFFER_SMALL, ERROR_INSUFFICIENT_BUFFER },
        { CR_NO_REGISTRY_HANDLE, ERROR_INVALID_HANDLE },
        { CR_REGISTRY_ERROR, ERROR_REGISTRY_CORRUPT },
        { CR_INVALID_DEVICE_ID, ERROR_INVALID_DATA },
        { CR_NO_SUCH_VALUE, ERROR_NOT_FOUND },
        { CR_NO_SUCH_REGISTRY_KEY, ERROR_FILE_NOT_FOUND },
        { CR_INVALID_MACHINENAME, ERROR_INVALID_DATA },
        { CR_REMOTE_COMM_FAILURE, ERROR_SERVICE_NOT_ACTIVE },
        { CR_MACHINE_UNAVAILABLE, ERROR_SERVICE_NOT_ACTIVE },
        { CR_NO_CM_SERVICES, ERROR_SERVICE_NOT_ACTIVE },
        { CR_ACCESS_DENIED, ERROR_ACCESS_DENIED },
        { CR_CALL_NOT_IMPLEMENTED, ERROR_CALL_NOT_IMPLEMENTED },
        { CR_INVALID_PROPERTY, ERROR_INVALID_DATA },
        { CR_NO_SUCH_DEVICE_INTERFACE, ERROR_NOT_FOUND },
        { CR_INVALID_REFERENCE_STRING, ERROR_INVALID_DATA },
        { 0x1000, 0xdead },
    };
    ULONG i;

    /* Windows exports it from cfgmgr32 only */
    pCM_MapCrToWin32Err = (PVOID)GetProcAddress(LoadLibraryW(L"cfgmgr32.dll"), "CM_MapCrToWin32Err");
    if (!pCM_MapCrToWin32Err)
    {
        skip("CM_MapCrToWin32Err is not available\n");
        return;
    }
    for (i = 0; i < ARRAYSIZE(Map); i++)
    {
        DWORD Error = pCM_MapCrToWin32Err(Map[i].Cr, 0xdead);
        ok(Error == Map[i].Error, "CR 0x%lx mapped to %lu, expected %lu\n", Map[i].Cr, Error, Map[i].Error);
    }
}
