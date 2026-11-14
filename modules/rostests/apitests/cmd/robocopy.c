/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for robocopy.exe (copy, merge, move, purge, exclude, exit codes)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

static WCHAR s_Root[MAX_PATH];
static CHAR s_Output[64 * 1024];
static BOOL s_HaveRobocopy;

/* Build "<root>\<rel>" */
static PCWSTR
P(PCWSTR Rel)
{
    static WCHAR Buf[8][MAX_PATH * 2];
    static INT Index;
    PWSTR p = Buf[Index++ % 8];
    StringCchPrintfW(p, MAX_PATH * 2, L"%s\\%s", s_Root, Rel);
    return p;
}

static void
SetTimeDays(PCWSTR Path, INT DaysAgo)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    HANDLE h;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* Whole even seconds, so FAT volumes keep it exactly */
    u.QuadPart -= (ULONGLONG)DaysAgo * 86400ULL * 10000000ULL;
    u.QuadPart -= u.QuadPart % (20000000ULL);
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    h = CreateFileW(Path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(h != INVALID_HANDLE_VALUE, "Cannot open %S for times: %lu\n", Path, GetLastError());
    if (h != INVALID_HANDLE_VALUE)
    {
        ok(SetFileTime(h, NULL, NULL, &ft), "SetFileTime(%S) failed\n", Path);
        CloseHandle(h);
    }
}

static void
MakeFile(PCWSTR Rel, PCSTR Data, INT DaysAgo)
{
    PCWSTR Path = P(Rel);
    HANDLE h;
    DWORD Written;

    h = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(h != INVALID_HANDLE_VALUE, "Cannot create %S: %lu\n", Path, GetLastError());
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, Data, (DWORD)strlen(Data), &Written, NULL);
    CloseHandle(h);
    if (DaysAgo >= 0)
        SetTimeDays(Path, DaysAgo);
}

static void
MakeDir(PCWSTR Rel)
{
    ok(CreateDirectoryW(P(Rel), NULL) || GetLastError() == ERROR_ALREADY_EXISTS,
       "CreateDirectory(%S) failed: %lu\n", Rel, GetLastError());
}

