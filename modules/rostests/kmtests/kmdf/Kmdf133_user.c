/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF 1.33 client test, user-mode part
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include <kmt_test.h>

#include "../kmtest/kmtest.h"
#include "Kmdf133.h"

/*
 * cdrom.sys is a KMDF driver. When the machine has a CD-ROM drive, read the
 * ISO 9660 volume descriptor through it as a functional check of a real
 * KMDF client running on the current framework.
 */
static
VOID
TestCdRom(VOID)
{
    HANDLE Device;
    PUCHAR Sector;
    DWORD Read = 0;
    BOOL Ok;

    Device = CreateFileW(L"\\\\.\\CdRom0", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE)
    {
        skip(FALSE, "No CD-ROM drive (error %lu)\n", GetLastError());
        return;
    }

    Sector = VirtualAlloc(NULL, 2048, MEM_COMMIT, PAGE_READWRITE);
    ok(Sector != NULL, "VirtualAlloc failed\n");
    if (Sector)
    {
        ok(SetFilePointer(Device, 16 * 2048, NULL, FILE_BEGIN) == 16 * 2048,
           "SetFilePointer failed with %lu\n", GetLastError());
        Ok = ReadFile(Device, Sector, 2048, &Read, NULL);
        if (!Ok && GetLastError() == ERROR_NOT_READY)
        {
            skip(FALSE, "No medium in the CD-ROM drive\n");
            VirtualFree(Sector, 0, MEM_RELEASE);
            CloseHandle(Device);
            return;
        }
        ok(Ok, "ReadFile failed with %lu\n", GetLastError());
        ok_eq_ulong(Read, 2048UL);
        if (Ok && Read == 2048)
        {
            ok(Sector[0] == 1 && memcmp(Sector + 1, "CD001", 5) == 0,
               "No primary volume descriptor at sector 16\n");
            trace("CdRom0 sector 16: %02x %.5s\n", Sector[0], (PCSTR)Sector + 1);
        }
        VirtualFree(Sector, 0, MEM_RELEASE);
    }
    CloseHandle(Device);
}

START_TEST(Kmdf133)
{
    SC_HANDLE CxService = NULL;
    DWORD Error;

    /*
     * The client binds to the KmtCx class extension before its DriverEntry
     * runs; WDFLDR loads the extension through its default service name.
     */
    Error = KmtCreateService(KMTCX_SERVICE_NAME, KMTCX_IMAGE_NAME, NULL, &CxService);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);

    Error = KmtLoadAndOpenDriver(L"Kmdf133", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (!Error)
    {
        Error = KmtSendToDriver(IOCTL_KMDF133_RUN_TESTS);
        ok_eq_int(Error, ERROR_SUCCESS);

        KmtCloseDriver();
        KmtUnloadDriver();
    }

    if (CxService)
        KmtDeleteService(KMTCX_SERVICE_NAME, &CxService);

    TestCdRom();
}
