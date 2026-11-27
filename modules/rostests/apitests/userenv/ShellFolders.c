/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the shell folders of a newly created user profile
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * Loading the profile of a user that has none creates it.  The folders of the
 * new profile that its hive names in "Shell Folders" are the folders of that
 * profile: My Music is <profile>\Music, My Pictures <profile>\Pictures and
 * My Video <profile>\Videos.  Needs the rights to add a local user; skipped
 * otherwise.
 */

#include <apitest.h>
#include <winreg.h>
#include <sddl.h>
#include <userenv.h>
#include <lm.h>
#include <strsafe.h>

#undef SE_RESTORE_NAME
#undef SE_BACKUP_NAME
#define SE_RESTORE_NAME     L"SeRestorePrivilege"
#define SE_BACKUP_NAME      L"SeBackupPrivilege"

static WCHAR TestUser[] = L"rostest_shfold";
static WCHAR TestPassword[] = L"Rt-shf0ld!9q";

static BOOL
EnablePrivilege(LPCWSTR lpszPrivilegeName, BOOL bEnablePrivilege)
{
    BOOL Success;
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;

    Success = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken);
    if (!Success)
        return Success;

    Success = LookupPrivilegeValueW(NULL, lpszPrivilegeName, &tp.Privileges[0].Luid);
    if (Success)
    {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = (bEnablePrivilege ? SE_PRIVILEGE_ENABLED : 0);
        Success = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
    }

    CloseHandle(hToken);
    return Success;
}

/* The string SID of the user of hToken, LocalFree it; NULL on failure. */
static LPWSTR
GetTokenUserSidString(HANDLE hToken)
{
    BYTE Buffer[256];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    DWORD Length;
    LPWSTR SidString = NULL;

    if (!GetTokenInformation(hToken, TokenUser, Buffer, sizeof(Buffer), &Length))
        return NULL;
    if (!ConvertSidToStringSidW(User->User.Sid, &SidString))
        return NULL;
    return SidString;
}

static VOID
CheckFolder(HKEY hFolders, LPCWSTR ValueName, LPCWSTR ProfileDir, LPCWSTR SubDir)
{
    WCHAR Expected[MAX_PATH], Value[MAX_PATH];
    DWORD Type, Size = sizeof(Value);
    LONG Error;

    Error = RegQueryValueExW(hFolders, ValueName, NULL, &Type, (LPBYTE)Value, &Size);
    if (Error == ERROR_FILE_NOT_FOUND)
    {
        trace("No '%ls' value in Shell Folders\n", ValueName);
        return;
    }
    ok(Error == ERROR_SUCCESS, "RegQueryValueExW(%ls) returned %ld\n", ValueName, Error);
    if (Error != ERROR_SUCCESS)
        return;
    ok(Type == REG_SZ, "'%ls' has type %lu\n", ValueName, Type);
    Value[ARRAYSIZE(Value) - 1] = UNICODE_NULL;

    StringCchPrintfW(Expected, ARRAYSIZE(Expected), L"%s\\%s", ProfileDir, SubDir);
    ok(_wcsicmp(Value, Expected) == 0, "'%ls' is '%ls', expected '%ls'\n",
       ValueName, Value, Expected);
}

START_TEST(ShellFolders)
{
    USER_INFO_1 UserInfo;
    NET_API_STATUS Status;
    HANDLE hToken = NULL;
    PROFILEINFOW ProfileInfo;
    WCHAR ProfileDir[MAX_PATH];
    DWORD Size;
    LPWSTR SidString = NULL;
    HKEY hFolders;
    LONG Error;
    BOOL Success;

    ZeroMemory(&UserInfo, sizeof(UserInfo));
    UserInfo.usri1_name = TestUser;
    UserInfo.usri1_password = TestPassword;
    UserInfo.usri1_priv = USER_PRIV_USER;
    UserInfo.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&UserInfo, NULL);
    if (Status == NERR_UserExists)
    {
        /* Left over by an earlier run: start from scratch. */
        Status = NetUserDel(NULL, TestUser);
        ok(Status == NERR_Success, "NetUserDel(%ls) failed with %lu\n", TestUser, Status);
        Status = NetUserAdd(NULL, 1, (LPBYTE)&UserInfo, NULL);
    }
    if (Status != NERR_Success)
    {
        skip("NetUserAdd failed with %lu\n", Status);
        return;
    }

    Success = LogonUserW(TestUser, L".", TestPassword, LOGON32_LOGON_INTERACTIVE,
                         LOGON32_PROVIDER_DEFAULT, &hToken);
    ok(Success, "LogonUserW failed with error %lu\n", GetLastError());
    if (!Success)
        goto DeleteUser;

    SidString = GetTokenUserSidString(hToken);
    ok(SidString != NULL, "No SID for the token, error %lu\n", GetLastError());

    EnablePrivilege(SE_RESTORE_NAME, TRUE);
    EnablePrivilege(SE_BACKUP_NAME, TRUE);

    ZeroMemory(&ProfileInfo, sizeof(ProfileInfo));
    ProfileInfo.dwSize = sizeof(ProfileInfo);
    ProfileInfo.dwFlags = PI_NOUI;
    ProfileInfo.lpUserName = TestUser;
    Success = LoadUserProfileW(hToken, &ProfileInfo);
    ok(Success, "LoadUserProfileW failed with error %lu\n", GetLastError());
    if (!Success)
        goto CloseToken;

    Size = ARRAYSIZE(ProfileDir);
    Success = GetUserProfileDirectoryW(hToken, ProfileDir, &Size);
    ok(Success, "GetUserProfileDirectoryW failed with error %lu\n", GetLastError());
    if (Success)
    {
        trace("New profile in '%ls'\n", ProfileDir);
        Error = RegOpenKeyExW(ProfileInfo.hProfile,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                              0, KEY_QUERY_VALUE, &hFolders);
        if (Error == ERROR_SUCCESS)
        {
            CheckFolder(hFolders, L"My Music", ProfileDir, L"Music");
            CheckFolder(hFolders, L"My Pictures", ProfileDir, L"Pictures");
            CheckFolder(hFolders, L"My Video", ProfileDir, L"Videos");
            RegCloseKey(hFolders);
        }
        else
        {
            trace("No Shell Folders key in the new profile (%ld)\n", Error);
        }
    }

    Success = UnloadUserProfile(hToken, ProfileInfo.hProfile);
    ok(Success, "UnloadUserProfile failed with error %lu\n", GetLastError());

CloseToken:
    EnablePrivilege(SE_BACKUP_NAME, FALSE);
    EnablePrivilege(SE_RESTORE_NAME, FALSE);
    CloseHandle(hToken);

    if (SidString != NULL)
    {
        Success = DeleteProfileW(SidString, NULL, NULL);
        ok(Success, "DeleteProfileW(%ls) failed with error %lu\n", SidString, GetLastError());
        LocalFree(SidString);
    }

DeleteUser:
    Status = NetUserDel(NULL, TestUser);
    ok(Status == NERR_Success, "NetUserDel failed with %lu\n", Status);
}
