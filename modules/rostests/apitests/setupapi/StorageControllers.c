/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that every present PCI mass storage controller is started
 * COPYRIGHT:   ReactOS Developers
 *
 * A storage port driver that can't start (e.g. storport asking a filter
 * device instead of the PDO for its bus type) leaves the controller with a
 * problem code and all its disks and CD drives missing.
 */

#include <apitest.h>

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <strsafe.h>

/* PCI base class 01h: mass storage controller */
static
BOOL
IsMassStorageController(HDEVINFO DevInfo, PSP_DEVINFO_DATA DevData)
{
    WCHAR Ids[2048];
    PCWSTR Id;

    if (!SetupDiGetDeviceRegistryPropertyW(DevInfo, DevData, SPDRP_COMPATIBLEIDS,
                                           NULL, (PBYTE)Ids, sizeof(Ids) - 2 * sizeof(WCHAR), NULL))
    {
        return FALSE;
    }
    Ids[RTL_NUMBER_OF(Ids) - 1] = Ids[RTL_NUMBER_OF(Ids) - 2] = UNICODE_NULL;

    for (Id = Ids; *Id; Id += wcslen(Id) + 1)
    {
        if (_wcsnicmp(Id, L"PCI\\CC_01", 9) == 0)
            return TRUE;
    }
    return FALSE;
}

START_TEST(StorageControllers)
{
    HDEVINFO DevInfo;
    SP_DEVINFO_DATA DevData;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    ULONG Status, Problem, Index, Found = 0;
    CONFIGRET Cr;

    DevInfo = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    ok(DevInfo != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed with %lu\n", GetLastError());
    if (DevInfo == INVALID_HANDLE_VALUE)
        return;

    DevData.cbSize = sizeof(DevData);
    for (Index = 0; SetupDiEnumDeviceInfo(DevInfo, Index, &DevData); Index++)
    {
        if (!IsMassStorageController(DevInfo, &DevData))
            continue;
        Found++;

        if (CM_Get_Device_IDW(DevData.DevInst, InstanceId, RTL_NUMBER_OF(InstanceId), 0) != CR_SUCCESS)
            StringCchCopyW(InstanceId, RTL_NUMBER_OF(InstanceId), L"?");

        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevData.DevInst, 0);
        ok(Cr == CR_SUCCESS, "%ls: CM_Get_DevNode_Status returned %lu\n", InstanceId, Cr);
        if (Cr != CR_SUCCESS)
            continue;

        trace("%ls: status 0x%lx, problem %lu\n", InstanceId, Status, Problem);
        ok(!(Status & DN_HAS_PROBLEM), "%ls: has problem %lu\n", InstanceId, Problem);
        ok(Status & DN_STARTED, "%ls: not started (status 0x%lx)\n", InstanceId, Status);
    }

    if (Found == 0)
        skip("No PCI mass storage controller present\n");

    SetupDiDestroyDeviceInfoList(DevInfo);
}
