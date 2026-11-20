/*
 * Copyright (C) 2005 Benjamin Cutler
 * Copyright (C) 2008 Dmitry Chapyshev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#define NTOS_MODE_USER
#include <ndk/pofuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>
#include <powrprof.h>
#include <wine/debug.h>
#include <wine/unicode.h>

WINE_DEFAULT_DEBUG_CHANNEL(powrprof);


static const WCHAR szPowerCfgSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg";
static const WCHAR szUserPowerConfigSubKey[] =
    L"Control Panel\\PowerCfg";
static const WCHAR szCurrentPowerPolicies[] =
    L"CurrentPowerPolicy";
static const WCHAR szPolicies[] = L"Policies";
static const WCHAR szName[] = L"Name";
static const WCHAR szDescription[] = L"Description";
static const WCHAR szDiskMax[] = L"DiskSpindownMax";
static const WCHAR szDiskMin[] = L"DiskSpindownMin";
static const WCHAR szLastID[] = L"LastID";

static UINT g_LastID = (UINT)-1;
static HANDLE PPRegSemaphore = NULL;


/**
 * @brief
 * Creates a security descriptor for the registry semaphore.
 *
 * @param[out]  PowrProfSd
 * A pointer to an allocated security descriptor for the semaphore.
 *
 * @return
 * Returns TRUE if the function succeeds, otherwise FALSE is returned.
 *
 * @remarks
 * Authenticated users are only given a subset of specific rights for
 * the semaphore access, local system and administrators have full power.
 **/
static BOOLEAN
CreatePwrProfSemaphoreSecurity(
    _Out_ PSECURITY_DESCRIPTOR *PowrProfSd)
{
    BOOLEAN Success = FALSE;
    PSID AuthenticatedUsersSid = NULL, SystemSid = NULL, AdminsSid = NULL;
    PACL Dacl = NULL;
    PSECURITY_DESCRIPTOR RelSd = NULL;
    ULONG DaclSize, RelSDSize = 0;
    SECURITY_DESCRIPTOR AbsSd;
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  1,
                                  SECURITY_AUTHENTICATED_USER_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  &AuthenticatedUsersSid))
    {
        return FALSE;
    }

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  1,
                                  SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  &SystemSid))
    {
        goto Quit;
    }

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &AdminsSid))
    {
        goto Quit;
    }

    if (!InitializeSecurityDescriptor(&AbsSd, SECURITY_DESCRIPTOR_REVISION))
        goto Quit;

    /* Compute the size needed for the DACL and allocate it */
    DaclSize = sizeof(ACL) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(AuthenticatedUsersSid) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(SystemSid) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(AdminsSid);

    Dacl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DaclSize);
    if (!Dacl || !InitializeAcl(Dacl, DaclSize, ACL_REVISION))
        goto Quit;

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SYNCHRONIZE | READ_CONTROL | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE,
                             AuthenticatedUsersSid))
    {
        goto Quit;
    }

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SEMAPHORE_ALL_ACCESS,
                             SystemSid))
    {
        goto Quit;
    }

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SEMAPHORE_ALL_ACCESS,
                             AdminsSid))
    {
        goto Quit;
    }

    if (!SetSecurityDescriptorDacl(&AbsSd, TRUE, Dacl, FALSE))
        goto Quit;

    if (!SetSecurityDescriptorOwner(&AbsSd, AdminsSid, FALSE))
        goto Quit;

    if (!SetSecurityDescriptorGroup(&AbsSd, SystemSid, FALSE))
        goto Quit;

    /* Retrieve the size needed for the relative SD */
    if (MakeSelfRelativeSD(&AbsSd, NULL, &RelSDSize) ||
        (GetLastError() != ERROR_INSUFFICIENT_BUFFER))
    {
        goto Quit;
    }

    /* Build the relative SD */
    RelSd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RelSDSize);
    if (!RelSd || !MakeSelfRelativeSD(&AbsSd, RelSd, &RelSDSize))
    {
        if (RelSd)
            HeapFree(GetProcessHeap(), 0, RelSd);
        goto Quit;
    }

    *PowrProfSd = RelSd;
    Success = TRUE;

Quit:
    if (Dacl)
        HeapFree(GetProcessHeap(), 0, Dacl);

    if (AdminsSid)
        FreeSid(AdminsSid);

    if (SystemSid)
        FreeSid(SystemSid);

    if (AuthenticatedUsersSid)
        FreeSid(AuthenticatedUsersSid);

    return Success;
}

/**
 * @brief
 * Creates or opens a registry-synchronizing semaphore, with access for
 * SYNCHRONIZE | READ_CONTROL | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE
 * by everyone.
 *
 * @note
 * Because this semaphore aims at synchronizing registry accesses for both
 * the global HKEY_LOCAL_MACHINE and the per-user HKEY_CURRENT_USER, this
 * Windows <= 2003 - compatible mechanism isn't fully reliable. On Win7+,
 * synchronization is ensured with the help of the "User-mode Power Service"
 * ("Power", umpo.dll), using RPC calls.
 * https://redplait.blogspot.com/2010/11/umpodll-rpc-interfaces.html
 **/
static BOOLEAN
CreatePwrProfSemaphore(VOID)
{
    SECURITY_ATTRIBUTES SecAttrs;
    PSECURITY_DESCRIPTOR Sd;
    HANDLE hSemaphore;
    DWORD dwError;

    while (TRUE)
    {
    /* Return early if we have already obtained the semaphore handle */
    if (PPRegSemaphore)
        return TRUE;

    /*
     * Suppose first that the semaphore already exists and try only to open it
     * with the necessary access rights. The semaphore may have been created
     * by another instance from a different user (e.g. powrprof.dll loaded by
     * a LocalSystem service) and attempting to create-or-open it instead would
     * fail with ERROR_ACCESS_DENIED.
     */
    hSemaphore = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE,
                                FALSE, L"PowerProfileRegistrySemaphore");
    if (hSemaphore)
        break;
    dwError = GetLastError();
    if (dwError == ERROR_ACCESS_DENIED)
    {
        ERR("Couldn't open POWRPROF semaphore: %ld\n", dwError);
        return FALSE;
    }

    /* The semaphore may not exist. Create it so that all logged-in users
     * can access it with the necessary but limited access rights. */
    if (!CreatePwrProfSemaphoreSecurity(&Sd))
    {
        ERR("Couldn't create POWRPROF semaphore security descriptor\n");
        return FALSE;
    }
    // TODO: Consider using no SD (or no security attributes i.e. falling back to defaults?) if creating the SD failed.

    SecAttrs.nLength = sizeof(SecAttrs);
    SecAttrs.lpSecurityDescriptor = Sd;
    SecAttrs.bInheritHandle = FALSE;

    hSemaphore = CreateSemaphoreW(&SecAttrs, 1, 1, L"PowerProfileRegistrySemaphore");
    dwError = GetLastError();
    HeapFree(GetProcessHeap(), 0, Sd);
    if (hSemaphore)
        break;

    /* If a process from a different user created the semaphore in the
     * meantime, we failed with ERROR_ACCESS_DENIED. Then, retry again
     * by only opening the semaphore this time. Otherwise, bail out. */
    if (dwError != ERROR_ACCESS_DENIED)
    {
        ERR("Couldn't create POWRPROF semaphore: %ld\n", dwError);
        return FALSE;
    }
    }

    /* Store the handle; if another thread already succeeded, close the handle */
    if (InterlockedCompareExchangePointer((PVOID*)&PPRegSemaphore, hSemaphore, NULL) != NULL)
        CloseHandle(hSemaphore);
    return TRUE;
}

