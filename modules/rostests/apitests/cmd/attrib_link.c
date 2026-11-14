/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for attrib.exe on junctions (/L) and directory trees (/S /D)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <strsafe.h>

static WCHAR s_Root[MAX_PATH];
static CHAR s_Output[16 * 1024];

static PCWSTR
P(PCWSTR Rel)
{
    static WCHAR Buf[8][MAX_PATH * 2];
    static INT Index;
    PWSTR p = Buf[Index++ % 8];
    StringCchPrintfW(p, MAX_PATH * 2, L"%s\\%s", s_Root, Rel);
    return p;
}

static DWORD
Attr(PCWSTR Rel)
{
    return GetFileAttributesW(P(Rel));
}

/* Attributes of the target of a link (the open follows the reparse point) */
static DWORD
TargetAttr(PCWSTR Rel)
{
    BY_HANDLE_FILE_INFORMATION Info;
    HANDLE h;
    DWORD a = INVALID_FILE_ATTRIBUTES;

    h = CreateFileW(P(Rel), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return a;
    if (GetFileInformationByHandle(h, &Info))
        a = Info.dwFileAttributes;
    CloseHandle(h);
    return a;
}

static void
MakeFile(PCWSTR Rel)
{
    HANDLE h = CreateFileW(P(Rel), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(h != INVALID_HANDLE_VALUE, "Cannot create %S: %lu\n", Rel, GetLastError());
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static void
MakeDir(PCWSTR Rel)
{
    ok(CreateDirectoryW(P(Rel), NULL), "CreateDirectory(%S) failed: %lu\n", Rel, GetLastError());
}

/* Make the empty directory Rel a junction (mount point) to the directory Target */
static BOOL
MakeJunction(PCWSTR Rel, PCWSTR Target)
{
    BYTE Buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    struct
    {
        DWORD ReparseTag;
        WORD ReparseDataLength;
        WORD Reserved;
        WORD SubstituteNameOffset;
        WORD SubstituteNameLength;
        WORD PrintNameOffset;
        WORD PrintNameLength;
        WCHAR PathBuffer[1];
    } *Rp = (void *)Buffer;
    WCHAR Subst[MAX_PATH * 2];
    WCHAR Full[MAX_PATH * 2];
    DWORD SubstLen, PrintLen, Returned;
    HANDLE h;
    BOOL Ret;

    StringCchCopyW(Full, ARRAYSIZE(Full), P(Target));
    StringCchPrintfW(Subst, ARRAYSIZE(Subst), L"\\??\\%s", Full);
    SubstLen = (DWORD)wcslen(Subst) * sizeof(WCHAR);
    PrintLen = (DWORD)wcslen(Full) * sizeof(WCHAR);

    ZeroMemory(Buffer, sizeof(Buffer));
    Rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Rp->SubstituteNameOffset = 0;
    Rp->SubstituteNameLength = (WORD)SubstLen;
    Rp->PrintNameOffset = (WORD)(SubstLen + sizeof(WCHAR));
    Rp->PrintNameLength = (WORD)PrintLen;
    memcpy(Rp->PathBuffer, Subst, SubstLen);
    memcpy((PBYTE)Rp->PathBuffer + SubstLen + sizeof(WCHAR), Full, PrintLen);
    Rp->ReparseDataLength = (WORD)(4 * sizeof(WORD) + SubstLen + PrintLen + 2 * sizeof(WCHAR));

    h = CreateFileW(P(Rel), GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, Rp, Rp->ReparseDataLength + 8, NULL, 0, &Returned, NULL);
    CloseHandle(h);
    return Ret;
}

static void
Cleanup(void)
{
    static PCWSTR Files[] = { L"tgt\\f.txt", L"tree\\a.txt", L"tree\\sub\\b.txt" };
    static PCWSTR Dirs[] = { L"lnk", L"tgt", L"tree\\sub", L"tree" };
    SIZE_T i;

    for (i = 0; i < ARRAYSIZE(Files); i++)
    {
        SetFileAttributesW(P(Files[i]), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(P(Files[i]));
    }
    for (i = 0; i < ARRAYSIZE(Dirs); i++)
    {
        SetFileAttributesW(P(Dirs[i]), FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryW(P(Dirs[i]));
    }
    RemoveDirectoryW(s_Root);
}

/* Run "attrib <Args>" in the test root; output (stdout + stderr) goes to s_Output */
static DWORD
RunAttrib(PCWSTR Args)
{
    WCHAR CmdLine[1024];
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

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"attrib.exe %s", Args);
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
    ok(WaitForSingleObject(pi.hProcess, 30 * 1000) == WAIT_OBJECT_0, "attrib %S timed out\n", Args);
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

#define ok_attrib(args) \
    do { DWORD _rc = RunAttrib(args); \
         ok(_rc == 0, "attrib %S: exit code %ld\n%s\n", args, _rc, s_Output); \
         ok(strstr(s_Output, "Invalid parameter") == NULL, "attrib %S: %s\n", args, s_Output); } while (0)

#define HAS(a, f)   ((a) != INVALID_FILE_ATTRIBUTES && ((a) & (f)))
#define LACKS(a, f) ((a) != INVALID_FILE_ATTRIBUTES && !((a) & (f)))

static void
Test_Link(void)
{
    MakeDir(L"tgt");
    MakeFile(L"tgt\\f.txt");
    MakeDir(L"lnk");
    if (!MakeJunction(L"lnk", L"tgt"))
    {
        skip("Cannot create a junction: %lu\n", GetLastError());
        return;
    }
    ok(HAS(Attr(L"lnk"), FILE_ATTRIBUTE_REPARSE_POINT), "lnk is not a reparse point: 0x%lx\n", Attr(L"lnk"));
    ok(GetFileAttributesW(P(L"lnk\\f.txt")) != INVALID_FILE_ATTRIBUTES, "junction does not resolve\n");

    /* /L: the link itself (what Scoop does after every install) */
    ok_attrib(L"lnk +R /L");
    ok(HAS(Attr(L"lnk"), FILE_ATTRIBUTE_READONLY), "+R /L: link 0x%lx\n", Attr(L"lnk"));
    ok(LACKS(Attr(L"tgt"), FILE_ATTRIBUTE_READONLY), "+R /L changed the target: 0x%lx\n", Attr(L"tgt"));
    ok(HAS(Attr(L"lnk"), FILE_ATTRIBUTE_REPARSE_POINT), "+R /L dropped the reparse point\n");

    /* Display with /L shows the link's attributes */
    ok_attrib(L"/L lnk");
    ok(strstr(s_Output, "    R ") != NULL || strstr(s_Output, "   R ") != NULL,
       "attrib /L lnk does not show R: %s\n", s_Output);

    ok_attrib(L"-R /L lnk");
    ok(LACKS(Attr(L"lnk"), FILE_ATTRIBUTE_READONLY), "-R /L: link 0x%lx\n", Attr(L"lnk"));

    /* Without /L ATTRIB works on the target of the link */
    ok_attrib(L"+H lnk");
    ok(HAS(Attr(L"tgt"), FILE_ATTRIBUTE_HIDDEN), "+H: target 0x%lx\n", Attr(L"tgt"));
    ok(LACKS(Attr(L"lnk"), FILE_ATTRIBUTE_HIDDEN), "+H changed the link itself: 0x%lx\n", Attr(L"lnk"));
    ok(HAS(TargetAttr(L"lnk"), FILE_ATTRIBUTE_HIDDEN), "+H: target via link 0x%lx\n", TargetAttr(L"lnk"));

    ok_attrib(L"lnk");
    ok(strstr(s_Output, " H ") != NULL, "attrib lnk does not show the target's H: %s\n", s_Output);
    ok_attrib(L"/L lnk");
    ok(strstr(s_Output, " H ") == NULL, "attrib /L lnk shows the target's H: %s\n", s_Output);

    ok_attrib(L"-H lnk");
    ok(LACKS(Attr(L"tgt"), FILE_ATTRIBUTE_HIDDEN), "-H: target 0x%lx\n", Attr(L"tgt"));

    /* The files behind the link are untouched by all of the above */
    ok(LACKS(Attr(L"tgt\\f.txt"), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
       "tgt\\f.txt 0x%lx\n", Attr(L"tgt\\f.txt"));
}

static void
Test_Tree(void)
{
    MakeDir(L"tree");
    MakeDir(L"tree\\sub");
    MakeFile(L"tree\\a.txt");
    MakeFile(L"tree\\sub\\b.txt");

    /* /S: files in all subdirectories, but not the directories */
    ok_attrib(L"+H /S tree\\*");
    ok(HAS(Attr(L"tree\\a.txt"), FILE_ATTRIBUTE_HIDDEN), "/S: a.txt 0x%lx\n", Attr(L"tree\\a.txt"));
    ok(HAS(Attr(L"tree\\sub\\b.txt"), FILE_ATTRIBUTE_HIDDEN), "/S: b.txt 0x%lx\n", Attr(L"tree\\sub\\b.txt"));
    ok(LACKS(Attr(L"tree\\sub"), FILE_ATTRIBUTE_HIDDEN), "/S without /D changed sub: 0x%lx\n", Attr(L"tree\\sub"));

    /* /S /D: the directories as well */
    ok_attrib(L"+R /S /D tree\\*");
    ok(HAS(Attr(L"tree\\sub"), FILE_ATTRIBUTE_READONLY), "/S /D: sub 0x%lx\n", Attr(L"tree\\sub"));
    ok(HAS(Attr(L"tree\\sub\\b.txt"), FILE_ATTRIBUTE_READONLY), "/S /D: b.txt 0x%lx\n", Attr(L"tree\\sub\\b.txt"));

    ok_attrib(L"-R -H /S /D tree\\*");
    ok(LACKS(Attr(L"tree\\sub"), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN), "sub 0x%lx\n", Attr(L"tree\\sub"));
    ok(LACKS(Attr(L"tree\\a.txt"), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN), "a.txt 0x%lx\n", Attr(L"tree\\a.txt"));
    ok(LACKS(Attr(L"tree\\sub\\b.txt"), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
       "b.txt 0x%lx\n", Attr(L"tree\\sub\\b.txt"));
}

START_TEST(attrib_link)
{
    WCHAR Dir[MAX_PATH], Volume[MAX_PATH];
    DWORD FsFlags = 0;

    /* Junctions need a file system with reparse points: prefer the current
     * directory (e.g. an NTFS volume), else the temporary directory. */
    GetCurrentDirectoryW(ARRAYSIZE(Dir), Dir);
    if (!GetVolumePathNameW(Dir, Volume, ARRAYSIZE(Volume)) ||
        !GetVolumeInformationW(Volume, NULL, 0, NULL, NULL, &FsFlags, NULL, 0) ||
        !(FsFlags & FILE_SUPPORTS_REPARSE_POINTS))
    {
        GetTempPathW(ARRAYSIZE(Dir), Dir);
    }
    StringCchPrintfW(s_Root, ARRAYSIZE(s_Root), L"%s%sattrib-link-test", Dir,
                     Dir[wcslen(Dir) - 1] == L'\\' ? L"" : L"\\");

    Cleanup();
    ok(CreateDirectoryW(s_Root, NULL), "Cannot create %S: %lu\n", s_Root, GetLastError());

    Test_Tree();
    Test_Link();

    Cleanup();
}
