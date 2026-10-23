/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the CM_Get_Device_Interface_List* functions
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>

#include <stdio.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <ntddstor.h>

/* The ANSI functions must give what the Unicode ones give for a device */
static
VOID
CheckDevice(
    _In_ PCSTR DeviceIdA,
    _In_ PCWSTR DeviceIdW)
{
    ULONG LengthA = 0, LengthW = 0;
    PSTR ListA;
    PWSTR ListW;
    CHAR Converted[2048];
    CONFIGRET Cr;

    Cr = CM_Get_Device_Interface_List_SizeW(&LengthW, (LPGUID)&GUID_DEVINTERFACE_DISK,
                                            (DEVINSTID_W)DeviceIdW, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    ok(Cr == CR_SUCCESS, "CM_Get_Device_Interface_List_SizeW(%s) returned %lu\n", DeviceIdA, Cr);
    Cr = CM_Get_Device_Interface_List_SizeA(&LengthA, (LPGUID)&GUID_DEVINTERFACE_DISK,
                                            (DEVINSTID_A)DeviceIdA, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    ok(Cr == CR_SUCCESS, "CM_Get_Device_Interface_List_SizeA(%s) returned %lu\n", DeviceIdA, Cr);
    ok(LengthA == LengthW, "Length %lu, expected %lu\n", LengthA, LengthW);
    if (Cr != CR_SUCCESS || LengthW == 0 || LengthW > sizeof(Converted))
        return;

    ListW = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, LengthW * sizeof(WCHAR));
    ListA = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, LengthW);
    if (!ListW || !ListA)
    {
        skip("Out of memory\n");
        goto Done;
    }

    Cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_DISK, (DEVINSTID_W)DeviceIdW,
                                       ListW, LengthW, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    ok(Cr == CR_SUCCESS, "CM_Get_Device_Interface_ListW(%s) returned %lu\n", DeviceIdA, Cr);
    Cr = CM_Get_Device_Interface_ListA((LPGUID)&GUID_DEVINTERFACE_DISK, (DEVINSTID_A)DeviceIdA,
                                       ListA, LengthW, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    ok(Cr == CR_SUCCESS, "CM_Get_Device_Interface_ListA(%s) returned %lu\n", DeviceIdA, Cr);
    if (Cr == CR_SUCCESS)
    {
        PWSTR Interface;
        HANDLE Handle;

        /* Win32 paths that CreateFile opens */
        for (Interface = ListW; *Interface; Interface += wcslen(Interface) + 1)
        {
            ok(wcsncmp(Interface, L"\\\\?\\", 4) == 0, "Interface %S\n", Interface);
            Handle = CreateFileW(Interface, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            ok(Handle != INVALID_HANDLE_VALUE, "Opening %S failed with %lu\n", Interface, GetLastError());
            if (Handle != INVALID_HANDLE_VALUE)
                CloseHandle(Handle);
        }
        WideCharToMultiByte(CP_ACP, 0, ListW, LengthW, Converted, sizeof(Converted), NULL, NULL);
        ok(memcmp(ListA, Converted, LengthW) == 0, "Lists differ: %s, expected %s\n", ListA, Converted);
        ok(ListA[0] != ANSI_NULL, "No disk interface for %s\n", DeviceIdA);
    }

Done:
    HeapFree(GetProcessHeap(), 0, ListW);
    HeapFree(GetProcessHeap(), 0, ListA);
}

START_TEST(CM_Get_Device_Interface_List)
{
    SP_DEVINFO_DATA DevInfo;
    HDEVINFO Set;
    CHAR IdA[MAX_DEVICE_ID_LEN];
    WCHAR IdW[MAX_DEVICE_ID_LEN];
    DWORD Index;

    Set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    ok(Set != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed with %lu\n", GetLastError());
    if (Set == INVALID_HANDLE_VALUE)
        return;

    DevInfo.cbSize = sizeof(DevInfo);
    for (Index = 0; SetupDiEnumDeviceInfo(Set, Index, &DevInfo); Index++)
    {
        if (!SetupDiGetDeviceInstanceIdA(Set, &DevInfo, IdA, sizeof(IdA), NULL) ||
            !SetupDiGetDeviceInstanceIdW(Set, &DevInfo, IdW, ARRAYSIZE(IdW), NULL))
        {
            ok(FALSE, "SetupDiGetDeviceInstanceId failed with %lu\n", GetLastError());
            continue;
        }
        CheckDevice(IdA, IdW);
    }
    ok(Index > 0, "No disk found\n");

    SetupDiDestroyDeviceInfoList(Set);
}