static BOOLEAN
AcquirePwrProfSemaphore(VOID)
{
    if (!PPRegSemaphore && !CreatePwrProfSemaphore())
        return FALSE;
    return (WaitForSingleObject(PPRegSemaphore, INFINITE) == WAIT_OBJECT_0);
}


BOOLEAN WINAPI WritePwrPolicy(PUINT puiID, PPOWER_POLICY pPowerPolicy);

NTSTATUS WINAPI
CallNtPowerInformation(POWER_INFORMATION_LEVEL InformationLevel,
                       PVOID lpInputBuffer,
                       ULONG nInputBufferSize,
                       PVOID lpOutputBuffer,
                       ULONG nOutputBufferSize)
{
    BOOLEAN old;

	//Lohnegrim: In order to get the right results, we have to adjust our Privileges
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);
    RtlAdjustPrivilege(SE_CREATE_PAGEFILE_PRIVILEGE, TRUE, FALSE, &old);

    return NtPowerInformation(InformationLevel,
                              lpInputBuffer,
                              nInputBufferSize,
                              lpOutputBuffer,
                              nOutputBufferSize);
}

BOOLEAN WINAPI
CanUserWritePwrScheme(VOID)
{
    HKEY hKey = NULL;
    LONG Ret;

    TRACE("()\n");

    Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ | KEY_WRITE, &hKey);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("RegOpenKeyEx failed: %d\n", Ret);
        SetLastError(Ret);
        return FALSE;
    }

    RegCloseKey(hKey);
    return TRUE;
}

