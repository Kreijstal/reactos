/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests that driver lists follow INF files added, changed and removed
 *              in %windir%\inf after the INF cache was written
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <apitest.h>
#include <windows.h>
#include <setupapi.h>

static const GUID GUID_DEVCLASS_SAMPLE = {
    0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

static const char InfTemplate[] =
    "[Version]\r\n"
    "Signature=\"$Windows NT$\"\r\n"
    "Class=Ports\r\n"
    "ClassGUID={4D36E978-E325-11CE-BFC1-08002BE10318}\r\n"
    "Provider=%%Mfg%%\r\n"
    "DriverVer=01/01/2026,1.0.0.%u\r\n"
    "\r\n"
    "[Manufacturer]\r\n"
    "%%Mfg%%=Models\r\n"
    "\r\n"
    "[Models]\r\n"
    "%%Desc%%=Inst,ApitestInfCacheRefresh\r\n"
    "\r\n"
    "[Inst]\r\n"
    "\r\n"
    "[Strings]\r\n"
    "Mfg=\"ReactOS apitest\"\r\n"
    "Desc=\"%s\"\r\n";

static BOOL
WriteTestInf(PCWSTR Path, PCSTR Description, UINT Version)
{
    char Buffer[1024];
    HANDLE File;
    DWORD Written;
    int Length;
    BOOL Ret;

    Length = _snprintf(Buffer, sizeof(Buffer), InfTemplate, Version, Description);
    if (Length <= 0)
        return FALSE;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(File, Buffer, Length, &Written, NULL) && Written == (DWORD)Length;
    CloseHandle(File);
    return Ret;
}

/* Builds the compatible driver list and returns how many drivers it has;
 * Description receives the first one's description */
static DWORD
CountDrivers(HDEVINFO DevInfo, PSP_DEVINFO_DATA DevInfoData, PWSTR Description, DWORD Cch)
{
    SP_DRVINFO_DATA_W DrvInfo = { sizeof(DrvInfo) };
    DWORD Count;

    Description[0] = UNICODE_NULL;
    SetupDiDestroyDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER);
    ok(SetupDiBuildDriverInfoList(DevInfo, DevInfoData, SPDIT_COMPATDRIVER),
       "SetupDiBuildDriverInfoList failed: %lu\n", GetLastError());
    for (Count = 0; SetupDiEnumDriverInfoW(DevInfo, DevInfoData, SPDIT_COMPATDRIVER, Count, &DrvInfo); Count++)
    {
        if (Count == 0)
            lstrcpynW(Description, DrvInfo.Description, Cch);
    }
    return Count;
}

START_TEST(InfCacheRefresh)
{
    static const WCHAR Hwid[] = L"ApitestInfCacheRefresh\0";
    WCHAR InfPath[MAX_PATH], Description[LINE_LEN];
    HDEVINFO DevInfo;
    SP_DEVINFO_DATA DevInfoData = { sizeof(DevInfoData) };
    UINT Length;

    Length = GetSystemWindowsDirectoryW(InfPath, ARRAYSIZE(InfPath));
    if (Length == 0 || Length + 40 > ARRAYSIZE(InfPath))
    {
        skip("No Windows directory\n");
        return;
    }
    wcscat(InfPath, L"\\inf\\apitest_infcache.inf");

    if (!WriteTestInf(InfPath, "Apitest driver A", 1))
    {
        skip("Cannot write %ls: %lu\n", InfPath, GetLastError());
        return;
    }

    DevInfo = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SAMPLE, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE ||
        !SetupDiCreateDeviceInfoW(DevInfo, L"ApitestInfCacheRefresh", &GUID_DEVCLASS_SAMPLE,
                                  NULL, NULL, DICD_GENERATE_ID, &DevInfoData) ||
        !SetupDiSetDeviceRegistryPropertyW(DevInfo, &DevInfoData, SPDRP_HARDWAREID,
                                           (const BYTE *)Hwid, sizeof(Hwid)) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DevInfoData))
    {
        skip("Creating the test device failed: %lu\n", GetLastError());
        if (DevInfo != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(DevInfo);
        DeleteFileW(InfPath);
        return;
    }

    /* A new INF is found (and the cache, if writable, is rewritten now) */
    ok_int(CountDrivers(DevInfo, &DevInfoData, Description, ARRAYSIZE(Description)), 1);
    ok(!wcscmp(Description, L"Apitest driver A"), "Description '%ls'\n", Description);

    /* An INF changed after the cache was written is scanned again: its old
     * entries are replaced, not kept beside the new ones. The pause makes the
     * change newer than the cache even on a FAT volume (2 s timestamps). */
    Sleep(2500);
    ok(WriteTestInf(InfPath, "Apitest driver B", 2), "Rewriting %ls failed: %lu\n", InfPath, GetLastError());
    ok_int(CountDrivers(DevInfo, &DevInfoData, Description, ARRAYSIZE(Description)), 1);
    ok(!wcscmp(Description, L"Apitest driver B"), "Description '%ls'\n", Description);

    /* Unchanged since then: the cached entry is used */
    ok_int(CountDrivers(DevInfo, &DevInfoData, Description, ARRAYSIZE(Description)), 1);
    ok(!wcscmp(Description, L"Apitest driver B"), "Description '%ls'\n", Description);

    /* A removed INF no longer provides a driver */
    ok(DeleteFileW(InfPath), "DeleteFileW failed: %lu\n", GetLastError());
    ok_int(CountDrivers(DevInfo, &DevInfoData, Description, ARRAYSIZE(Description)), 0);

    /* And a driver for it is found again once it is back */
    Sleep(2500);
    ok(WriteTestInf(InfPath, "Apitest driver C", 3), "Rewriting %ls failed: %lu\n", InfPath, GetLastError());
    ok_int(CountDrivers(DevInfo, &DevInfoData, Description, ARRAYSIZE(Description)), 1);
    ok(!wcscmp(Description, L"Apitest driver C"), "Description '%ls'\n", Description);

    SetupDiDestroyDriverInfoList(DevInfo, &DevInfoData, SPDIT_COMPATDRIVER);
    ok(SetupDiRemoveDevice(DevInfo, &DevInfoData), "SetupDiRemoveDevice failed: %lu\n", GetLastError());
    SetupDiDestroyDeviceInfoList(DevInfo);
    ok(DeleteFileW(InfPath), "DeleteFileW failed: %lu\n", GetLastError());
}
