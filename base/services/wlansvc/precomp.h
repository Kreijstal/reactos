#ifndef _WLANSVC_PCH_
#define _WLANSVC_PCH_

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winsvc.h>
#include <wchar.h>
#include <wlansvc_s.h>

/* WLAN_*_ACCESS would normally come from wlanapi.h, but the widl-generated
 * wlansvc_s.h redefines several public WLAN types (WLAN_CONNECTION_PARAMETERS,
 * WLAN_NOTIFICATION_DATA, ...) that would collide if wlanapi.h were also in
 * scope.  These are stable Vista-era constants so duplicate them locally. */
#ifndef WLAN_READ_ACCESS
#define WLAN_READ_ACCESS    (STANDARD_RIGHTS_READ | FILE_READ_DATA)
#define WLAN_EXECUTE_ACCESS (STANDARD_RIGHTS_EXECUTE | FILE_EXECUTE | WLAN_READ_ACCESS)
#define WLAN_WRITE_ACCESS   (STANDARD_RIGHTS_WRITE | FILE_WRITE_DATA | DELETE | WRITE_DAC | WLAN_READ_ACCESS | WLAN_EXECUTE_ACCESS)
#endif

#include <ndk/rtlfuncs.h>
#include <ndk/obfuncs.h>

typedef struct _WLANSVCHANDLE
{
    LIST_ENTRY WlanSvcHandleListEntry;
    DWORD      dwClientVersion;
    DWORD      dwNotifSource;
} WLANSVCHANDLE, *PWLANSVCHANDLE;

/* regkey.c */
VOID  WlanSvcGuidToString(const GUID *Guid, LPWSTR Buffer);
DWORD WlanSvcOpenServiceKey(REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenInterfaceKey(const GUID *InterfaceGuid, REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenProfilesKey(const GUID *InterfaceGuid, REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenProfileKey(const GUID *InterfaceGuid, LPCWSTR ProfileName, BOOL Create, REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenParametersKey(REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenFiltersKey(REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcOpenSecurityKey(REGSAM SamDesired, PHKEY pKey);
DWORD WlanSvcReadAllocBinary(HKEY Key, LPCWSTR ValueName, LPDWORD pDataSize, LPBYTE *ppData);
DWORD WlanSvcReadAllocString(HKEY Key, LPCWSTR ValueName, LPWSTR *ppString);
DWORD WlanSvcReadDword(HKEY Key, LPCWSTR ValueName, LPDWORD pValue);
DWORD WlanSvcEnumProfileNames(const GUID *InterfaceGuid, PWLAN_PROFILE_INFO_LIST *ppList);

/* rpcserver.c */
PWLANSVCHANDLE WlanSvcGetHandleEntry(LPWLANSVC_RPC_HANDLE ClientHandle);

#endif /* _WLANSVC_PCH_ */
