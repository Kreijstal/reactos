/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Tests for Get[AllUsers|DefaultUser|User]Profile[s]Directory APIs.
 * PROGRAMMERS:     Hermes Belusca-Maito
 */

#include <apitest.h>
#include <winreg.h>
#include <userenv.h>

/* The Get[AllUsers|DefaultUser|User]Profile[s]Directory have the same prototype */
typedef BOOL (WINAPI *GET_PROFILE_DIRS_FUNC)(LPWSTR lpProfileDir, LPDWORD lpcchSize);

typedef struct _GET_PROFILE_DIRS
{
    GET_PROFILE_DIRS_FUNC pFunc;
    LPCWSTR pFuncName;
} GET_PROFILE_DIRS, *PGET_PROFILE_DIRS;

GET_PROFILE_DIRS GetProfileDirsFuncsList[] =
{
    {GetAllUsersProfileDirectoryW,    L"GetAllUsersProfileDirectoryW"},
    {GetDefaultUserProfileDirectoryW, L"GetDefaultUserProfileDirectoryW"},
    {GetProfilesDirectoryW,           L"GetProfilesDirectoryW"},
//  {GetUserProfileDirectoryW,        L"GetUserProfileDirectoryW"},
};

/*
 * The directories come from the ProfileList values, which are there from the
 * start (in the hive setup and the live CD ship), as unexpanded
 * REG_EXPAND_SZ paths.
 */
static void
TestProfileListValue(LPCWSTR pValueName, GET_PROFILE_DIRS_FUNC pFunc, LPCWSTR pFuncName)
{
    HKEY hKey;
    LONG Error;
    DWORD dwType = REG_NONE, cbData, cchSize;
    WCHAR szValue[MAX_PATH], szExpanded[MAX_PATH], szProfileDir[MAX_PATH];

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                          0, KEY_QUERY_VALUE, &hKey);
    ok(Error == ERROR_SUCCESS, "RegOpenKeyExW(ProfileList) returned %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    cbData = sizeof(szValue) - sizeof(WCHAR);
    Error = RegQueryValueExW(hKey, pValueName, NULL, &dwType, (LPBYTE)szValue, &cbData);
    RegCloseKey(hKey);
    ok(Error == ERROR_SUCCESS, "ProfileList\\%S: RegQueryValueExW returned %ld\n", pValueName, Error);
    if (Error != ERROR_SUCCESS)
        return;
    szValue[cbData / sizeof(WCHAR)] = UNICODE_NULL;
    ok(dwType == REG_EXPAND_SZ, "ProfileList\\%S: type %lu, expected REG_EXPAND_SZ\n", pValueName, dwType);

    if (pFunc == NULL)
        return;
    ok(ExpandEnvironmentStringsW(szValue, szExpanded, _countof(szExpanded)) != 0,
       "ExpandEnvironmentStringsW(%S) failed\n", szValue);
    cchSize = _countof(szProfileDir);
    ok(pFunc(szProfileDir, &cchSize), "%S failed with %lu\n", pFuncName, GetLastError());
    ok(lstrcmpiW(szProfileDir, szExpanded) == 0, "%S: '%S', ProfileList\\%S is '%S'\n",
       pFuncName, szProfileDir, pValueName, szExpanded);
}

START_TEST(GetProfileDirs)
{
    BOOL Success;
    DWORD dwLastError;
    DWORD cchSize;
    WCHAR szProfileDir[MAX_PATH];

    USHORT i;
    PGET_PROFILE_DIRS GetProfileDirs;

    for (i = 0; i < _countof(GetProfileDirsFuncsList); ++i)
    {
        GetProfileDirs = &GetProfileDirsFuncsList[i];

        SetLastError(0xdeadbeef);
        Success = GetProfileDirs->pFunc(NULL, NULL);
        dwLastError = GetLastError();
        ok(!Success, "%S: Expected failure, got success instead\n", GetProfileDirs->pFuncName);
        ok(dwLastError == ERROR_INVALID_PARAMETER, "%S: Expected error %lu, got %lu\n",
           GetProfileDirs->pFuncName, (DWORD)ERROR_INVALID_PARAMETER, dwLastError);

        SetLastError(0xdeadbeef);
        Success = GetProfileDirs->pFunc(szProfileDir, NULL);
        dwLastError = GetLastError();
        ok(!Success, "%S: Expected failure, got success instead\n", GetProfileDirs->pFuncName);
        ok(dwLastError == ERROR_INVALID_PARAMETER, "%S: Expected error %lu, got %lu\n",
           GetProfileDirs->pFuncName, (DWORD)ERROR_INVALID_PARAMETER, dwLastError);

        cchSize = 0;
        SetLastError(0xdeadbeef);
        Success = GetProfileDirs->pFunc(NULL, &cchSize);
        dwLastError = GetLastError();
        ok(!Success, "%S: Expected failure, got success instead\n", GetProfileDirs->pFuncName);
        ok(dwLastError == ERROR_INSUFFICIENT_BUFFER, "%S: Expected error %lu, got %lu\n",
           GetProfileDirs->pFuncName, (DWORD)ERROR_INSUFFICIENT_BUFFER, dwLastError);
        ok(cchSize != 0, "%S: Expected a profile directory size != 0, got 0\n", GetProfileDirs->pFuncName);

        cchSize = 0;
        SetLastError(0xdeadbeef);
        Success = GetProfileDirs->pFunc(szProfileDir, &cchSize);
        dwLastError = GetLastError();
        ok(!Success, "%S: Expected failure, got success instead\n", GetProfileDirs->pFuncName);
        ok(dwLastError == ERROR_INSUFFICIENT_BUFFER, "%S: Expected error %lu, got %lu\n",
           GetProfileDirs->pFuncName, (DWORD)ERROR_INSUFFICIENT_BUFFER, dwLastError);
        ok(cchSize != 0, "%S: Expected a profile directory size != 0, got 0\n", GetProfileDirs->pFuncName);

        cchSize = _countof(szProfileDir);
        SetLastError(0xdeadbeef);
        Success = GetProfileDirs->pFunc(szProfileDir, &cchSize);
        dwLastError = GetLastError();
        ok(Success, "%S: Expected to success, got failure instead\n", GetProfileDirs->pFuncName);
        ok(dwLastError == 0xdeadbeef, "%S: Expected error %lu, got %lu\n",
           GetProfileDirs->pFuncName, (DWORD)0xdeadbeef, dwLastError);
        ok(cchSize != 0, "%S: Expected a profile directory size != 0, got 0\n", GetProfileDirs->pFuncName);
        ok(*szProfileDir, "%S: Expected a profile directory, got nothing\n", GetProfileDirs->pFuncName);
    }

    TestProfileListValue(L"ProgramData", GetAllUsersProfileDirectoryW, L"GetAllUsersProfileDirectoryW");
    TestProfileListValue(L"Default", GetDefaultUserProfileDirectoryW, L"GetDefaultUserProfileDirectoryW");
    TestProfileListValue(L"ProfilesDirectory", GetProfilesDirectoryW, L"GetProfilesDirectoryW");
    TestProfileListValue(L"Public", NULL, NULL);

    // TODO: Add more tests!
}