BOOLEAN WINAPI
DeletePwrScheme(UINT uiIndex)
{
    WCHAR Buf[MAX_PATH];
    UINT Current;
    LONG Err;

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%d", uiIndex);

    if (!GetActivePwrScheme(&Current))
        return FALSE;

    if (Current == uiIndex)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    Err = RegDeleteKey(HKEY_CURRENT_USER, (LPCTSTR)Buf);
    if (Err != ERROR_SUCCESS)
    {
        TRACE("RegDeleteKey failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
POWRPROF_GetUserPowerPolicy(LPWSTR szNum,
                            PUSER_POWER_POLICY puserPwrPolicy,
                            DWORD cchName, LPWSTR szName,
                            DWORD cchDesc, LPWSTR szDesc)
{
    HKEY hSubKey = NULL;
    DWORD dwSize;
    LONG Err;
    WCHAR szPath[MAX_PATH];
    BOOL bRet = FALSE;

    swprintf(szPath, L"Control Panel\\PowerCfg\\PowerPolicies\\%s", szNum);

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, szPath, 0, KEY_READ, &hSubKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyExW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = cchName * sizeof(WCHAR);
    Err = RegQueryValueExW(hSubKey, L"Name", NULL, NULL, (LPBYTE)szName, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = cchDesc * sizeof(WCHAR);
    Err = RegQueryValueExW(hSubKey, L"Description", NULL, NULL, (LPBYTE)szDesc, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(USER_POWER_POLICY);
    Err = RegQueryValueExW(hSubKey, L"Policies", NULL, NULL, (LPBYTE)puserPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    bRet = TRUE;

cleanup:
    RegCloseKey(hSubKey);

    return bRet;
}

static BOOLEAN
POWRPROF_GetMachinePowerPolicy(LPWSTR szNum, PMACHINE_POWER_POLICY pmachinePwrPolicy)
{
    HKEY hKey;
    LONG Err;
    WCHAR szPath[MAX_PATH];
    DWORD dwSize;

    swprintf(szPath, L"Software\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\PowerPolicies\\%s", szNum);

    Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyExW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = sizeof(MACHINE_POWER_POLICY);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)pmachinePwrPolicy, &dwSize);

    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);

    return TRUE;
}

BOOLEAN WINAPI
EnumPwrSchemes(PWRSCHEMESENUMPROC lpfnPwrSchemesEnumProc,
               LPARAM lParam)
{
    HKEY hKey;
    LONG Err;
    DWORD dwSize, dwNameSize = MAX_PATH, dwDescSize = MAX_PATH, dwIndex = 0;
    WCHAR szNum[3 + 1], szName[MAX_PATH], szDesc[MAX_PATH];
    POWER_POLICY PwrPolicy;
    USER_POWER_POLICY userPwrPolicy;
    MACHINE_POWER_POLICY machinePwrPolicy;
    BOOLEAN bRet = FALSE;

    if (!lpfnPwrSchemesEnumProc)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg\\PowerPolicies", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    dwSize = sizeof(szNum) / sizeof(WCHAR);

    while (RegEnumKeyExW(hKey, dwIndex, szNum, &dwSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
    {
        if (!POWRPROF_GetUserPowerPolicy(szNum, &userPwrPolicy,
                                         dwNameSize, szName,
                                         dwDescSize, szDesc))
        {
            WARN("POWRPROF_GetUserPowerPolicy failed\n");
            goto cleanup;
        }

        if (!POWRPROF_GetMachinePowerPolicy(szNum, &machinePwrPolicy))
        {
            WARN("POWRPROF_GetMachinePowerPolicy failed\n");
            goto cleanup;
        }

        memcpy(&PwrPolicy.user, &userPwrPolicy, sizeof(USER_POWER_POLICY));
        memcpy(&PwrPolicy.mach, &machinePwrPolicy, sizeof(MACHINE_POWER_POLICY));

        if (!lpfnPwrSchemesEnumProc(_wtoi(szNum), (wcslen(szName) + 1) * sizeof(WCHAR), szName, (wcslen(szDesc) + 1) * sizeof(WCHAR), szDesc, &PwrPolicy, lParam))
            goto cleanup;
        else
            bRet = TRUE;

        dwSize = sizeof(szNum) / sizeof(WCHAR);
        dwIndex++;
    }

cleanup:
    RegCloseKey(hKey);
    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return bRet;
}

BOOLEAN WINAPI
GetActivePwrScheme(PUINT puiID)
{
    HKEY hKey;
    WCHAR szBuf[MAX_PATH];
    DWORD dwSize;
    LONG Err;

    TRACE("GetActivePwrScheme(%u)", puiID);

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKey failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = sizeof(szBuf);
    Err = RegQueryValueExW(hKey, L"CurrentPowerPolicy",
                           NULL, NULL,
                           (LPBYTE)&szBuf, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueEx failed: %d\n", Err);
        RegCloseKey(hKey);
        SetLastError(Err);
        return FALSE;
    }

    RegCloseKey(hKey);
    *puiID = _wtoi(szBuf);

    return TRUE;
}

BOOLEAN WINAPI
GetCurrentPowerPolicies(PGLOBAL_POWER_POLICY pGlobalPowerPolicy,
                        PPOWER_POLICY pPowerPolicy)
{
    /*
    SYSTEM_POWER_POLICY ACPower, DCPower;

    FIXME("(%p, %p) stub!\n", pGlobalPowerPolicy, pPowerPolicy);

    NtPowerInformation(SystemPowerPolicyAc, 0, 0, &ACPower, sizeof(SYSTEM_POWER_POLICY));
    NtPowerInformation(SystemPowerPolicyDc, 0, 0, &DCPower, sizeof(SYSTEM_POWER_POLICY));

    return FALSE;
    */
/*
   Lohnegrim: I don't know why this Function should call NtPowerInformation, because as far as I know,
      it simply returns the GlobalPowerPolicy and the AktivPowerScheme!
 */
    UINT uiID;

    if (pGlobalPowerPolicy != NULL)
    {
        if (!ReadGlobalPwrPolicy(pGlobalPowerPolicy))
            return FALSE;
    }
    if (pPowerPolicy != NULL)
    {
        if (!GetActivePwrScheme(&uiID))
            return FALSE;

        if (!ReadPwrScheme(uiID, pPowerPolicy))
            return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
GetPwrCapabilities(PSYSTEM_POWER_CAPABILITIES lpSystemPowerCapabilities)
{
    NTSTATUS Status;

    TRACE("(%p)\n", lpSystemPowerCapabilities);

    if (!lpSystemPowerCapabilities)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtPowerInformation(SystemPowerCapabilities, 0, 0, lpSystemPowerCapabilities, sizeof(SYSTEM_POWER_CAPABILITIES));
    if(!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
GetPwrDiskSpindownRange(PUINT RangeMax, PUINT RangeMin)
{
    HKEY hKey;
    BYTE lpValue[40];
    LONG Ret;
    DWORD cbValue = sizeof(lpValue);

    TRACE("(%p, %p)\n", RangeMax, RangeMin);

    if (RangeMax == NULL || RangeMin == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!AcquirePwrProfSemaphore())
        return FALSE;

    Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ, &hKey);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("RegOpenKeyEx failed: %d\n", Ret);
        TRACE("Using defaults: 3600, 3\n");
        *RangeMax = 3600;
        *RangeMin = 3;
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return TRUE;
    }

    Ret = RegQueryValueExW(hKey, szDiskMax, 0, 0, lpValue, &cbValue);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("Couldn't open DiskSpinDownMax: %d\n", Ret);
        TRACE("Using default: 3600\n");
        *RangeMax = 3600;
    }
    else
    {
        *RangeMax = _wtoi((LPCWSTR)lpValue);
    }

    cbValue = sizeof(lpValue);

    Ret = RegQueryValueExW(hKey, szDiskMin, 0, 0, lpValue, &cbValue);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("Couldn't open DiskSpinDownMin: %d\n", Ret);
        TRACE("Using default: 3\n");
        *RangeMin = 3;
    }
    else
    {
        *RangeMin = _wtoi((LPCWSTR)lpValue);
    }

    RegCloseKey(hKey);

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return TRUE;
}

BOOLEAN WINAPI
IsAdminOverrideActive(PADMINISTRATOR_POWER_POLICY p)
{
    FIXME("( %p) stub!\n", p);
    return FALSE;
}

BOOLEAN WINAPI
IsPwrHibernateAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return IS_PWR_HIBERNATE_ALLOWED(&PowerCaps);
}

BOOLEAN WINAPI
IsPwrShutdownAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return IS_PWR_POWEROFF_ALLOWED(&PowerCaps);
}

BOOLEAN WINAPI
IsPwrSuspendAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    // NOTE for possible future wine-syncs: wine erroneously use '&&'
    // instead of: PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3;
    return IS_PWR_SUSPEND_ALLOWED(&PowerCaps);
}

/*
 * Power schemes (Vista+). The store is the one the kernel reads,
 * HKLM\SYSTEM\CurrentControlSet\Control\Power:
 *   User\PowerSchemes                          ActivePowerScheme (REG_SZ GUID)
 *   User\PowerSchemes\<scheme>                 FriendlyName, Description
 *   User\PowerSchemes\<scheme>\<sub>\<setting> ACSettingIndex, DCSettingIndex (the scheme's own values)
 *   PowerSettings\<sub>                        FriendlyName, Description
 *   PowerSettings\<sub>\<setting>              FriendlyName, ValueMin/Max/Increment/Units, PossibleValues
 *   PowerSettings\<sub>\<setting>\DefaultPowerSchemeValues\<scheme>  ACSettingIndex, DCSettingIndex
 * GUIDs are written without braces. The kernel watches the store, so a
 * written value of the active scheme takes effect right away.
 */

static const WCHAR szPowerKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Power";
static const WCHAR szSchemesKey[] = L"User\\PowerSchemes";
static const WCHAR szSettingsKey[] = L"PowerSettings";
static const WCHAR szActivePowerScheme[] = L"ActivePowerScheme";
static const WCHAR szAcSettingIndex[] = L"ACSettingIndex";
static const WCHAR szDcSettingIndex[] = L"DCSettingIndex";

/* fea3413e-7e05-4911-9a71-700331f1c294 */
static const GUID PowrProfNoSubgroupGuid =
    { 0xfea3413e, 0x7e05, 0x4911, { 0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94 } };

#define GUID_STRING_CHARS 37 /* 36 characters and the terminator */

static
VOID
PowrProfGuidToString(
    _In_ const GUID *Guid,
    _Out_writes_(GUID_STRING_CHARS) PWSTR Buffer)
{
    swprintf(Buffer, L"%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
             Guid->Data1, Guid->Data2, Guid->Data3,
             Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
             Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

static
BOOL
PowrProfStringToGuid(
    _In_ PCWSTR String,
    _Out_ GUID *Guid)
{
    WCHAR Braced[GUID_STRING_CHARS + 2];
    UNICODE_STRING GuidString;

    if (wcslen(String) != GUID_STRING_CHARS - 1)
        return FALSE;

    swprintf(Braced, L"{%s}", String);
    RtlInitUnicodeString(&GuidString, Braced);
    return NT_SUCCESS(RtlGUIDFromString(&GuidString, Guid));
}

/* Opens <Power>\<Base>[\<g1>[\<g2>[\<g3>]]][\<Suffix>] */
static
LONG
PowrProfOpenKey(
    _In_ PCWSTR Base,
    _In_opt_ const GUID *Guid1,
    _In_opt_ const GUID *Guid2,
    _In_opt_ const GUID *Guid3,
    _In_opt_ PCWSTR Suffix,
    _In_ REGSAM Access,
    _In_ BOOL Create,
    _Out_ PHKEY Key)
{
    WCHAR Path[MAX_PATH];
    WCHAR GuidString[GUID_STRING_CHARS];
    const GUID *Guids[3] = { Guid1, Guid2, Guid3 };
    ULONG i;

    swprintf(Path, L"%s\\%s", szPowerKey, Base);
    for (i = 0; i < ARRAYSIZE(Guids) && Guids[i]; i++)
    {
        PowrProfGuidToString(Guids[i], GuidString);
        wcscat(Path, L"\\");
        wcscat(Path, GuidString);
    }
    if (Suffix)
    {
        wcscat(Path, L"\\");
        wcscat(Path, Suffix);
    }

    if (Create)
        return RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, Access, NULL, Key, NULL);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, Access, Key);
}

static
LONG
PowrProfGetActiveScheme(
    _Out_ GUID *Scheme)
{
    WCHAR Buffer[GUID_STRING_CHARS + 1];
    DWORD Size = sizeof(Buffer) - sizeof(WCHAR), Type;
    HKEY Key;
    LONG Error;

    Error = PowrProfOpenKey(szSchemesKey, NULL, NULL, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    ZeroMemory(Buffer, sizeof(Buffer));
    Error = RegQueryValueExW(Key, szActivePowerScheme, NULL, &Type, (PBYTE)Buffer, &Size);
    RegCloseKey(Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Type != REG_SZ || !PowrProfStringToGuid(Buffer, Scheme))
        return ERROR_INVALID_DATA;

    return ERROR_SUCCESS;
}

/* A setting outside every subgroup lives under NO_SUBGROUP_GUID */
static
const GUID *
PowrProfSubgroup(
    _In_opt_ const GUID *SubGroup)
{
    return SubGroup ? SubGroup : &PowrProfNoSubgroupGuid;
}

/* The scheme's own index, else the setting's default for that scheme */
static
DWORD
PowrProfReadValueIndex(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _Out_ LPDWORD Index)
{
    GUID ActiveScheme;
    HKEY Key;
    DWORD Size, Type, Value;
    LONG Error;

    if (!Setting || !Index)
        return ERROR_INVALID_PARAMETER;

    if (!Scheme)
    {
        Error = PowrProfGetActiveScheme(&ActiveScheme);
        if (Error != ERROR_SUCCESS)
            return Error;
        Scheme = &ActiveScheme;
    }
    SubGroup = PowrProfSubgroup(SubGroup);

    /* The setting must exist */
    Error = PowrProfOpenKey(szSettingsKey, SubGroup, Setting, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;
    RegCloseKey(Key);

    Error = PowrProfOpenKey(szSchemesKey, Scheme, SubGroup, Setting, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error == ERROR_SUCCESS)
    {
        Size = sizeof(Value);
        Error = RegQueryValueExW(Key, ValueName, NULL, &Type, (PBYTE)&Value, &Size);
        RegCloseKey(Key);
        if (Error == ERROR_SUCCESS && Type == REG_DWORD && Size == sizeof(Value))
        {
            *Index = Value;
            return ERROR_SUCCESS;
        }
    }

    Error = PowrProfOpenKey(szSettingsKey, SubGroup, Setting, NULL, L"DefaultPowerSchemeValues",
                            KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* The default of this scheme lives in a key named after it */
    {
        HKEY SchemeKey;
        WCHAR SchemeString[GUID_STRING_CHARS];

        PowrProfGuidToString(Scheme, SchemeString);
        Error = RegOpenKeyExW(Key, SchemeString, 0, KEY_QUERY_VALUE, &SchemeKey);
        RegCloseKey(Key);
        if (Error != ERROR_SUCCESS)
            return Error;

        Size = sizeof(Value);
        Error = RegQueryValueExW(SchemeKey, ValueName, NULL, &Type, (PBYTE)&Value, &Size);
        RegCloseKey(SchemeKey);
    }
    if (Error != ERROR_SUCCESS)
        return Error;
    if (Type != REG_DWORD || Size != sizeof(Value))
        return ERROR_INVALID_DATA;

    *Index = Value;
    return ERROR_SUCCESS;
}

static
DWORD
PowrProfWriteValueIndex(
    _In_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _In_ DWORD Index)
{
    HKEY Key;
    LONG Error;

    if (!Scheme || !Setting)
        return ERROR_INVALID_PARAMETER;
    SubGroup = PowrProfSubgroup(SubGroup);

    /* Only existing schemes and settings take values */
    Error = PowrProfOpenKey(szSchemesKey, Scheme, NULL, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;
    RegCloseKey(Key);
    Error = PowrProfOpenKey(szSettingsKey, SubGroup, Setting, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;
    RegCloseKey(Key);

    Error = PowrProfOpenKey(szSchemesKey, Scheme, SubGroup, Setting, NULL, KEY_SET_VALUE, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, REG_DWORD, (const BYTE *)&Index, sizeof(Index));
    RegCloseKey(Key);
    return Error;
}

/* The value of a power setting, given as a DWORD index */
static
DWORD
PowrProfReadValue(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) LPBYTE Buffer,
    _Inout_opt_ LPDWORD BufferSize)
{
    DWORD Index, Error;

    if (!BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = PowrProfReadValueIndex(Scheme, SubGroup, Setting, ValueName, &Index);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Type)
        *Type = REG_DWORD;

    if (!Buffer)
    {
        *BufferSize = sizeof(Index);
        return ERROR_SUCCESS;
    }
    if (*BufferSize < sizeof(Index))
    {
        *BufferSize = sizeof(Index);
        return ERROR_MORE_DATA;
    }

    memcpy(Buffer, &Index, sizeof(Index));
    *BufferSize = sizeof(Index);
    return ERROR_SUCCESS;
}

DWORD WINAPI
PowerGetActiveScheme(HKEY UserRootPowerKey, GUID **polguid)
{
    GUID Scheme;
    LONG Error;

    TRACE("(%p,%p)\n", UserRootPowerKey, polguid);

    if (!polguid)
        return ERROR_INVALID_PARAMETER;

    Error = PowrProfGetActiveScheme(&Scheme);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* The caller frees it with LocalFree */
    *polguid = LocalAlloc(LMEM_FIXED, sizeof(GUID));
    if (!*polguid)
        return ERROR_NOT_ENOUGH_MEMORY;

    **polguid = Scheme;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSetActiveScheme(HKEY UserRootPowerKey, const GUID *polguid)
{
    WCHAR SchemeString[GUID_STRING_CHARS];
    HKEY Key;
    LONG Error;

    TRACE("(%p,%s)\n", UserRootPowerKey, wine_dbgstr_guid(polguid));

    if (!polguid)
        return ERROR_INVALID_PARAMETER;

    /* The scheme must exist */
    Error = PowrProfOpenKey(szSchemesKey, polguid, NULL, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;
    RegCloseKey(Key);

    Error = PowrProfOpenKey(szSchemesKey, NULL, NULL, NULL, NULL, KEY_SET_VALUE, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    PowrProfGuidToString(polguid, SchemeString);
    Error = RegSetValueExW(Key, szActivePowerScheme, 0, REG_SZ, (const BYTE *)SchemeString,
                           (wcslen(SchemeString) + 1) * sizeof(WCHAR));
    RegCloseKey(Key);
    return Error;
}

DWORD WINAPI
PowerReadDCValue(HKEY RootPowerKey, const GUID *Scheme, const GUID *SubGroup, const GUID *PowerSettings, PULONG Type, PUCHAR Buffer, DWORD *BufferSize)
{
    TRACE("(%p,%s,%s,%s,%p,%p,%p)\n", RootPowerKey, debugstr_guid(Scheme), debugstr_guid(SubGroup), debugstr_guid(PowerSettings), Type, Buffer, BufferSize);
    return PowrProfReadValue(Scheme, SubGroup, PowerSettings, szDcSettingIndex, Type, Buffer, BufferSize);
}

DWORD WINAPI PowerReadFriendlyName(HKEY RootPowerKey, const GUID *Scheme,
	const GUID *SubGroup, const GUID *PowerSettings, UCHAR *Buffer,
	DWORD *BufferSize)
{
    HKEY Key;
    DWORD Size, Type;
    LONG Error;

    TRACE("(%p,%s,%s,%s,%p,%p)\n", RootPowerKey, debugstr_guid(Scheme), debugstr_guid(SubGroup), debugstr_guid(PowerSettings), Buffer, BufferSize);

    if (!BufferSize)
        return ERROR_INVALID_PARAMETER;

    /* The scheme's name, or the name of a subgroup or a setting */
    if (PowerSettings)
        Error = PowrProfOpenKey(szSettingsKey, PowrProfSubgroup(SubGroup), PowerSettings, NULL, NULL,
                                KEY_QUERY_VALUE, FALSE, &Key);
    else if (SubGroup)
        Error = PowrProfOpenKey(szSettingsKey, SubGroup, NULL, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    else if (Scheme)
        Error = PowrProfOpenKey(szSchemesKey, Scheme, NULL, NULL, NULL, KEY_QUERY_VALUE, FALSE, &Key);
    else
        return ERROR_INVALID_PARAMETER;
    if (Error != ERROR_SUCCESS)
        return Error;

    Size = Buffer ? *BufferSize : 0;
    Error = RegQueryValueExW(Key, L"FriendlyName", NULL, &Type, Buffer, &Size);
    RegCloseKey(Key);
    if (Error == ERROR_SUCCESS && Type != REG_SZ && Type != REG_EXPAND_SZ)
        return ERROR_INVALID_DATA;
    if (Error == ERROR_SUCCESS || Error == ERROR_MORE_DATA)
    {
        *BufferSize = Size;
        if (!Buffer)
            return ERROR_SUCCESS;
    }
    return Error;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRole(void)
{
   FIXME("stub\n");
   return PlatformRoleDesktop;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRoleEx(ULONG version)
{
    FIXME("%lu stub.\n", version);
    return PlatformRoleDesktop;
}

/* The Index-th subkey of an open key whose name is a GUID */
static
DWORD
PowrProfEnumGuidKey(
    _In_ HKEY Key,
    _In_ ULONG Index,
    _Out_writes_bytes_opt_(*BufferSize) UCHAR *Buffer,
    _Inout_ DWORD *BufferSize)
{
    WCHAR Name[GUID_STRING_CHARS + 1];
    DWORD NameLength;
    ULONG SubKey, Found = 0;
    GUID Guid;
    LONG Error;

    /* Keys whose names are not GUIDs (none in a well-formed store) do not count */
    for (SubKey = 0; ; SubKey++)
    {
        NameLength = ARRAYSIZE(Name);
        Error = RegEnumKeyExW(Key, SubKey, Name, &NameLength, NULL, NULL, NULL, NULL);
        if (Error == ERROR_MORE_DATA)
            continue;
        if (Error != ERROR_SUCCESS)
            return Error;
        if (!PowrProfStringToGuid(Name, &Guid))
            continue;
        if (Found++ == Index)
            break;
    }

    if (!Buffer || *BufferSize < sizeof(GUID))
    {
        *BufferSize = sizeof(GUID);
        return ERROR_MORE_DATA;
    }

    memcpy(Buffer, &Guid, sizeof(GUID));
    *BufferSize = sizeof(GUID);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerEnumerate(HKEY key, const GUID *scheme, const GUID *subgroup, POWER_DATA_ACCESSOR flags,
                        ULONG index, UCHAR *buffer, DWORD *buffer_size)
{
    HKEY Key;
    LONG Error;

    TRACE("(%p,%s,%s,%d,%ld,%p,%p)\n", key, debugstr_guid(scheme), debugstr_guid(subgroup),
          flags, index, buffer, buffer_size);

    if (!buffer_size)
        return ERROR_INVALID_PARAMETER;

    switch (flags)
    {
        case ACCESS_SCHEME:
            Error = PowrProfOpenKey(szSchemesKey, NULL, NULL, NULL, NULL,
                                    KEY_ENUMERATE_SUB_KEYS, FALSE, &Key);
            break;

        /* Every subgroup and setting the store defines applies to every scheme */
        case ACCESS_SUBGROUP:
            if (!scheme)
                return ERROR_INVALID_PARAMETER;
            Error = PowrProfOpenKey(szSettingsKey, NULL, NULL, NULL, NULL,
                                    KEY_ENUMERATE_SUB_KEYS, FALSE, &Key);
            break;

        case ACCESS_INDIVIDUAL_SETTING:
            if (!scheme)
                return ERROR_INVALID_PARAMETER;
            Error = PowrProfOpenKey(szSettingsKey, PowrProfSubgroup(subgroup), NULL, NULL, NULL,
                                    KEY_ENUMERATE_SUB_KEYS, FALSE, &Key);
            break;

        default:
            FIXME("Unsupported access flags %d\n", flags);
            return ERROR_INVALID_PARAMETER;
    }
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PowrProfEnumGuidKey(Key, index, buffer, buffer_size);
    RegCloseKey(Key);
    return Error;
}

DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle)
{
    FIXME("(0x%08lx,%p,%p) stub!\n", flags, recipient, handle);
    *handle = (HPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerUnregisterSuspendResumeNotification(HPOWERNOTIFY handle)
{
    FIXME("(%p) stub!\n", handle);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingRegisterNotification(const GUID *setting, DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle)
{
    FIXME("(%s,0x%08lx,%p,%p) stub!\n", debugstr_guid(setting), flags, recipient, handle);
    *handle = (PHPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingUnregisterNotification(HPOWERNOTIFY handle)
{
    FIXME("(%p) stub!\n", handle);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerWriteACValueIndex(HKEY key, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD index)
{
    TRACE("(%p,%s,%s,%s,0x%08lx)\n", key, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index);
    return PowrProfWriteValueIndex(scheme, subgroup, setting, szAcSettingIndex, index);
}

DWORD WINAPI PowerWriteDCValueIndex(
   HKEY       key,
   const GUID *scheme,
   const GUID *subgroup,
   const GUID *setting,
   DWORD      index
)
{
    TRACE("(%p,%s,%s,%s,0x%08lx)\n", key, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index);
    return PowrProfWriteValueIndex(scheme, subgroup, setting, szDcSettingIndex, index);
}

DWORD WINAPI PowerReadACValueIndex(
   HKEY       key,
   const GUID *scheme,
   const GUID *subgroup,
   const GUID *setting,
   LPDWORD    AcValueIndex
)
{
    TRACE("(%p,%s,%s,%s,%p)\n", key, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), AcValueIndex);
    return PowrProfReadValueIndex(scheme, subgroup, setting, szAcSettingIndex, AcValueIndex);
}

DWORD WINAPI PowerReadDCValueIndex(
    HKEY       key,
    const GUID *scheme,
    const GUID *subgroup,
    const GUID *setting,
    LPDWORD    DcValueIndex
)
{
    TRACE("(%p,%s,%s,%s,%p)\n", key, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), DcValueIndex);
    return PowrProfReadValueIndex(scheme, subgroup, setting, szDcSettingIndex, DcValueIndex);
}

DWORD WINAPI
PowerReadACValue(HKEY RootPowerKey, const GUID *Scheme, const GUID *SubGroup, const GUID *PowerSettings, PULONG Type, PUCHAR Buffer, DWORD *BufferSize)
{
    TRACE("(%p,%s,%s,%s,%p,%p,%p)\n", RootPowerKey, debugstr_guid(Scheme), debugstr_guid(SubGroup), debugstr_guid(PowerSettings), Type, Buffer, BufferSize);
    return PowrProfReadValue(Scheme, SubGroup, PowerSettings, szAcSettingIndex, Type, Buffer, BufferSize);
}

BOOLEAN WINAPI
ReadGlobalPwrPolicy(PGLOBAL_POWER_POLICY pGlobalPowerPolicy)
{
    GLOBAL_MACHINE_POWER_POLICY glMachPwrPolicy;
    GLOBAL_USER_POWER_POLICY glUserPwrPolicy;
    HKEY hKey = NULL;
    DWORD dwSize;
    LONG Err;
    BOOL bRet = FALSE;

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    // Getting user global power policy
    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg\\GlobalPowerPolicy", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(glUserPwrPolicy);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)&glUserPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    RegCloseKey(hKey);

    // Getting machine global power policy
    Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\GlobalPowerPolicy", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(glMachPwrPolicy);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)&glMachPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    memcpy(&pGlobalPowerPolicy->user, &glUserPwrPolicy, sizeof(GLOBAL_USER_POWER_POLICY));
    memcpy(&pGlobalPowerPolicy->mach, &glMachPwrPolicy, sizeof(GLOBAL_MACHINE_POWER_POLICY));
    bRet = TRUE;

cleanup:
    if(hKey)
        RegCloseKey(hKey);
    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return bRet;
}


BOOLEAN WINAPI
ReadProcessorPwrScheme(UINT uiID,
                       PMACHINE_PROCESSOR_POWER_POLICY pMachineProcessorPowerPolicy)
{
    HKEY hKey;
    WCHAR szPath[MAX_PATH];
    DWORD dwSize = sizeof(MACHINE_PROCESSOR_POWER_POLICY);

    swprintf(szPath, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\ProcessorPolicies\\%i", uiID);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    if (RegQueryValueExW(hKey, szPolicies, NULL, 0, (LPBYTE)pMachineProcessorPowerPolicy, &dwSize) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return TRUE;
    }

    RegCloseKey(hKey);
    if (uiID != 0)
        return ReadProcessorPwrScheme(0, pMachineProcessorPowerPolicy);

    return FALSE;
}


BOOLEAN WINAPI
ReadPwrScheme(UINT uiID,
    PPOWER_POLICY pPowerPolicy)
{
    USER_POWER_POLICY userPwrPolicy;
    MACHINE_POWER_POLICY machinePwrPolicy;
    WCHAR szNum[16]; // max number - 999

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    swprintf(szNum, L"%d", uiID);

    if (!POWRPROF_GetUserPowerPolicy(szNum, &userPwrPolicy, 0, NULL, 0, NULL))
    {
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return FALSE;
    }

    if (!POWRPROF_GetMachinePowerPolicy(szNum, &machinePwrPolicy))
    {
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return FALSE;
    }

    memcpy(&pPowerPolicy->user, &userPwrPolicy, sizeof(userPwrPolicy));
    memcpy(&pPowerPolicy->mach, &machinePwrPolicy, sizeof(machinePwrPolicy));

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return TRUE;
}

BOOLEAN WINAPI
SetActivePwrScheme(UINT uiID,
                   PGLOBAL_POWER_POLICY lpGlobalPowerPolicy,
                   PPOWER_POLICY lpPowerPolicy)
{
    POWER_POLICY tmp;
    HKEY hKey;
    WCHAR Buf[16];

    if (!ReadPwrScheme(uiID, &tmp))
        return FALSE;

    if (RegOpenKeyEx(HKEY_CURRENT_USER, szUserPowerConfigSubKey, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return FALSE;

    swprintf(Buf, L"%i", uiID);

    if (RegSetValueExW(hKey, szCurrentPowerPolicies, 0, REG_SZ, (PBYTE)Buf, strlenW(Buf)*sizeof(WCHAR)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }
    RegCloseKey(hKey);

    if (lpGlobalPowerPolicy != NULL || lpPowerPolicy != NULL)
    {
        if (!ValidatePowerPolicies(lpGlobalPowerPolicy, lpPowerPolicy))
            return FALSE;

        if (lpGlobalPowerPolicy != NULL && !WriteGlobalPwrPolicy(lpGlobalPowerPolicy))
            return FALSE;

        if (lpPowerPolicy != NULL && !WritePwrPolicy(&uiID,lpPowerPolicy))
            return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
SetSuspendState(BOOLEAN Hibernate,
                BOOLEAN ForceCritical,
                BOOLEAN DisableWakeEvent)
{
    FIXME("(%d, %d, %d) stub!\n", Hibernate, ForceCritical, DisableWakeEvent);
    return TRUE;
}

BOOLEAN WINAPI
WriteGlobalPwrPolicy(PGLOBAL_POWER_POLICY pGlobalPowerPolicy)
{
    HKEY hKey;
    GLOBAL_USER_POWER_POLICY gupp;
    GLOBAL_MACHINE_POWER_POLICY gmpp;

    gupp = pGlobalPowerPolicy->user;
    gmpp = pGlobalPowerPolicy->mach;

    if (RegOpenKeyEx(HKEY_CURRENT_USER,
                    L"Control Panel\\PowerCfg\\GlobalPowerPolicy",
                    0,
                    KEY_WRITE,
                    &hKey) != ERROR_SUCCESS)
        return FALSE;

    if (RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (PBYTE)&gupp, sizeof(gupp)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\GlobalPowerPolicy",
                     0,
                     KEY_ALL_ACCESS,
                     &hKey))
        return FALSE;

    if (RegSetValueExW(hKey,szPolicies, 0, REG_BINARY, (PBYTE)&gmpp, sizeof(gmpp)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);
    return TRUE;
}

BOOLEAN WINAPI
WriteProcessorPwrScheme(UINT ID,
                        PMACHINE_PROCESSOR_POWER_POLICY pMachineProcessorPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    swprintf(Buf, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\ProcessorPolicies\\%i", ID);

    if (RegCreateKey(HKEY_LOCAL_MACHINE, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (PBYTE)pMachineProcessorPowerPolicy, sizeof(MACHINE_PROCESSOR_POWER_POLICY));
    RegCloseKey(hKey);
    return TRUE;
}

static VOID
SetLastID(VOID)
{
    WCHAR Buf[16];
    HKEY hKey;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                    szPowerCfgSubKey,
                    0,
                    KEY_WRITE,
                    &hKey) != ERROR_SUCCESS)
        return;
    swprintf(Buf, L"%i", g_LastID);
    RegSetValueExW(hKey, szLastID, 0, REG_SZ, (PBYTE)Buf, strlenW(Buf)*sizeof(WCHAR));
    RegCloseKey(hKey);
}

BOOLEAN WINAPI
WritePwrScheme(PUINT puiID,
               LPWSTR lpszName,
               LPWSTR lpszDescription,
               PPOWER_POLICY pPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    if (*puiID == -1)
    {
        g_LastID++;
        *puiID = g_LastID;
        SetLastID();
    }

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_CURRENT_USER, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szName, 0, REG_SZ, (PBYTE)lpszName, strlenW(lpszName)*sizeof(WCHAR));
    RegSetValueExW(hKey, szDescription, 0, REG_SZ, (PBYTE)lpszDescription, strlenW(lpszDescription)*sizeof(WCHAR));
    RegCloseKey(hKey);
    return WritePwrPolicy(puiID, pPowerPolicy);
}

static BOOLEAN
CheckPowerActionPolicy(PPOWER_ACTION_POLICY pPAP, SYSTEM_POWER_CAPABILITIES PowerCaps)
{
/*
   Lohnegrim: this is an Helper function, it checks if the POWERACTIONPOLICY is valid
   Also, if the System doesn't support Hibernation, then change the PowerAction
*/
    switch (pPAP->Action)
    {
    case PowerActionNone:
        return TRUE;
    case PowerActionReserved:
        if (PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3)
            pPAP->Action = PowerActionSleep;
        else
            pPAP->Action = PowerActionReserved;
    case PowerActionSleep:
        return TRUE;
    case PowerActionHibernate:
        if (!(PowerCaps.SystemS4 && PowerCaps.HiberFilePresent))
        {
            if (PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3)
                pPAP->Action = PowerActionSleep;
            else
                pPAP->Action = PowerActionReserved;
        }
    case PowerActionShutdown:
    case PowerActionShutdownReset:
    case PowerActionShutdownOff:
    case PowerActionWarmEject:
        return TRUE;
    default:
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    };
}

static VOID
FixSystemPowerState(PSYSTEM_POWER_STATE Psps, SYSTEM_POWER_CAPABILITIES PowerCaps)
{
	//Lohnegrim: If the System doesn't support the Powerstates, then we have to change them
    if (!PowerCaps.SystemS1 && *Psps == PowerSystemSleeping1)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS2 && *Psps == PowerSystemSleeping2)
        *Psps = PowerSystemSleeping3;
    if (!PowerCaps.SystemS3 && *Psps == PowerSystemSleeping3)
        *Psps = PowerSystemHibernate;
    if (!(PowerCaps.SystemS4 && PowerCaps.HiberFilePresent) && *Psps == PowerSystemHibernate)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS1 && *Psps == PowerSystemSleeping1)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS2 && *Psps == PowerSystemSleeping2)
        *Psps = PowerSystemSleeping3;
    if (!PowerCaps.SystemS3 && *Psps == PowerSystemSleeping3)
        *Psps = PowerSystemShutdown;

}

BOOLEAN WINAPI
ValidatePowerPolicies(PGLOBAL_POWER_POLICY pGPP, PPOWER_POLICY pPP)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS ret;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);
    ret = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (ret != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(ret));
        return FALSE;
    }

    if (pGPP)
    {
        if (pGPP->user.Revision != 1 || pGPP->mach.Revision != 1)
        {
            SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }
        if (pGPP->mach.LidOpenWakeAc == PowerSystemUnspecified)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pGPP->mach.LidOpenWakeAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if (pGPP->mach.LidOpenWakeDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pGPP->mach.LidOpenWakeDc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((pGPP->mach.LidOpenWakeDc < PowerSystemWorking) || (pGPP->mach.LidOpenWakeDc >= PowerSystemMaximum))
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (!CheckPowerActionPolicy(&pGPP->user.LidCloseAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.LidCloseDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.PowerButtonAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.PowerButtonDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.SleepButtonAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.SleepButtonDc,PowerCaps))
        {
            return FALSE;
        }
        //Lohnegrim: The BroadcastCapacityResolution presents the Powerlevel in Percent, if invalid set th 100 == FULL
        if (pGPP->mach.BroadcastCapacityResolution > 100)
            pGPP->mach.BroadcastCapacityResolution = 100;

		//Lohnegrim: I have no idea, if they are really needed, or if they are specific for my System, or what they mean, so I removed them
        //pGPP->user.DischargePolicy[1].PowerPolicy.EventCode = pGPP->user.DischargePolicy[1].PowerPolicy.EventCode | 0x010000;
        //pGPP->user.DischargePolicy[2].PowerPolicy.EventCode = pGPP->user.DischargePolicy[2].PowerPolicy.EventCode | 0x020000;
        //pGPP->user.DischargePolicy[3].PowerPolicy.EventCode = pGPP->user.DischargePolicy[3].PowerPolicy.EventCode | 0x030000;

        FixSystemPowerState(&pGPP->mach.LidOpenWakeAc,PowerCaps);
        FixSystemPowerState(&pGPP->mach.LidOpenWakeDc,PowerCaps);

    }

    if (pPP)
    {
        if (pPP->user.Revision != 1 || pPP->mach.Revision != 1)
        {
            SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }

		//Lohnegrim: unneeded
        //if (pPP->mach.MinSleepAc < PowerSystemWorking)
        //{
        //    SetLastError(ERROR_GEN_FAILURE);
        //    return FALSE;
        //}
        if ((int)pPP->mach.MinSleepAc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        //if (pPP->mach.MinSleepDc < PowerSystemWorking)
        //{
        //    SetLastError(ERROR_GEN_FAILURE);
        //    return FALSE;
        //}
        if ((int)pPP->mach.MinSleepDc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepAc == PowerSystemUnspecified)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepDc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }

        if (!CheckPowerActionPolicy(&pPP->mach.OverThrottledAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->mach.OverThrottledDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->user.IdleAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->user.IdleDc,PowerCaps))
        {
            return FALSE;
        }
        if (pPP->user.MaxSleepAc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((int)pPP->user.MaxSleepAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (pPP->user.MaxSleepDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((int)pPP->user.MaxSleepDc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (PowerCaps.SystemS1)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping1;
            pPP->mach.MinSleepDc=PowerSystemSleeping1;
        }
        else if (PowerCaps.SystemS2)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping2;
            pPP->mach.MinSleepDc=PowerSystemSleeping2;
        }
        else if (PowerCaps.SystemS3)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping3;
            pPP->mach.MinSleepDc=PowerSystemSleeping3;
        }

        if (PowerCaps.SystemS4)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping3;
            pPP->user.MaxSleepDc=PowerSystemSleeping3;
        }
        else if (PowerCaps.SystemS3)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping2;
            pPP->user.MaxSleepDc=PowerSystemSleeping2;
        }
        else if (PowerCaps.SystemS1)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping1;
            pPP->user.MaxSleepDc=PowerSystemSleeping1;
        }
		//Lohnegrim: I don't know where to get this info from, so I removed it
        //pPP->user.OptimizeForPowerAc=TRUE;
        //pPP->user.OptimizeForPowerDc=TRUE;

        FixSystemPowerState(&pPP->mach.ReducedLatencySleepAc,PowerCaps);
        FixSystemPowerState(&pPP->mach.ReducedLatencySleepDc,PowerCaps);
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOLEAN WINAPI WritePwrPolicy(PUINT puiID, PPOWER_POLICY pPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_CURRENT_USER, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (const unsigned char *)&pPowerPolicy->user, sizeof(USER_POWER_POLICY));
    RegCloseKey(hKey);

    swprintf(Buf, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_LOCAL_MACHINE, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (const unsigned char *)&pPowerPolicy->mach, sizeof(MACHINE_POWER_POLICY));
    RegCloseKey(hKey);

    return TRUE;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch(fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            HKEY hKey;
            LONG Err;

            DisableThreadLibraryCalls(hinstDLL);

            Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ, &hKey);

            if (Err != ERROR_SUCCESS)
            {
                TRACE("Couldn't open registry key HKLM\\%s, using some sane(?) defaults\n", debugstr_w(szPowerCfgSubKey));
            }
            else
            {
                WCHAR lpValue[MAX_PATH];
                DWORD cbValue = sizeof(lpValue);

                Err = RegQueryValueExW(hKey, szLastID, 0, 0, (BYTE*)lpValue, &cbValue);
                if (Err == ERROR_SUCCESS)
                {
                    g_LastID = _wtoi(lpValue);
                }
                else
                {
                    TRACE("Couldn't open registry entry HKLM\\%s\\LastID, using some sane(?) defaults\n", debugstr_w(szPowerCfgSubKey));
                }
                RegCloseKey(hKey);
            }

            break;
        }
        case DLL_PROCESS_DETACH:
            if (lpvReserved) break;
            CloseHandle(PPRegSemaphore);
            break;
    }
    return TRUE;
}
