/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetSystemWow64DirectoryA/W
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

static BOOL HaveWow64Directory(void)
{
#ifdef _WIN64
    return TRUE;
#else
    BOOL IsWow64 = FALSE;
    return IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64;
#endif
}

static void Test_Unsupported(void)
{
    WCHAR BufferW[MAX_PATH];
    CHAR BufferA[MAX_PATH];
    UINT Ret;

    SetLastError(0xdeadbeef);
    Ret = GetSystemWow64DirectoryW(BufferW, _countof(BufferW));
    ok_int(Ret, 0);
    ok_err(ERROR_CALL_NOT_IMPLEMENTED);

    SetLastError(0xdeadbeef);
    Ret = GetSystemWow64DirectoryA(BufferA, _countof(BufferA));
    ok_int(Ret, 0);
    ok_err(ERROR_CALL_NOT_IMPLEMENTED);
}

static void Test_Supported(void)
{
    WCHAR ExpectedW[MAX_PATH], BufferW[MAX_PATH];
    CHAR ExpectedA[MAX_PATH], BufferA[MAX_PATH];
    UINT Ret, LenW, LenA;

    /* The WoW64 system directory is SysWOW64 under the system windows directory */
    LenW = GetSystemWindowsDirectoryW(ExpectedW, _countof(ExpectedW));
    ok(LenW != 0 && LenW < _countof(ExpectedW), "GetSystemWindowsDirectoryW returned %u\n", LenW);
    if (LenW == 0 || LenW >= _countof(ExpectedW))
        return;
    if (ExpectedW[LenW - 1] != L'\\')
        ExpectedW[LenW++] = L'\\';
    ExpectedW[LenW] = UNICODE_NULL;
    StringCchCatW(ExpectedW, _countof(ExpectedW), L"SysWOW64");
    LenW = lstrlenW(ExpectedW);

    LenA = GetSystemWindowsDirectoryA(ExpectedA, _countof(ExpectedA));
    ok(LenA != 0 && LenA < _countof(ExpectedA), "GetSystemWindowsDirectoryA returned %u\n", LenA);
    if (LenA == 0 || LenA >= _countof(ExpectedA))
        return;
    if (ExpectedA[LenA - 1] != '\\')
        ExpectedA[LenA++] = '\\';
    ExpectedA[LenA] = ANSI_NULL;
    StringCchCatA(ExpectedA, _countof(ExpectedA), "SysWOW64");
    LenA = lstrlenA(ExpectedA);

    /* Big enough buffer: the length without the terminator */
    FillMemory(BufferW, sizeof(BufferW), 0x55);
    Ret = GetSystemWow64DirectoryW(BufferW, _countof(BufferW));
    ok_int(Ret, LenW);
    ok(lstrcmpiW(BufferW, ExpectedW) == 0, "Got '%S', expected '%S'\n", BufferW, ExpectedW);

    /* Exactly big enough */
    FillMemory(BufferW, sizeof(BufferW), 0x55);
    Ret = GetSystemWow64DirectoryW(BufferW, LenW + 1);
    ok_int(Ret, LenW);
    ok(lstrcmpiW(BufferW, ExpectedW) == 0, "Got '%S', expected '%S'\n", BufferW, ExpectedW);

    /* Too small: the required size including the terminator */
    Ret = GetSystemWow64DirectoryW(BufferW, LenW);
    ok_int(Ret, LenW + 1);
    Ret = GetSystemWow64DirectoryW(NULL, 0);
    ok_int(Ret, LenW + 1);

    FillMemory(BufferA, sizeof(BufferA), 0x55);
    Ret = GetSystemWow64DirectoryA(BufferA, _countof(BufferA));
    ok_int(Ret, LenA);
    ok(lstrcmpiA(BufferA, ExpectedA) == 0, "Got '%s', expected '%s'\n", BufferA, ExpectedA);

    FillMemory(BufferA, sizeof(BufferA), 0x55);
    Ret = GetSystemWow64DirectoryA(BufferA, LenA + 1);
    ok_int(Ret, LenA);
    ok(lstrcmpiA(BufferA, ExpectedA) == 0, "Got '%s', expected '%s'\n", BufferA, ExpectedA);

    Ret = GetSystemWow64DirectoryA(BufferA, LenA);
    ok_int(Ret, LenA + 1);
    Ret = GetSystemWow64DirectoryA(NULL, 0);
    ok_int(Ret, LenA + 1);
}

START_TEST(GetSystemWow64Directory)
{
    if (HaveWow64Directory())
        Test_Supported();
    else
        Test_Unsupported();
}