static BOOL
IsFile(PCWSTR Rel)
{
    DWORD a = GetFileAttributesW(P(Rel));
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL
IsDir(PCWSTR Rel)
{
    DWORD a = GetFileAttributesW(P(Rel));
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL
Exists(PCWSTR Rel)
{
    return GetFileAttributesW(P(Rel)) != INVALID_FILE_ATTRIBUTES;
}

static BOOL
FileHas(PCWSTR Rel, PCSTR Data)
{
    CHAR Buf[256];
    DWORD Read = 0;
    HANDLE h = CreateFileW(P(Rel), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(h, Buf, sizeof(Buf) - 1, &Read, NULL);
    CloseHandle(h);
    Buf[Read] = 0;
    return strcmp(Buf, Data) == 0;
}

static void
DeleteTree(PCWSTR Path)
{
    WCHAR Pattern[MAX_PATH * 2], Child[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    StringCchPrintfW(Pattern, ARRAYSIZE(Pattern), L"%s\\*", Path);
    h = FindFirstFileW(Pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            StringCchPrintfW(Child, ARRAYSIZE(Child), L"%s\\%s", Path, fd.cFileName);
            SetFileAttributesW(Child, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(Child);
            else
                DeleteFileW(Child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(Path);
}

/* Fresh tree:
 *   s\a.txt  s\b.log  s\sub\c.txt  s\sub\deep\d.txt  s\empty\  s\skip\e.txt
 */
static void
MakeSource(void)
{
    DeleteTree(s_Root);
    ok(CreateDirectoryW(s_Root, NULL), "Cannot create %S: %lu\n", s_Root, GetLastError());
    MakeDir(L"s");
    MakeDir(L"s\\sub");
    MakeDir(L"s\\sub\\deep");
    MakeDir(L"s\\empty");
    MakeDir(L"s\\skip");
    MakeFile(L"s\\a.txt", "alpha", 10);
    MakeFile(L"s\\b.log", "bravo!", 10);
    MakeFile(L"s\\sub\\c.txt", "charlie", 10);
    MakeFile(L"s\\sub\\deep\\d.txt", "delta", 10);
    MakeFile(L"s\\skip\\e.txt", "echo", 10);
}

/* Run "robocopy <Args>" with the working directory at the test root.
 * Returns the exit code; output goes to s_Output. */
static DWORD
RunRobocopy(PCWSTR Args)
{
    WCHAR CmdLine[2048];
    WCHAR OutPath[MAX_PATH];
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hOut;
    DWORD Code = 0xDEAD, Read = 0;

    s_Output[0] = 0;
    StringCchPrintfW(OutPath, ARRAYSIZE(OutPath), L"%s.out", s_Root);
    hOut = CreateFileW(OutPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        ok(0, "Cannot create output file: %lu\n", GetLastError());
        return Code;
    }

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"robocopy.exe %s", Args);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError = hOut;

    if (!CreateProcessW(NULL, CmdLine, NULL, NULL, TRUE, 0, NULL, s_Root, &si, &pi))
    {
        ok(0, "CreateProcess(%S) failed: %lu\n", CmdLine, GetLastError());
        CloseHandle(hOut);
        DeleteFileW(OutPath);
        return Code;
    }
    ok(WaitForSingleObject(pi.hProcess, 60 * 1000) == WAIT_OBJECT_0, "robocopy %S timed out\n", Args);
    GetExitCodeProcess(pi.hProcess, &Code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    SetFilePointer(hOut, 0, NULL, FILE_BEGIN);
    ReadFile(hOut, s_Output, sizeof(s_Output) - 1, &Read, NULL);
    s_Output[Read] = 0;
    CloseHandle(hOut);
    DeleteFileW(OutPath);
    return Code;
}

#define ok_rc(args, expected) \
    do { DWORD _rc = RunRobocopy(args); \
         ok(_rc == (expected), "robocopy %S: exit code %lu, expected %u\n%s\n", args, _rc, (expected), \
            _rc == (expected) ? "" : s_Output); } while (0)

#define ok_out(substr, present) \
    ok((strstr(s_Output, substr) != NULL) == (present), "Output %s '%s'\n", \
       (present) ? "lacks" : "has", substr)

static void
Test_CopyAndMerge(void)
{
    MakeSource();

    /* Top level only, no subdirectories */
    ok_rc(L"s d /NP", 1);
    ok(FileHas(L"d\\a.txt", "alpha"), "d\\a.txt not copied\n");
    ok(FileHas(L"d\\b.log", "bravo!"), "d\\b.log not copied\n");
    ok(!Exists(L"d\\sub"), "d\\sub must not exist without /S\n");

    /* Job header and summary are printed by default */
    ok_out("ROBOCOPY", TRUE);
    ok_out("Total    Copied   Skipped  Mismatch    FAILED    Extras", TRUE);
    ok_out("a.txt", TRUE);

    /* Nothing changed: exit code 0 */
    ok_rc(L"s d /NJH /NJS", 0);
    ok_out("ROBOCOPY", FALSE);
    ok_out("Total", FALSE);

    /* Merge: /S copies subdirectories but no empty ones */
    ok_rc(L"s d /S /NJH /NJS /NP", 1);
    ok(FileHas(L"d\\sub\\deep\\d.txt", "delta"), "d\\sub\\deep\\d.txt not copied\n");
    ok(!Exists(L"d\\empty"), "d\\empty must not be created by /S\n");

    /* /E adds the empty ones */
    ok_rc(L"s d /E /NJH /NJS /NP", 0);
    ok(IsDir(L"d\\empty"), "d\\empty not created by /E\n");

    /* Source newer than destination: copied again */
    MakeFile(L"s\\a.txt", "alpha2", 1);
    ok_rc(L"s d /E /NJH /NJS /NP", 1);
    ok(FileHas(L"d\\a.txt", "alpha2"), "newer d\\a.txt not copied\n");
    ok_out("Newer", TRUE);

    /* /XO: source older than destination is left alone */
    MakeFile(L"d\\a.txt", "dest-newer", 0);
    ok_rc(L"s d /XO /NJH /NJS /NP", 0);
    ok(FileHas(L"d\\a.txt", "dest-newer"), "/XO overwrote a newer destination\n");
    /* ...but without /XO it is overwritten (Older) */
    ok_rc(L"s d /NJH /NJS /NP", 1);
    ok(FileHas(L"d\\a.txt", "alpha2"), "older file not copied without /XO\n");

    /* A read-only destination is still updated */
    MakeFile(L"s\\b.log", "bravo2", 1);
    SetFileAttributesW(P(L"d\\b.log"), FILE_ATTRIBUTE_READONLY);
    ok_rc(L"s d /NJH /NJS /NP /R:0 /W:0", 1);
    ok(FileHas(L"d\\b.log", "bravo2"), "read-only destination not updated\n");

    /* Wildcards in the file specification */
    ok_rc(L"s w *.txt /S /NJH /NJS /NP", 1);
    ok(IsFile(L"w\\a.txt") && IsFile(L"w\\sub\\c.txt"), "*.txt files not copied\n");
    ok(!Exists(L"w\\b.log"), "*.txt copied b.log\n");
    ok_rc(L"s w2 a.* b.log /NJH /NJS /NP", 1);
    ok(IsFile(L"w2\\a.txt") && IsFile(L"w2\\b.log"), "a.* b.log not copied\n");

    /* /LEV:2 stops below the first level of subdirectories */
    ok_rc(L"s lev /E /LEV:2 /NJH /NJS /NP", 1);
    ok(IsFile(L"lev\\sub\\c.txt"), "/LEV:2 did not copy level 2\n");
    ok(!Exists(L"lev\\sub\\deep"), "/LEV:2 copied level 3\n");

    /* /L lists only */
    ok_rc(L"s list /L /E /NJH /NJS", 1);
    ok(!Exists(L"list"), "/L created the destination\n");
    ok_out("a.txt", TRUE);

    /* Timestamps are copied (/COPY:DAT) */
    {
        WIN32_FILE_ATTRIBUTE_DATA a, b;
        ok(GetFileAttributesExW(P(L"s\\sub\\c.txt"), GetFileExInfoStandard, &a) &&
           GetFileAttributesExW(P(L"d\\sub\\c.txt"), GetFileExInfoStandard, &b), "attributes\n");
        ok(CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0, "LastWriteTime not copied\n");
    }

    /* Deep destination paths are created */
    ok_rc(L"s n1\\n2\\n3 /NJH /NJS /NP", 1);
    ok(IsFile(L"n1\\n2\\n3\\a.txt"), "deep destination not created\n");

    /* Listing suppression */
    ok_rc(L"s quiet /NFL /NDL /NJH /NJS /NP", 1);
    ok_out("a.txt", FALSE);
    ok(s_Output[0] == 0 || strspn(s_Output, "\r\n ") == strlen(s_Output), "Unexpected output: %s\n", s_Output);
}

static void
Test_ExtrasPurgeExclude(void)
{
    MakeSource();
    ok_rc(L"s d /E /NJH /NJS /NP", 1);

    MakeFile(L"d\\extra.txt", "x", 5);
    MakeFile(L"d\\keep.log", "k", 5);
    MakeDir(L"d\\xdir");
    MakeFile(L"d\\xdir\\z.txt", "z", 5);
    MakeDir(L"d\\protect");

    /* Extras are reported (bit 2), but not deleted without /PURGE */
    ok_rc(L"s d /E /NJH /NJS /NP", 2);
    ok(IsFile(L"d\\extra.txt") && IsDir(L"d\\xdir"), "extras deleted without /PURGE\n");
    ok_out("*EXTRA File", TRUE);
    ok_out("*EXTRA Dir", TRUE);

    /* /XX: still counted for the exit code, but neither listed nor purged */
    ok_rc(L"s d /MIR /XX /NJH /NJS /NP", 2);
    ok_out("*EXTRA", FALSE);
    ok(IsFile(L"d\\extra.txt") && IsDir(L"d\\xdir"), "/MIR /XX purged extras\n");

    /* /MIR purges, but excluded names are protected */
    ok_rc(L"s d /MIR /XF *.log /XD protect skip /NJH /NJS /NP", 2);
    ok(!Exists(L"d\\extra.txt"), "/MIR did not purge extra.txt\n");
    ok(!Exists(L"d\\xdir"), "/MIR did not purge xdir\n");
    ok(IsFile(L"d\\keep.log"), "/XF *.log did not protect keep.log\n");
    ok(IsDir(L"d\\protect"), "/XD protect did not protect the directory\n");
    ok(IsDir(L"d\\skip"), "skip was copied before and is excluded now\n");

    /* /PURGE alone on a fresh tree with exclusions */
    MakeSource();
    ok_rc(L"s d /E /XD skip /XF b.log /NJH /NJS /NP", 1);
    ok(!Exists(L"d\\skip"), "/XD skip copied the directory\n");
    ok(!Exists(L"d\\b.log"), "/XF b.log copied the file\n");
    ok(IsFile(L"d\\a.txt"), "a.txt missing\n");

    MakeFile(L"d\\gone.txt", "g", 5);
    ok_rc(L"s d /PURGE /NJH /NJS /NP", 3);   /* b.log is copied now; gone.txt is purged */
    ok(!Exists(L"d\\gone.txt"), "/PURGE did not delete gone.txt\n");
    ok(IsFile(L"d\\b.log"), "b.log not copied\n");
}

static void
Test_Move(void)
{
    /* /MOV moves files only */
    MakeSource();
    ok_rc(L"s d /MOV /S /NJH /NJS /NP", 1);
    ok(IsFile(L"d\\sub\\deep\\d.txt"), "/MOV did not copy\n");
    ok(!Exists(L"s\\a.txt") && !Exists(L"s\\sub\\deep\\d.txt"), "/MOV left source files\n");
    ok(IsDir(L"s\\sub\\deep"), "/MOV removed source directories\n");

    /* /MOVE moves files and directories, including the source root */
    MakeSource();
    ok_rc(L"s d /MOVE /E /NJH /NJS /NP", 1);
    ok(!Exists(L"s"), "/MOVE left the source tree\n");
    ok(IsFile(L"d\\sub\\deep\\d.txt") && IsDir(L"d\\empty"), "/MOVE did not move everything\n");

    /* Scoop's movedir: move a subdirectory's content into its parent */
    MakeSource();
    MakeDir(L"s\\sub\\deep\\inner");
    MakeFile(L"s\\sub\\deep\\inner\\i.txt", "india", 3);
    {
        DWORD rc = RunRobocopy(L"\"s\\sub\" \"s\" /e /move");
        ok(rc < 8, "movedir: exit code %lu\n%s\n", rc, s_Output);
        ok(rc & 1, "movedir: nothing copied (%lu)\n", rc);
    }
    ok(!Exists(L"s\\sub"), "movedir left the source directory\n");
    ok(FileHas(L"s\\c.txt", "charlie"), "movedir did not move c.txt\n");
    ok(FileHas(L"s\\deep\\d.txt", "delta"), "movedir did not move deep\\d.txt\n");
    ok(FileHas(L"s\\deep\\inner\\i.txt", "india"), "movedir did not move inner\\i.txt\n");
    ok(FileHas(L"s\\a.txt", "alpha"), "movedir damaged the destination\n");
}

static void
Test_MismatchFailErrors(void)
{
    HANDLE h;

    /* A directory where the source has a file, and vice versa: bit 4 */
    MakeSource();
    MakeDir(L"d");
    MakeDir(L"d\\a.txt");
    MakeFile(L"d\\sub", "file-not-dir", 5);
    ok_rc(L"s d /E /NJH /NJS /NP", 5);
    ok(IsDir(L"d\\a.txt") && IsFile(L"d\\sub"), "mismatched entries were replaced\n");
    ok(IsFile(L"d\\b.log"), "b.log not copied\n");

    /* A copy that keeps failing: bit 8 */
    MakeSource();
    h = CreateFileW(P(L"s\\a.txt"), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(h != INVALID_HANDLE_VALUE, "cannot lock a.txt\n");
    ok_rc(L"s d /R:0 /W:0 /NJH /NJS /NP", 9);
    ok_out("ERROR 32 (0x00000020)", TRUE);
    CloseHandle(h);
    ok_rc(L"s d /R:0 /W:0 /NJH /NJS /NP", 1);

    /* Fatal errors: bit 16 */
    ok_rc(L"s d /BOGUS", 16);
    ok_out("Invalid Parameter #3 : \"/BOGUS\"", TRUE);
    ok_rc(L"s d /LEV:x", 16);
    ok_rc(L"s d /COPY:Q", 16);
    ok_rc(L"", 16);
    ok_rc(L"s", 16);
    ok_rc(L"nosuchdir d /NJH", 16);
    ok_out("ERROR 2 (0x00000002) Accessing Source Directory", TRUE);
    ok_rc(L"/?", 16);

    /* Options are case-insensitive and '-' works as a prefix */
    MakeSource();
    ok_rc(L"s d -e /njh /njs /np /r:0 /w:0", 1);
    ok(IsDir(L"d\\empty"), "-e not honored\n");
}

START_TEST(robocopy)
{
    WCHAR Temp[MAX_PATH], Path[MAX_PATH];
    DWORD Len;

    GetTempPathW(ARRAYSIZE(Temp), Temp);
    StringCchPrintfW(s_Root, ARRAYSIZE(s_Root), L"%srobocopy_test_%lu", Temp, GetCurrentProcessId());

    Len = SearchPathW(NULL, L"robocopy.exe", NULL, ARRAYSIZE(Path), Path, NULL);
    s_HaveRobocopy = (Len != 0);
    ok(s_HaveRobocopy, "robocopy.exe not found in PATH\n");
    if (!s_HaveRobocopy)
        return;

    Test_CopyAndMerge();
    Test_ExtrasPurgeExclude();
    Test_Move();
    Test_MismatchFailErrors();

    DeleteTree(s_Root);
}
