/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetFileSize
 */

#include "precomp.h"

START_TEST(GetFileSize)
{
    WCHAR Path[MAX_PATH], Dir[MAX_PATH];
    HANDLE Event, File;
    DWORD Low, High, Written;

    /* A handle that cannot be queried fails with INVALID_FILE_SIZE,
     * whether or not the caller asks for the high part */
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(Event != NULL, "CreateEventW failed with %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Low = GetFileSize(Event, NULL);
    ok(Low == INVALID_FILE_SIZE, "GetFileSize(NULL) returned %lu\n", Low);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "GetLastError() = %lu\n", GetLastError());

    High = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Low = GetFileSize(Event, &High);
    ok(Low == INVALID_FILE_SIZE, "GetFileSize(&High) returned %lu\n", Low);
    ok(GetLastError() == ERROR_INVALID_HANDLE, "GetLastError() = %lu\n", GetLastError());
    ok(High == 0xdeadbeef, "High = %lu\n", High);
    CloseHandle(Event);

    /* A real file reports its size and a cleared error */
    GetTempPathW(ARRAYSIZE(Dir), Dir);
    GetTempFileNameW(Dir, L"gfs", 0, Path);
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    ok(WriteFile(File, "12345", 5, &Written, NULL) && Written == 5, "WriteFile failed\n");

    High = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Low = GetFileSize(File, &High);
    ok(Low == 5, "GetFileSize returned %lu\n", Low);
    ok(High == 0, "High = %lu\n", High);
    CloseHandle(File);
}
