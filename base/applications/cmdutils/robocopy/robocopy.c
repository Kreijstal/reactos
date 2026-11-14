/*
 * PROJECT:     ReactOS Robust File Copy
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ROBOCOPY: copy, merge, move and mirror directory trees
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winnt.h>
#include <strsafe.h>
#include <conutils.h>
#include "resource.h"

/* Exit code bits, as documented for Windows robocopy */
#define RC_COPIED       0x01
#define RC_EXTRA        0x02
#define RC_MISMATCH     0x04
#define RC_FAILED       0x08
#define RC_FATAL        0x10

/* Statistics rows and columns of the job summary */
enum { ROW_DIRS, ROW_FILES, ROW_BYTES, ROW_COUNT };
enum { COL_TOTAL, COL_COPIED, COL_SKIPPED, COL_MISMATCH, COL_FAILED, COL_EXTRAS, COL_COUNT };

#define FT_SECOND       10000000ULL
#define FT_DAY          (86400ULL * FT_SECOND)

#define ATTR_SETTABLE   (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | \
                         FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY | \
                         FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
#define ATTR_TWEAK_MASK (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | \
                         FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | \
                         FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE)

typedef struct _STRLIST
{
    PWSTR *Items;
    SIZE_T Count;
    SIZE_T Capacity;
} STRLIST;

typedef struct _ENTRY
{
    PWSTR Name;
    DWORD Attr;
    FILETIME Create;
    FILETIME Access;
    FILETIME Write;
    ULONGLONG Size;
} ENTRY;

typedef struct _ENTRYLIST
{
    ENTRY *Items;
    SIZE_T Count;
    SIZE_T Capacity;
} ENTRYLIST;

/* One directory level of the walk */
typedef struct _DIRCTX
{
    struct _DIRCTX *Parent;
    PWSTR Src;          /* full source path, with trailing backslash */
    PWSTR Dst;          /* full destination path, with trailing backslash */
    BOOL DstExisted;    /* destination existed before we got there */
    BOOL DstReady;      /* destination exists now (or would, in /L mode) */
    DWORD SrcAttr;
    FILETIME SrcCreate, SrcAccess, SrcWrite;
    DWORD Level;        /* the root is level 1 */
} DIRCTX;

typedef struct _OPTIONS
{
    /* Copy options */
    BOOL Subdirs, Empty, Purge, Mirror, Move, MoveFiles, Create, NoCopy;
    BOOL CopyAll, Sec, SecFix, TimFix, Restartable, Backup, RestartBackup;
    BOOL Unbuffered, EfsRaw, NoOffload, Fat, No256, SymLinks, SparseOpt, Compress, NoClone, SJ;
    DWORD Level;
    BOOL CopyD, CopyA, CopyT, CopyS, CopyO, CopyU, CopyX;
    WCHAR CopyFlags[16];
    BOOL DCopyGiven, NoDCopy;
    BOOL DCopyD, DCopyA, DCopyT, DCopyE, DCopyX;
    WCHAR DCopyFlags[16];
    DWORD AttrAdd, AttrRemove, AttrInclude, AttrExclude;
    WCHAR AttrAddStr[16], AttrRemoveStr[16], AttrIncludeStr[16], AttrExcludeStr[16];
    DWORD Threads;
    PWSTR MonStr, MotStr, RhStr, IpgStr, IoMaxStr, IoRateStr, ThresholdStr, LfsmStr;
    BOOL PerFile, Reg, Tbd, Lfsm;

    /* File selection options */
    BOOL ArchiveOnly, ResetArchive;
    BOOL ExcludeChanged, ExcludeNewer, ExcludeOlder, ExcludeExtra, ExcludeLonely;
    BOOL IncludeSame, IncludeTweaked, IncludeModified;
    BOOL ExcludeJunctions, ExcludeJunctionDirs, ExcludeJunctionFiles;
    BOOL FatTimes, DstComp;
    BOOL HasMax, HasMin;
    ULONGLONG MaxSize, MinSize;
    BOOL HasMaxAge, HasMinAge, HasMaxLad, HasMinLad;
    DWORD MaxAge, MinAge, MaxLad, MinLad;
    ULONGLONG MaxAgeFt, MinAgeFt, MaxLadFt, MinLadFt;

    /* Retry options */
    DWORD Retries, Wait;

    /* Logging options */
    BOOL ListOnly, ReportAllExtra, Verbose, TimeStamps, FullPath, Bytes;
    BOOL NoSize, NoClass, NoFileList, NoDirList, NoProgress, Eta, Tee;
    BOOL NoJobHeader, NoJobSummary, Unicode, Debug;
    PWSTR LogFile;
    BOOL LogAppend, LogUnicode;

    /* Job options */
    PWSTR SaveJob;
    BOOL Quit, NoSD, NoDD;

    STRLIST Files, XFiles, XDirs;
    PWSTR Src, Dst;
    PWSTR SrcFull, DstFull;
} OPTIONS;

static OPTIONS Opt;
static ULONGLONG Stats[ROW_COUNT][COL_COUNT];
static ULONGLONG CopyTicks, FailTicks, ExtraTicks;
static HANDLE hLog = NULL;
static BOOL ToConsole = TRUE;
static BOOL Fatal = FALSE;
static ULONGLONG NowFt;

/* ---------------------------------------------------------------------- */
/* Memory and strings                                                     */
/* ---------------------------------------------------------------------- */

static void OutOfMemory(void);

static PVOID
XAlloc(SIZE_T Size)
{
    PVOID p = calloc(1, Size ? Size : 1);
    if (!p)
        OutOfMemory();
    return p;
}

static PVOID
XRealloc(PVOID Old, SIZE_T Size)
{
    PVOID p = realloc(Old, Size ? Size : 1);
    if (!p)
        OutOfMemory();
    return p;
}

static PWSTR
XStrDup(PCWSTR s)
{
    SIZE_T n = wcslen(s) + 1;
    PWSTR p = XAlloc(n * sizeof(WCHAR));
    memcpy(p, s, n * sizeof(WCHAR));
    return p;
}

static PWSTR
XConcat(PCWSTR a, PCWSTR b, PCWSTR c)
{
    SIZE_T la = a ? wcslen(a) : 0, lb = b ? wcslen(b) : 0, lc = c ? wcslen(c) : 0;
    PWSTR p = XAlloc((la + lb + lc + 1) * sizeof(WCHAR));
    if (la) memcpy(p, a, la * sizeof(WCHAR));
    if (lb) memcpy(p + la, b, lb * sizeof(WCHAR));
    if (lc) memcpy(p + la + lb, c, lc * sizeof(WCHAR));
    p[la + lb + lc] = 0;
    return p;
}

static void
StrListAdd(STRLIST *List, PCWSTR s)
{
    if (List->Count == List->Capacity)
    {
        List->Capacity = List->Capacity ? List->Capacity * 2 : 8;
        List->Items = XRealloc(List->Items, List->Capacity * sizeof(PWSTR));
    }
    List->Items[List->Count++] = XStrDup(s);
}

static void
StrListInsert(STRLIST *List, SIZE_T Index, PCWSTR s)
{
    StrListAdd(List, s);
    if (Index < List->Count - 1)
    {
        PWSTR Last = List->Items[List->Count - 1];
        memmove(&List->Items[Index + 1], &List->Items[Index],
                (List->Count - 1 - Index) * sizeof(PWSTR));
        List->Items[Index] = Last;
    }
}

/* ---------------------------------------------------------------------- */
/* Output                                                                 */
/* ---------------------------------------------------------------------- */

static PCWSTR
Res(UINT Id)
{
    static PWSTR Cache[512];
    PCWSTR Ptr = NULL;
    INT Len;

    if (Id < ARRAYSIZE(Cache) && Cache[Id])
        return Cache[Id];

    Len = LoadStringW(GetModuleHandleW(NULL), Id, (PWSTR)&Ptr, 0);
    if (Len <= 0 || !Ptr)
        return L"";

    if (Id < ARRAYSIZE(Cache))
    {
        Cache[Id] = malloc((Len + 1) * sizeof(WCHAR));
        if (!Cache[Id])
            return L"";
        memcpy(Cache[Id], Ptr, Len * sizeof(WCHAR));
        Cache[Id][Len] = 0;
        return Cache[Id];
    }
    return L"";
}

static void
WriteLog(PCWSTR s)
{
    SIZE_T i, Len = wcslen(s), Start = 0;
    DWORD Written;

    /* Translate LF into CRLF and convert to the log encoding */
    for (i = 0; i <= Len; i++)
    {
        if (i == Len || s[i] == L'\n')
        {
            SIZE_T Chunk = i - Start;
            if (Chunk)
            {
                if (Opt.LogUnicode)
                {
                    WriteFile(hLog, s + Start, (DWORD)(Chunk * sizeof(WCHAR)), &Written, NULL);
                }
                else
                {
                    INT n = WideCharToMultiByte(CP_ACP, 0, s + Start, (INT)Chunk, NULL, 0, NULL, NULL);
                    if (n > 0)
                    {
                        PSTR a = malloc(n);
                        if (a)
                        {
                            WideCharToMultiByte(CP_ACP, 0, s + Start, (INT)Chunk, a, n, NULL, NULL);
                            WriteFile(hLog, a, n, &Written, NULL);
                            free(a);
                        }
                    }
                }
            }
            if (i < Len)
            {
                if (Opt.LogUnicode)
                    WriteFile(hLog, L"\r\n", 2 * sizeof(WCHAR), &Written, NULL);
                else
                    WriteFile(hLog, "\r\n", 2, &Written, NULL);
            }
            Start = i + 1;
        }
    }
}

static void
Emit(PCWSTR s)
{
    if (ToConsole)
        ConPuts(StdOut, s);
    if (hLog)
        WriteLog(s);
}

static void
OutV(PCWSTR Format, va_list Args)
{
    WCHAR Small[512];
    PWSTR Buf = Small;
    INT Len;
    va_list Copy;

    va_copy(Copy, Args);
    Len = _vscwprintf(Format, Copy);
    va_end(Copy);
    if (Len < 0)
        return;
    if (Len >= (INT)ARRAYSIZE(Small))
        Buf = XAlloc((Len + 1) * sizeof(WCHAR));
    _vsnwprintf(Buf, Len + 1, Format, Args);
    Buf[Len] = 0;
    Emit(Buf);
    if (Buf != Small)
        free(Buf);
}

static void
Out(PCWSTR Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    OutV(Format, Args);
    va_end(Args);
}

static void
OutRes(UINT Id, ...)
{
    va_list Args;
    va_start(Args, Id);
    OutV(Res(Id), Args);
    va_end(Args);
}

static void
OutOfMemory(void)
{
    ConPuts(StdOut, L"ERROR : Not enough memory.\n");
    ExitProcess(RC_FATAL);
}

/* "2026/11/14 10:00:00" */
static void
FormatErrorTime(PWSTR Buf, SIZE_T Cch)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    StringCchPrintfW(Buf, Cch, L"%04u/%02u/%02u %02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void
ReportError(DWORD Error, UINT CtxId, PCWSTR Path)
{
    WCHAR When[32];
    PWSTR Msg = NULL;
    FormatErrorTime(When, ARRAYSIZE(When));

    if (!FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0, (PWSTR)&Msg, 0, NULL))
    {
        Msg = NULL;
    }
    else
    {
        /* Strip the trailing line break, we print our own */
        SIZE_T n = wcslen(Msg);
        while (n && (Msg[n - 1] == L'\n' || Msg[n - 1] == L'\r'))
            Msg[--n] = 0;
    }
    OutRes(IDS_ERR_FORMAT, When, Error, Error, Res(CtxId), Path, Msg ? Msg : L"");
    if (Msg)
        LocalFree(Msg);
}

/* Robocopy-style size: bytes, then k/m/g/t with truncated decimals */
static void
FormatSize(ULONGLONG Size, PWSTR Buf, SIZE_T Cch, BOOL Listing)
{
    static const WCHAR Units[] = L"kmgt";
    ULONGLONG Unit = 1024;
    INT i;

    if (Opt.Bytes || Size < 1024)
    {
        StringCchPrintfW(Buf, Cch, L"%I64u", Size);
        return;
    }
    if (Listing && Size < 1024 * 1024)
    {
        /* File lists show plain bytes below one megabyte */
        StringCchPrintfW(Buf, Cch, L"%I64u", Size);
        return;
    }

    for (i = 0; i < 3 && Size >= Unit * 1024; i++)
        Unit *= 1024;

    if (Listing || i == 0)
    {
        ULONGLONG Tenths = (Size * 10) / Unit;
        if (Size >= (1ULL << 60) / 10)
            Tenths = (Size / Unit) * 10;
        StringCchPrintfW(Buf, Cch, L"%I64u.%I64u %c", Tenths / 10, Tenths % 10, Units[i]);
    }
    else if (i == 1)
    {
        ULONGLONG Hund = (Size * 100) / Unit;
        StringCchPrintfW(Buf, Cch, L"%I64u.%02I64u %c", Hund / 100, Hund % 100, Units[i]);
    }
    else
    {
        ULONGLONG Thou = (Size / 1024) * 1000 / (Unit / 1024);
        StringCchPrintfW(Buf, Cch, L"%I64u.%03I64u %c", Thou / 1000, Thou % 1000, Units[i]);
    }
}

static void
FormatDuration(ULONGLONG Ms, PWSTR Buf, SIZE_T Cch)
{
    ULONGLONG s = Ms / 1000;
    StringCchPrintfW(Buf, Cch, L"%I64u:%02I64u:%02I64u", s / 3600, (s / 60) % 60, s % 60);
}

static void
FormatLongDate(PWSTR Buf, SIZE_T Cch)
{
    SYSTEMTIME st;
    WCHAR Date[128], Time[64];

    GetLocalTime(&st);
    if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL, Date, ARRAYSIZE(Date)))
        Date[0] = 0;
    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, Time, ARRAYSIZE(Time)))
        Time[0] = 0;
    StringCchPrintfW(Buf, Cch, L"%ls %ls", Date, Time);
}

static void
FormatFileTime(const FILETIME *Ft, PWSTR Buf, SIZE_T Cch)
{
    FILETIME Local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(Ft, &Local);
    FileTimeToSystemTime(&Local, &st);
    StringCchPrintfW(Buf, Cch, L"%04u/%02u/%02u %02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void
PrintDirLine(UINT ClassId, LONGLONG Count, PCWSTR Path)
{
    WCHAR Num[32];
    if (Opt.NoDirList)
        return;
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%I64d", Count);
    Out(L"\t%-10ls%10ls\t%ls\n", (ClassId && !Opt.NoClass) ? Res(ClassId) : L"", Num, Path);
}

static void
PrintFileLine(UINT ClassId, const ENTRY *Entry, PCWSTR Dir)
{
    WCHAR Size[32], Stamp[48];

    if (Opt.NoFileList)
        return;

    if (Opt.NoSize)
        Size[0] = 0;
    else
        FormatSize(Entry->Size, Size, ARRAYSIZE(Size), TRUE);

    Stamp[0] = 0;
    if (Opt.TimeStamps)
    {
        FormatFileTime(&Entry->Write, Stamp, ARRAYSIZE(Stamp) - 1);
        StringCchCatW(Stamp, ARRAYSIZE(Stamp), L"\t");
    }

    Out(L"\t%ls\t\t%8ls\t%ls%ls%ls\n",
        Opt.NoClass ? L"" : Res(ClassId), Size, Stamp,
        (Opt.FullPath || Opt.Threads) ? Dir : L"", Entry->Name);
}

static void
PrintProgress(void)
{
    if (Opt.NoProgress || Opt.NoFileList || Opt.ListOnly || Opt.Threads)
        return;
    Out(L"  0%%  \r100%%  \n");
}

/* ---------------------------------------------------------------------- */
/* Paths and file system wrappers                                         */
/* ---------------------------------------------------------------------- */

/* Path suitable for Win32 calls: no trailing backslash (except roots),
 * and a \\?\ prefix when the path is long. */
static PWSTR
ApiPath(PCWSTR Path)
{
    SIZE_T Len = wcslen(Path);
    PWSTR p;

    while (Len > 1 && Path[Len - 1] == L'\\' &&
           !(Len == 3 && Path[1] == L':') &&
           !(Len == 7 && !wcsncmp(Path, L"\\\\?\\", 4) && Path[5] == L':'))
    {
        Len--;
    }

    if (Len >= MAX_PATH - 12 && wcsncmp(Path, L"\\\\?\\", 4) != 0)
    {
        if (Path[0] == L'\\' && Path[1] == L'\\')
        {
            p = XAlloc((Len + 8 + 1) * sizeof(WCHAR));
            memcpy(p, L"\\\\?\\UNC", 7 * sizeof(WCHAR));
            memcpy(p + 7, Path + 1, (Len - 1) * sizeof(WCHAR));
            p[Len + 6] = 0;
            return p;
        }
        p = XAlloc((Len + 4 + 1) * sizeof(WCHAR));
        memcpy(p, L"\\\\?\\", 4 * sizeof(WCHAR));
        memcpy(p + 4, Path, Len * sizeof(WCHAR));
        p[Len + 4] = 0;
        return p;
    }

    p = XAlloc((Len + 1) * sizeof(WCHAR));
    memcpy(p, Path, Len * sizeof(WCHAR));
    p[Len] = 0;
    return p;
}

static DWORD
XGetAttributes(PCWSTR Path)
{
    PWSTR p = ApiPath(Path);
    DWORD Attr = GetFileAttributesW(p);
    free(p);
    return Attr;
}

static BOOL
XSetAttributes(PCWSTR Path, DWORD Attr)
{
    PWSTR p = ApiPath(Path);
    BOOL Ret = SetFileAttributesW(p, Attr ? Attr : FILE_ATTRIBUTE_NORMAL);
    free(p);
    return Ret;
}

static BOOL
XCreateDirectory(PCWSTR Path)
{
    PWSTR p = ApiPath(Path);
    BOOL Ret = CreateDirectoryW(p, NULL);
    if (!Ret && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        DWORD Attr = GetFileAttributesW(p);
        if (Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_DIRECTORY))
            Ret = TRUE;
        else
            SetLastError(ERROR_ALREADY_EXISTS);
    }
    free(p);
    return Ret;
}

static BOOL
XRemoveDirectory(PCWSTR Path)
{
    PWSTR p = ApiPath(Path);
    BOOL Ret = RemoveDirectoryW(p);
    if (!Ret && GetLastError() == ERROR_ACCESS_DENIED)
    {
        DWORD Attr = GetFileAttributesW(p);
        if (Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_READONLY))
        {
            SetFileAttributesW(p, Attr & ~FILE_ATTRIBUTE_READONLY);
            Ret = RemoveDirectoryW(p);
        }
    }
    free(p);
    return Ret;
}

static BOOL
XDeleteFile(PCWSTR Path, DWORD Attr)
{
    PWSTR p = ApiPath(Path);
    BOOL Ret;
    if (Attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    Ret = DeleteFileW(p);
    free(p);
    return Ret;
}

static BOOL
XSetTimes(PCWSTR Path, BOOL IsDir, const FILETIME *Create, const FILETIME *Access, const FILETIME *Write)
{
    PWSTR p = ApiPath(Path);
    HANDLE h = CreateFileW(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, IsDir ? FILE_FLAG_BACKUP_SEMANTICS : 0, NULL);
    BOOL Ret = FALSE;
    free(p);
    if (h != INVALID_HANDLE_VALUE)
    {
        Ret = SetFileTime(h, Create, Access, Write);
        CloseHandle(h);
    }
    return Ret;
}

/* Create a directory and all of its missing parents */
static BOOL
XCreateDirectoryTree(PCWSTR Path)
{
    PWSTR Copy, p;
    SIZE_T Len, Start = 0;
    BOOL Ret;

    if (XCreateDirectory(Path))
        return TRUE;
    if (GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND)
        return FALSE;

    Copy = XStrDup(Path);
    Len = wcslen(Copy);
    if (Copy[0] == L'\\' && Copy[1] == L'\\')
    {
        /* Skip \\server\share\ */
        INT Seps = 0;
        for (Start = 2; Start < Len && Seps < 2; Start++)
            if (Copy[Start] == L'\\')
                Seps++;
    }
    else if (Len >= 3 && Copy[1] == L':')
    {
        Start = 3;
    }

    for (p = Copy + Start; *p; p++)
    {
        if (*p == L'\\' && p[1])
        {
            *p = 0;
            XCreateDirectory(Copy);
            *p = L'\\';
        }
    }
    free(Copy);
    Ret = XCreateDirectory(Path);
    return Ret;
}

/* ---------------------------------------------------------------------- */
/* Directory listing                                                      */
/* ---------------------------------------------------------------------- */

static int __cdecl
EntryCompare(const void *a, const void *b)
{
    return _wcsicmp(((const ENTRY *)a)->Name, ((const ENTRY *)b)->Name);
}

static void
EntryListAdd(ENTRYLIST *List, const WIN32_FIND_DATAW *Fd)
{
    ENTRY *e;
    if (List->Count == List->Capacity)
    {
        List->Capacity = List->Capacity ? List->Capacity * 2 : 16;
        List->Items = XRealloc(List->Items, List->Capacity * sizeof(ENTRY));
    }
    e = &List->Items[List->Count++];
    e->Name = XStrDup(Fd->cFileName);
    e->Attr = Fd->dwFileAttributes;
    e->Create = Fd->ftCreationTime;
    e->Access = Fd->ftLastAccessTime;
    e->Write = Fd->ftLastWriteTime;
    e->Size = ((ULONGLONG)Fd->nFileSizeHigh << 32) | Fd->nFileSizeLow;
}

static void
EntryListFree(ENTRYLIST *List)
{
    SIZE_T i;
    for (i = 0; i < List->Count; i++)
        free(List->Items[i].Name);
    free(List->Items);
    List->Items = NULL;
    List->Count = List->Capacity = 0;
}

static ENTRY *
EntryFind(const ENTRYLIST *List, PCWSTR Name)
{
    ENTRY Key;
    if (!List->Count)
        return NULL;
    Key.Name = (PWSTR)Name;
    return bsearch(&Key, List->Items, List->Count, sizeof(ENTRY), EntryCompare);
}

/* Returns ERROR_SUCCESS, or the error of FindFirstFile */
static DWORD
ReadDirectory(PCWSTR Dir, ENTRYLIST *Files, ENTRYLIST *Dirs)
{
    WIN32_FIND_DATAW Fd;
    PWSTR Pattern, Api;
    HANDLE h;
    DWORD Error = ERROR_SUCCESS;

    Pattern = XConcat(Dir, L"*", NULL);
    Api = ApiPath(Pattern);
    free(Pattern);
    /* ApiPath() may have prefixed it; it never strips a trailing '*' */
    h = FindFirstFileW(Api, &Fd);
    free(Api);
    if (h == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_NO_MORE_FILES)
            Error = ERROR_SUCCESS;
        return Error;
    }

    do
    {
        if (Fd.cFileName[0] == L'.' &&
            (Fd.cFileName[1] == 0 || (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == 0)))
        {
            continue;
        }
        if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            EntryListAdd(Dirs, &Fd);
        else
            EntryListAdd(Files, &Fd);
    } while (FindNextFileW(h, &Fd));

    Error = GetLastError();
    FindClose(h);
    if (Error == ERROR_NO_MORE_FILES)
        Error = ERROR_SUCCESS;

    if (Files->Count > 1)
        qsort(Files->Items, Files->Count, sizeof(ENTRY), EntryCompare);
    if (Dirs->Count > 1)
        qsort(Dirs->Items, Dirs->Count, sizeof(ENTRY), EntryCompare);
    return Error;
}

/* ---------------------------------------------------------------------- */
/* Matching                                                               */
/* ---------------------------------------------------------------------- */

static BOOL
WildMatchRaw(PCWSTR Pat, PCWSTR Str)
{
    PCWSTR StarPat = NULL, StarStr = NULL;

    while (*Str)
    {
        if (*Pat == L'*')
        {
            while (*Pat == L'*')
                Pat++;
            if (!*Pat)
                return TRUE;
            StarPat = Pat;
            StarStr = Str;
        }
        else if (*Pat == L'?' || towupper(*Pat) == towupper(*Str))
        {
            Pat++;
            Str++;
        }
        else if (StarPat)
        {
            Pat = StarPat;
            Str = ++StarStr;
        }
        else
        {
            return FALSE;
        }
    }
    while (*Pat == L'*')
        Pat++;
    return *Pat == 0;
}

/* DOS-style wildcard match: "*.*" matches everything, "x.*" also matches
 * "x", and a trailing "." matches names without an extension. */
static BOOL
WildMatch(PCWSTR Pat, PCWSTR Str)
{
    SIZE_T Len;

    if (!wcscmp(Pat, L"*") || !wcscmp(Pat, L"*.*"))
        return TRUE;
    if (WildMatchRaw(Pat, Str))
        return TRUE;

    Len = wcslen(Pat);
    if (!wcschr(Str, L'.'))
    {
        if (Len >= 2 && Pat[Len - 2] == L'.' && Pat[Len - 1] == L'*')
        {
            PWSTR Short = XStrDup(Pat);
            BOOL Ret;
            Short[Len - 2] = 0;
            Ret = WildMatchRaw(Short, Str);
            free(Short);
            return Ret;
        }
        if (Len >= 1 && Pat[Len - 1] == L'.')
        {
            PWSTR Short = XStrDup(Pat);
            BOOL Ret;
            Short[Len - 1] = 0;
            Ret = WildMatchRaw(Short, Str);
            free(Short);
            return Ret;
        }
    }
    return FALSE;
}

static BOOL
MatchesFileSpec(PCWSTR Name)
{
    SIZE_T i;
    if (!Opt.Files.Count)
        return TRUE;
    for (i = 0; i < Opt.Files.Count; i++)
        if (WildMatch(Opt.Files.Items[i], Name))
            return TRUE;
    return FALSE;
}

/* Match a name or a full path (without trailing backslash) against an exclusion list */
static BOOL
MatchesList(const STRLIST *List, PCWSTR Dir, PCWSTR Name)
{
    SIZE_T i;
    PWSTR Full = NULL;
    BOOL Ret = FALSE;

    for (i = 0; i < List->Count && !Ret; i++)
    {
        PCWSTR Pat = List->Items[i];
        if (wcschr(Pat, L'\\'))
        {
            SIZE_T Len;
            PWSTR PatCopy;
            if (!Full)
                Full = XConcat(Dir, Name, NULL);
            PatCopy = XStrDup(Pat);
            Len = wcslen(PatCopy);
            while (Len > 1 && PatCopy[Len - 1] == L'\\')
                PatCopy[--Len] = 0;
            Ret = WildMatchRaw(PatCopy, Full);
            free(PatCopy);
        }
        else
        {
            Ret = WildMatch(Pat, Name);
        }
    }
    free(Full);
    return Ret;
}

/* ---------------------------------------------------------------------- */
/* Comparison                                                             */
/* ---------------------------------------------------------------------- */

static ULONGLONG
Ft64(const FILETIME *Ft)
{
    return ((ULONGLONG)Ft->dwHighDateTime << 32) | Ft->dwLowDateTime;
}

/* <0: a older than b, 0: same, >0: a newer than b */
static INT
CompareTimes(const FILETIME *a, const FILETIME *b)
{
    LONGLONG Diff = (LONGLONG)(Ft64(a) - Ft64(b));
    LONGLONG Tol = Opt.FatTimes ? (LONGLONG)(2 * FT_SECOND) : 0;
    LONGLONG Abs = Diff < 0 ? -Diff : Diff;

    if (Abs <= Tol)
        return 0;
    if (Opt.DstComp)
    {
        LONGLONG Hour = 3600 * (LONGLONG)FT_SECOND;
        LONGLONG d = Abs - Hour;
        if (d < 0) d = -d;
        if (d <= (Tol ? Tol : 2 * (LONGLONG)FT_SECOND))
            return 0;
    }
    return Diff < 0 ? -1 : 1;
}

/* Attribute filters (/A, /M, /IA, /XA, /XJF): FALSE if the file is excluded */
static BOOL
PassesAttributeFilters(const ENTRY *e)
{
    if ((Opt.ArchiveOnly || Opt.ResetArchive) && !(e->Attr & FILE_ATTRIBUTE_ARCHIVE))
        return FALSE;
    if (Opt.AttrInclude && !(e->Attr & Opt.AttrInclude))
        return FALSE;
    if (Opt.AttrExclude && (e->Attr & Opt.AttrExclude))
        return FALSE;
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Copy engine                                                            */
/* ---------------------------------------------------------------------- */

static void
AddStat(INT Row, INT Col, ULONGLONG Count, ULONGLONG Bytes)
{
    Stats[Row][Col] += Count;
    if (Row == ROW_FILES)
        Stats[ROW_BYTES][Col] += Bytes;
}

static BOOL
EnsureDestination(DIRCTX *Ctx)
{
    if (Ctx->DstReady)
        return TRUE;
    if (Ctx->Parent && !EnsureDestination(Ctx->Parent))
        return FALSE;
    if (!Opt.ListOnly)
    {
        if (!XCreateDirectory(Ctx->Dst))
        {
            ReportError(GetLastError(), IDS_CTX_CREATE_DIR, Ctx->Dst);
            return FALSE;
        }
        if (Opt.DCopyA && !Opt.NoDCopy)
        {
            DWORD Attr = Ctx->SrcAttr & (ATTR_SETTABLE & ~FILE_ATTRIBUTE_NORMAL);
            if (Attr)
                XSetAttributes(Ctx->Dst, Attr);
        }
    }
    Ctx->DstReady = TRUE;
    return TRUE;
}

static DWORD
CopyOneFile(PCWSTR SrcPath, PCWSTR DstPath, const ENTRY *Src, const ENTRY *Dst)
{
    PWSTR ApiSrc, ApiDst;
    DWORD Error = ERROR_SUCCESS;
    DWORD Attr;

    if (Dst && (Dst->Attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
        XSetAttributes(DstPath, FILE_ATTRIBUTE_NORMAL);

    ApiSrc = ApiPath(SrcPath);
    ApiDst = ApiPath(DstPath);

    if (Opt.Create || Opt.NoCopy || !Opt.CopyD)
    {
        /* /CREATE: zero-length files. Without D, keep existing data. */
        HANDLE h;
        if (Opt.Create || !Dst)
        {
            h = CreateFileW(ApiDst, GENERIC_WRITE, 0, NULL,
                            Opt.Create ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE)
                Error = GetLastError();
            else
                CloseHandle(h);
        }
    }
    else
    {
        if (!CopyFileExW(ApiSrc, ApiDst, NULL, NULL, NULL, 0))
            Error = GetLastError();
    }

    if (Error == ERROR_SUCCESS)
    {
        /* Timestamps */
        if (Opt.CopyT && !Opt.NoCopy)
        {
            XSetTimes(DstPath, FALSE, &Src->Create, &Src->Access, &Src->Write);
        }
        else if (Opt.CopyD && !Opt.NoCopy && !Opt.Create)
        {
            /* Without T the copy carries the time it was made */
            FILETIME Now;
            GetSystemTimeAsFileTime(&Now);
            XSetTimes(DstPath, FALSE, &Now, &Now, &Now);
        }

        /* Attributes */
        if (Opt.CopyA && !Opt.NoCopy)
            Attr = Src->Attr & (ATTR_SETTABLE & ~FILE_ATTRIBUTE_NORMAL);
        else
            Attr = FILE_ATTRIBUTE_ARCHIVE;
        Attr |= Opt.AttrAdd;
        Attr &= ~Opt.AttrRemove;
        if (!XSetAttributes(DstPath, Attr))
            Error = GetLastError();
    }

    free(ApiSrc);
    free(ApiDst);
    return Error;
}

static DWORD
CopyWithRetries(PCWSTR SrcPath, PCWSTR DstPath, const ENTRY *Src, const ENTRY *Dst)
{
    DWORD Try, Error;
    ULONGLONG Start;

    for (Try = 0; ; Try++)
    {
        Start = (ULONGLONG)GetTickCount();
        Error = CopyOneFile(SrcPath, DstPath, Src, Dst);
        CopyTicks += (DWORD)((DWORD)GetTickCount() - (DWORD)Start);
        if (Error == ERROR_SUCCESS)
            return Error;

        ReportError(Error, IDS_CTX_COPY_FILE, SrcPath);
        if (Try >= Opt.Retries)
            break;
        OutRes(IDS_ERR_RETRY_WAIT, Opt.Wait);
        Start = (ULONGLONG)GetTickCount();
        Sleep(Opt.Wait * 1000);
        FailTicks += (DWORD)((DWORD)GetTickCount() - (DWORD)Start);
        OutRes(IDS_ERR_RETRYING);
    }
    OutRes(IDS_ERR_RETRY_LIMIT);
    return Error;
}

/* Delete an extra destination directory tree (/PURGE), listing its contents */
static void
PurgeTree(PCWSTR Dir)
{
    ENTRYLIST Files = {0}, Dirs = {0};
    SIZE_T i;
    DWORD Error;

    Error = ReadDirectory(Dir, &Files, &Dirs);
    if (Error != ERROR_SUCCESS)
        ReportError(Error, IDS_CTX_SCAN_DST, Dir);

    for (i = 0; i < Files.Count; i++)
    {
        ENTRY *e = &Files.Items[i];
        PWSTR Path = XConcat(Dir, e->Name, NULL);
        AddStat(ROW_FILES, COL_EXTRAS, 1, e->Size);
        PrintFileLine(IDS_CLS_EXTRAFILE, e, Dir);
        if (!Opt.ListOnly && !XDeleteFile(Path, e->Attr))
            ReportError(GetLastError(), IDS_CTX_DELETE_EXTRA, Path);
        free(Path);
    }
    for (i = 0; i < Dirs.Count; i++)
    {
        PWSTR Path = XConcat(Dir, Dirs.Items[i].Name, L"\\");
        AddStat(ROW_DIRS, COL_EXTRAS, 1, 0);
        PrintDirLine(IDS_DCLS_EXTRADIR, -1, Path);
        if (Dirs.Items[i].Attr & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            /* Never follow a junction while deleting: remove the link only */
            if (!Opt.ListOnly && !XRemoveDirectory(Path))
                ReportError(GetLastError(), IDS_CTX_DELETE_EXTRADIR, Path);
        }
        else
        {
            PurgeTree(Path);
        }
        free(Path);
    }
    EntryListFree(&Files);
    EntryListFree(&Dirs);

    if (!Opt.ListOnly && !XRemoveDirectory(Dir))
        ReportError(GetLastError(), IDS_CTX_DELETE_EXTRADIR, Dir);
}

static BOOL
IsJunction(DWORD Attr)
{
    return (Attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static void ProcessDirectory(DIRCTX *Ctx);

static void
ProcessExtras(DIRCTX *Ctx, const ENTRYLIST *SrcFiles, const ENTRYLIST *SrcDirs,
              const ENTRYLIST *DstFiles, const ENTRYLIST *DstDirs)
{
    SIZE_T i;
    ULONGLONG Start = (ULONGLONG)GetTickCount();

    /* Extra directories first */
    for (i = 0; i < DstDirs->Count; i++)
    {
        ENTRY *d = &DstDirs->Items[i];
        PWSTR Path;

        if (EntryFind(SrcDirs, d->Name) || EntryFind(SrcFiles, d->Name))
            continue;
        if (MatchesList(&Opt.XDirs, Ctx->Dst, d->Name))
            continue;
        if (Opt.ExcludeJunctionDirs && IsJunction(d->Attr))
            continue;

        AddStat(ROW_DIRS, COL_EXTRAS, 1, 0);
        if (Opt.ExcludeExtra)
            continue;
        Path = XConcat(Ctx->Dst, d->Name, L"\\");
        PrintDirLine(IDS_DCLS_EXTRADIR, -1, Path);
        if (Opt.Purge)
        {
            if (IsJunction(d->Attr))
            {
                if (!Opt.ListOnly && !XRemoveDirectory(Path))
                    ReportError(GetLastError(), IDS_CTX_DELETE_EXTRADIR, Path);
            }
            else
            {
                PurgeTree(Path);
            }
        }
        free(Path);
    }

    /* Then extra files */
    for (i = 0; i < DstFiles->Count; i++)
    {
        ENTRY *f = &DstFiles->Items[i];

        if (EntryFind(SrcFiles, f->Name) || EntryFind(SrcDirs, f->Name))
            continue;
        if (!MatchesFileSpec(f->Name))
            continue;
        if (MatchesList(&Opt.XFiles, Ctx->Dst, f->Name))
            continue;

        AddStat(ROW_FILES, COL_EXTRAS, 1, f->Size);
        if (Opt.ExcludeExtra)
            continue;
        PrintFileLine(IDS_CLS_EXTRAFILE, f, Ctx->Dst);
        if (Opt.Purge && !Opt.ListOnly)
        {
            PWSTR Path = XConcat(Ctx->Dst, f->Name, NULL);
            if (!XDeleteFile(Path, f->Attr))
                ReportError(GetLastError(), IDS_CTX_DELETE_EXTRA, Path);
            free(Path);
        }
    }
    ExtraTicks += (DWORD)((DWORD)GetTickCount() - (DWORD)Start);
}

/* Returns the skip reason class for /V, or 0 if the file passes all filters */
static UINT
FileFilterClass(DIRCTX *Ctx, const ENTRY *f)
{
    if (MatchesList(&Opt.XFiles, Ctx->Src, f->Name))
        return IDS_CLS_V_NAMED;
    if (Opt.ExcludeJunctionFiles && IsJunction(f->Attr))
        return IDS_CLS_V_JUNCTION;
    if (!PassesAttributeFilters(f))
        return IDS_CLS_V_ATTRIB;
    if (Opt.HasMax && f->Size > Opt.MaxSize)
        return IDS_CLS_V_LARGE;
    if (Opt.HasMin && f->Size < Opt.MinSize)
        return IDS_CLS_V_SMALL;
    if (Opt.HasMaxAge && Ft64(&f->Write) < Opt.MaxAgeFt)
        return IDS_CLS_V_TOOOLD;
    if (Opt.HasMinAge && Ft64(&f->Write) > Opt.MinAgeFt)
        return IDS_CLS_V_TOONEW;
    if (Opt.HasMaxLad && Ft64(&f->Access) < Opt.MaxLadFt)
        return IDS_CLS_V_TOOOLD;
    if (Opt.HasMinLad && Ft64(&f->Access) > Opt.MinLadFt)
        return IDS_CLS_V_TOONEW;
    return 0;
}

static void
ProcessFiles(DIRCTX *Ctx, const ENTRYLIST *SrcFiles, const ENTRYLIST *DstFiles, const ENTRYLIST *DstDirs)
{
    SIZE_T i;

    for (i = 0; i < SrcFiles->Count; i++)
    {
        ENTRY *f = &SrcFiles->Items[i];
        ENTRY *d;
        UINT Class, Skip;
        BOOL DoCopy;
        PWSTR SrcPath, DstPath;

        if (!MatchesFileSpec(f->Name))
            continue;

        AddStat(ROW_FILES, COL_TOTAL, 1, f->Size);

        Skip = FileFilterClass(Ctx, f);
        if (Skip)
        {
            AddStat(ROW_FILES, COL_SKIPPED, 1, f->Size);
            if (Opt.Verbose)
                PrintFileLine(Skip, f, Ctx->Src);
            continue;
        }

        if (EntryFind(DstDirs, f->Name))
        {
            /* A directory is in the way */
            AddStat(ROW_FILES, COL_MISMATCH, 1, f->Size);
            PrintFileLine(IDS_CLS_MISMATCH, f, Ctx->Src);
            continue;
        }

        d = EntryFind(DstFiles, f->Name);
        if (!d)
        {
            Class = IDS_CLS_NEWFILE;
            DoCopy = !Opt.ExcludeLonely;
            if (!DoCopy)
                Skip = IDS_CLS_V_LONELY;
        }
        else
        {
            INT Cmp = CompareTimes(&f->Write, &d->Write);
            if (Cmp == 0 && f->Size == d->Size)
            {
                if ((f->Attr ^ d->Attr) & ATTR_TWEAK_MASK)
                {
                    Class = IDS_CLS_TWEAKED;
                    DoCopy = Opt.IncludeTweaked;
                    Skip = IDS_CLS_V_TWEAKED;
                }
                else
                {
                    Class = IDS_CLS_SAME;
                    DoCopy = Opt.IncludeSame;
                    Skip = IDS_CLS_V_SAME;
                }
            }
            else if (Cmp > 0)
            {
                Class = IDS_CLS_NEWER;
                DoCopy = !Opt.ExcludeNewer;
                Skip = IDS_CLS_V_NEWER;
            }
            else if (Cmp < 0)
            {
                Class = IDS_CLS_OLDER;
                DoCopy = !Opt.ExcludeOlder;
                Skip = IDS_CLS_V_OLDER;
            }
            else
            {
                Class = IDS_CLS_CHANGED;
                DoCopy = !Opt.ExcludeChanged;
                Skip = IDS_CLS_V_CHANGED;
            }
        }

        SrcPath = XConcat(Ctx->Src, f->Name, NULL);
        DstPath = XConcat(Ctx->Dst, f->Name, NULL);

        if (!DoCopy)
        {
            AddStat(ROW_FILES, COL_SKIPPED, 1, f->Size);
            if (Opt.Verbose)
                PrintFileLine(Skip, f, Ctx->Src);
            if (d && Opt.TimFix && !Opt.ListOnly && Opt.CopyT)
                XSetTimes(DstPath, FALSE, &f->Create, &f->Access, &f->Write);
        }
        else
        {
            PrintFileLine(Class, f, Ctx->Src);
            if (Opt.ListOnly)
            {
                AddStat(ROW_FILES, COL_COPIED, 1, f->Size);
            }
            else if (!EnsureDestination(Ctx))
            {
                AddStat(ROW_FILES, COL_FAILED, 1, f->Size);
            }
            else
            {
                DWORD Error = CopyWithRetries(SrcPath, DstPath, f, d);
                if (Error == ERROR_SUCCESS)
                {
                    PrintProgress();
                    AddStat(ROW_FILES, COL_COPIED, 1, f->Size);
                    if (Opt.ResetArchive)
                        XSetAttributes(SrcPath, f->Attr & ~FILE_ATTRIBUTE_ARCHIVE & ATTR_SETTABLE);
                    if (Opt.Move || Opt.MoveFiles)
                    {
                        if (!XDeleteFile(SrcPath, f->Attr))
                            ReportError(GetLastError(), IDS_CTX_DELETE_SRC, SrcPath);
                    }
                }
                else
                {
                    AddStat(ROW_FILES, COL_FAILED, 1, f->Size);
                }
            }
        }
        free(SrcPath);
        free(DstPath);
    }
}

static void
ProcessDirectory(DIRCTX *Ctx)
{
    ENTRYLIST SrcFiles = {0}, SrcDirs = {0}, DstFiles = {0}, DstDirs = {0};
    DWORD Error;
    SIZE_T i;
    LONGLONG Matching = 0;
    BOOL Recurse;

    Error = ReadDirectory(Ctx->Src, &SrcFiles, &SrcDirs);
    if (Error != ERROR_SUCCESS)
    {
        ReportError(Error, IDS_CTX_SCAN_SRC, Ctx->Src);
        AddStat(ROW_DIRS, COL_TOTAL, 1, 0);
        AddStat(ROW_DIRS, COL_FAILED, 1, 0);
        if (Ctx->Level == 1)
            Fatal = TRUE;
        goto Done;
    }
    if (Ctx->DstExisted)
    {
        Error = ReadDirectory(Ctx->Dst, &DstFiles, &DstDirs);
        if (Error != ERROR_SUCCESS)
            ReportError(Error, IDS_CTX_SCAN_DST, Ctx->Dst);
    }

    AddStat(ROW_DIRS, COL_TOTAL, 1, 0);
    AddStat(ROW_DIRS, Ctx->DstExisted ? COL_SKIPPED : COL_COPIED, 1, 0);

    for (i = 0; i < SrcFiles.Count; i++)
        if (MatchesFileSpec(SrcFiles.Items[i].Name))
            Matching++;
    PrintDirLine(Ctx->DstExisted ? 0 : IDS_DCLS_NEWDIR, Matching, Ctx->Src);

    /* /E creates every directory; /S only those that receive files */
    if ((Opt.Empty || Opt.Create || Ctx->Level == 1) && !Ctx->DstReady)
    {
        if (!EnsureDestination(Ctx) && Ctx->Level == 1)
        {
            Fatal = TRUE;
            goto Done;
        }
    }

    /* With /XX extras still count (exit code 2) but are neither listed nor purged */
    if (Ctx->DstExisted)
        ProcessExtras(Ctx, &SrcFiles, &SrcDirs, &DstFiles, &DstDirs);

    ProcessFiles(Ctx, &SrcFiles, &DstFiles, &DstDirs);

    Recurse = (Opt.Subdirs || Opt.Empty) && (Opt.Level == 0 || Ctx->Level < Opt.Level);
    if (Recurse)
    {
        BOOL *Skip = XAlloc((SrcDirs.Count + 1) * sizeof(BOOL));

        /* Report excluded and mismatched directories before descending */
        for (i = 0; i < SrcDirs.Count; i++)
        {
            ENTRY *s = &SrcDirs.Items[i];
            if (MatchesList(&Opt.XDirs, Ctx->Src, s->Name) ||
                ((Opt.ExcludeJunctions || Opt.ExcludeJunctionDirs) && IsJunction(s->Attr)))
            {
                Skip[i] = TRUE;
                if (Opt.Verbose)
                {
                    PWSTR Path = XConcat(Ctx->Src, s->Name, L"\\");
                    PrintDirLine(IDS_DCLS_EXCLUDED, -1, Path);
                    free(Path);
                }
                continue;
            }
            if (EntryFind(&DstFiles, s->Name))
            {
                PWSTR Path = XConcat(Ctx->Src, s->Name, L"\\");
                Skip[i] = TRUE;
                AddStat(ROW_DIRS, COL_TOTAL, 1, 0);
                AddStat(ROW_DIRS, COL_MISMATCH, 1, 0);
                PrintDirLine(IDS_DCLS_MISMATCH, -1, Path);
                free(Path);
            }
        }

        for (i = 0; i < SrcDirs.Count && !Fatal; i++)
        {
            ENTRY *s = &SrcDirs.Items[i];
            DIRCTX Child;

            if (Skip[i])
                continue;

            ZeroMemory(&Child, sizeof(Child));
            Child.Parent = Ctx;
            Child.Src = XConcat(Ctx->Src, s->Name, L"\\");
            Child.Dst = XConcat(Ctx->Dst, s->Name, L"\\");
            Child.DstExisted = EntryFind(&DstDirs, s->Name) != NULL;
            Child.DstReady = Child.DstExisted;
            Child.SrcAttr = s->Attr;
            Child.SrcCreate = s->Create;
            Child.SrcAccess = s->Access;
            Child.SrcWrite = s->Write;
            Child.Level = Ctx->Level + 1;

            ProcessDirectory(&Child);

            free(Child.Src);
            free(Child.Dst);
        }
        free(Skip);
    }

    /* Directory timestamps are set last, after the contents changed them */
    if (Opt.DCopyT && !Opt.NoDCopy && !Opt.ListOnly && Ctx->DstReady)
        XSetTimes(Ctx->Dst, TRUE, &Ctx->SrcCreate, &Ctx->SrcAccess, &Ctx->SrcWrite);

    if (Opt.Move && !Opt.ListOnly)
    {
        /* Fails, as it should, if anything was left behind */
        XRemoveDirectory(Ctx->Src);
    }

Done:
    EntryListFree(&SrcFiles);
    EntryListFree(&SrcDirs);
    EntryListFree(&DstFiles);
    EntryListFree(&DstDirs);
}

/* ---------------------------------------------------------------------- */
/* Command line                                                           */
/* ---------------------------------------------------------------------- */

static BOOL
ParseNumber(PCWSTR s, ULONGLONG *Value)
{
    PWSTR End;
    if (!s || !*s || !iswdigit(*s))
        return FALSE;
    *Value = _wcstoui64(s, &End, 10);
    return *End == 0;
}

static BOOL
ParseAttributes(PCWSTR s, PCWSTR Allowed, DWORD *Attr, PWSTR Canon, SIZE_T CchCanon)
{
    static const struct { WCHAR Ch; DWORD Attr; } Map[] =
    {
        { L'R', FILE_ATTRIBUTE_READONLY }, { L'A', FILE_ATTRIBUTE_ARCHIVE },
        { L'S', FILE_ATTRIBUTE_SYSTEM }, { L'H', FILE_ATTRIBUTE_HIDDEN },
        { L'C', FILE_ATTRIBUTE_COMPRESSED }, { L'N', FILE_ATTRIBUTE_NOT_CONTENT_INDEXED },
        { L'E', FILE_ATTRIBUTE_ENCRYPTED }, { L'T', FILE_ATTRIBUTE_TEMPORARY },
        { L'O', FILE_ATTRIBUTE_OFFLINE },
    };
    SIZE_T i, n = 0;
    DWORD Result = 0;

    for (; *s; s++)
    {
        WCHAR c = towupper(*s);
        if (!wcschr(Allowed, c))
            return FALSE;
        for (i = 0; i < ARRAYSIZE(Map); i++)
        {
            if (Map[i].Ch == c)
                Result |= Map[i].Attr;
        }
    }
    *Attr = Result;
    for (i = 0; i < ARRAYSIZE(Map) && n + 1 < CchCanon; i++)
    {
        if ((Result & Map[i].Attr) && wcschr(Allowed, Map[i].Ch))
            Canon[n++] = Map[i].Ch;
    }
    Canon[n] = 0;
    return TRUE;
}

static BOOL
ParseCopyFlags(PCWSTR s, BOOL Dir)
{
    PCWSTR Allowed = Dir ? L"DATEX" : L"DATSOUX";
    BOOL D = FALSE, A = FALSE, T = FALSE, S = FALSE, O = FALSE, U = FALSE, X = FALSE, E = FALSE;
    PCWSTR p;

    for (p = s; *p; p++)
    {
        WCHAR c = towupper(*p);
        if (!wcschr(Allowed, c))
            return FALSE;
        switch (c)
        {
            case L'D': D = TRUE; break;
            case L'A': A = TRUE; break;
            case L'T': T = TRUE; break;
            case L'S': S = TRUE; break;
            case L'O': O = TRUE; break;
            case L'U': U = TRUE; break;
            case L'X': X = TRUE; break;
            case L'E': E = TRUE; break;
        }
    }

    if (Dir)
    {
        Opt.DCopyGiven = TRUE;
        Opt.NoDCopy = FALSE;
        Opt.DCopyD = D; Opt.DCopyA = A; Opt.DCopyT = T; Opt.DCopyE = E; Opt.DCopyX = X;
        StringCchPrintfW(Opt.DCopyFlags, ARRAYSIZE(Opt.DCopyFlags), L"%ls%ls%ls%ls%ls",
                         D ? L"D" : L"", A ? L"A" : L"", T ? L"T" : L"", E ? L"E" : L"", X ? L"X" : L"");
    }
    else
    {
        Opt.CopyD = D; Opt.CopyA = A; Opt.CopyT = T; Opt.CopyS = S; Opt.CopyO = O; Opt.CopyU = U; Opt.CopyX = X;
        Opt.CopyAll = Opt.Sec = Opt.NoCopy = FALSE;
        StringCchPrintfW(Opt.CopyFlags, ARRAYSIZE(Opt.CopyFlags), L"%ls%ls%ls%ls%ls%ls%ls",
                         D ? L"D" : L"", A ? L"A" : L"", T ? L"T" : L"", S ? L"S" : L"",
                         O ? L"O" : L"", U ? L"U" : L"", X ? L"X" : L"");
    }
    return TRUE;
}

/* n < 1900: n days ago; otherwise a YYYYMMDD date (local midnight) */
static ULONGLONG
AgeToFileTime(DWORD n)
{
    if (n < 1900)
        return NowFt - (ULONGLONG)n * FT_DAY;
    else
    {
        SYSTEMTIME st;
        FILETIME Local, Utc;
        ZeroMemory(&st, sizeof(st));
        st.wYear = (WORD)(n / 10000);
        st.wMonth = (WORD)((n / 100) % 100);
        st.wDay = (WORD)(n % 100);
        if (!SystemTimeToFileTime(&st, &Local) || !LocalFileTimeToFileTime(&Local, &Utc))
            return 0;
        return Ft64(&Utc);
    }
}

typedef enum { COLLECT_NONE, COLLECT_IF, COLLECT_XF, COLLECT_XD } COLLECT_MODE;

static BOOL ReadJobFile(PCWSTR Name, STRLIST *Args, SIZE_T InsertAt);

/* Returns 0 on success, or the 1-based index of the invalid parameter */
static SIZE_T
ParseArguments(STRLIST *Args, SIZE_T CmdLineCount)
{
    COLLECT_MODE Mode = COLLECT_NONE;
    SIZE_T i;

    for (i = 0; i < Args->Count; i++)
    {
        PCWSTR Arg = Args->Items[i];
        WCHAR Name[32];
        PCWSTR Colon, Value = NULL;
        SIZE_T NameLen;
        ULONGLONG Num;
        SIZE_T BadIndex = (i < CmdLineCount) ? i + 1 : CmdLineCount;

        if (Arg[0] != L'/' && Arg[0] != L'-')
        {
            switch (Mode)
            {
                case COLLECT_IF: StrListAdd(&Opt.Files, Arg); break;
                case COLLECT_XF: StrListAdd(&Opt.XFiles, Arg); break;
                case COLLECT_XD: StrListAdd(&Opt.XDirs, Arg); break;
                default:
                    if (!Opt.Src)
                        Opt.Src = XStrDup(Arg);
                    else if (!Opt.Dst)
                        Opt.Dst = XStrDup(Arg);
                    else
                        StrListAdd(&Opt.Files, Arg);
                    break;
            }
            continue;
        }

        Colon = wcschr(Arg + 1, L':');
        NameLen = Colon ? (SIZE_T)(Colon - (Arg + 1)) : wcslen(Arg + 1);
        if (NameLen == 0 || NameLen >= ARRAYSIZE(Name))
            return BadIndex;
        memcpy(Name, Arg + 1, NameLen * sizeof(WCHAR));
        Name[NameLen] = 0;
        _wcsupr(Name);
        if (Colon)
            Value = Colon + 1;

#define IS(n)       (!wcscmp(Name, L##n))
#define FLAG(n, f)  if (IS(n)) { if (Value) return BadIndex; (f) = TRUE; Mode = COLLECT_NONE; continue; }

        FLAG("S", Opt.Subdirs)
        if (IS("E")) { if (Value) return BadIndex; Opt.Empty = Opt.Subdirs = TRUE; Mode = COLLECT_NONE; continue; }
        FLAG("Z", Opt.Restartable)
        FLAG("B", Opt.Backup)
        FLAG("ZB", Opt.RestartBackup)
        FLAG("J", Opt.Unbuffered)
        FLAG("EFSRAW", Opt.EfsRaw)
        FLAG("SECFIX", Opt.SecFix)
        FLAG("TIMFIX", Opt.TimFix)
        FLAG("PURGE", Opt.Purge)
        FLAG("MOV", Opt.MoveFiles)
        FLAG("MOVE", Opt.Move)
        FLAG("CREATE", Opt.Create)
        FLAG("FAT", Opt.Fat)
        FLAG("256", Opt.No256)
        FLAG("PF", Opt.PerFile)
        FLAG("SL", Opt.SymLinks)
        FLAG("SJ", Opt.SJ)
        FLAG("NOOFFLOAD", Opt.NoOffload)
        FLAG("COMPRESS", Opt.Compress)
        FLAG("SPARSE", Opt.SparseOpt)
        FLAG("NOCLONE", Opt.NoClone)
        FLAG("A", Opt.ArchiveOnly)
        FLAG("M", Opt.ResetArchive)
        FLAG("XC", Opt.ExcludeChanged)
        FLAG("XN", Opt.ExcludeNewer)
        FLAG("XO", Opt.ExcludeOlder)
        FLAG("XX", Opt.ExcludeExtra)
        FLAG("XL", Opt.ExcludeLonely)
        FLAG("IS", Opt.IncludeSame)
        FLAG("IT", Opt.IncludeTweaked)
        FLAG("IM", Opt.IncludeModified)
        FLAG("XJ", Opt.ExcludeJunctions)
        FLAG("XJD", Opt.ExcludeJunctionDirs)
        FLAG("XJF", Opt.ExcludeJunctionFiles)
        FLAG("FFT", Opt.FatTimes)
        FLAG("DST", Opt.DstComp)
        FLAG("REG", Opt.Reg)
        FLAG("TBD", Opt.Tbd)
        FLAG("L", Opt.ListOnly)
        FLAG("X", Opt.ReportAllExtra)
        FLAG("V", Opt.Verbose)
        FLAG("TS", Opt.TimeStamps)
        FLAG("FP", Opt.FullPath)
        FLAG("BYTES", Opt.Bytes)
        FLAG("NS", Opt.NoSize)
        FLAG("NC", Opt.NoClass)
        FLAG("NFL", Opt.NoFileList)
        FLAG("NDL", Opt.NoDirList)
        FLAG("NP", Opt.NoProgress)
        FLAG("ETA", Opt.Eta)
        FLAG("TEE", Opt.Tee)
        FLAG("NJH", Opt.NoJobHeader)
        FLAG("NJS", Opt.NoJobSummary)
        FLAG("UNICODE", Opt.Unicode)
        FLAG("DEBUG", Opt.Debug)
        FLAG("QUIT", Opt.Quit)
        FLAG("NOSD", Opt.NoSD)
        FLAG("NODD", Opt.NoDD)

        if (IS("MIR"))
        {
            if (Value) return BadIndex;
            Opt.Mirror = Opt.Empty = Opt.Subdirs = Opt.Purge = TRUE;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("SEC") || IS("COPYALL") || IS("NOCOPY"))
        {
            if (Value) return BadIndex;
            ParseCopyFlags(IS("SEC") ? L"DATS" : IS("COPYALL") ? L"DATSOU" : L"", FALSE);
            Opt.Sec = IS("SEC");
            Opt.CopyAll = IS("COPYALL");
            Opt.NoCopy = IS("NOCOPY");
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("COPY") || IS("DCOPY"))
        {
            if (!Value || !ParseCopyFlags(Value, IS("DCOPY")))
                return BadIndex;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("NODCOPY"))
        {
            if (Value) return BadIndex;
            Opt.NoDCopy = TRUE;
            Opt.DCopyD = Opt.DCopyA = Opt.DCopyT = Opt.DCopyE = Opt.DCopyX = FALSE;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("LEV") || IS("MAX") || IS("MIN") || IS("MAXAGE") || IS("MINAGE") ||
            IS("MAXLAD") || IS("MINLAD"))
        {
            if (!ParseNumber(Value, &Num))
                return BadIndex;
            if (IS("LEV")) Opt.Level = (DWORD)Num;
            else if (IS("MAX")) { Opt.HasMax = TRUE; Opt.MaxSize = Num; }
            else if (IS("MIN")) { Opt.HasMin = TRUE; Opt.MinSize = Num; }
            else if (IS("MAXAGE")) { Opt.HasMaxAge = TRUE; Opt.MaxAge = (DWORD)Num; }
            else if (IS("MINAGE")) { Opt.HasMinAge = TRUE; Opt.MinAge = (DWORD)Num; }
            else if (IS("MAXLAD")) { Opt.HasMaxLad = TRUE; Opt.MaxLad = (DWORD)Num; }
            else { Opt.HasMinLad = TRUE; Opt.MinLad = (DWORD)Num; }
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("R") || IS("W"))
        {
            /* Like Windows: an empty value means 0, a malformed one is ignored */
            if (!Value)
                return BadIndex;
            if (!*Value)
                Num = 0;
            else if (!ParseNumber(Value, &Num))
                Num = IS("R") ? Opt.Retries : Opt.Wait;
            if (Num > MAXDWORD)
                Num = MAXDWORD;
            if (IS("R")) Opt.Retries = (DWORD)Num; else Opt.Wait = (DWORD)Num;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("MT"))
        {
            Num = 8;
            if (Value && !ParseNumber(Value, &Num))
                return BadIndex;
            if (Num < 1) Num = 1;
            if (Num > 128) Num = 128;
            Opt.Threads = (DWORD)Num;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("A+") || IS("A-") || IS("IA") || IS("XA"))
        {
            BOOL Ok;
            if (!Value)
                return BadIndex;
            if (IS("A+"))
                Ok = ParseAttributes(Value, L"RASHCNET", &Opt.AttrAdd, Opt.AttrAddStr, ARRAYSIZE(Opt.AttrAddStr));
            else if (IS("A-"))
                Ok = ParseAttributes(Value, L"RASHCNET", &Opt.AttrRemove, Opt.AttrRemoveStr, ARRAYSIZE(Opt.AttrRemoveStr));
            else if (IS("IA"))
                Ok = ParseAttributes(Value, L"RASHCNETO", &Opt.AttrInclude, Opt.AttrIncludeStr, ARRAYSIZE(Opt.AttrIncludeStr));
            else
                Ok = ParseAttributes(Value, L"RASHCNETO", &Opt.AttrExclude, Opt.AttrExcludeStr, ARRAYSIZE(Opt.AttrExcludeStr));
            if (!Ok)
                return BadIndex;
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("MON") || IS("MOT") || IS("RH") || IS("IPG") || IS("IOMAXSIZE") || IS("IORATE") || IS("THRESHOLD"))
        {
            if (!Value || !*Value)
                return BadIndex;
            if (IS("MON")) Opt.MonStr = XStrDup(Value);
            else if (IS("MOT")) Opt.MotStr = XStrDup(Value);
            else if (IS("RH")) Opt.RhStr = XStrDup(Value);
            else if (IS("IPG")) Opt.IpgStr = XStrDup(Value);
            else if (IS("IOMAXSIZE")) Opt.IoMaxStr = XStrDup(Value);
            else if (IS("IORATE")) Opt.IoRateStr = XStrDup(Value);
            else Opt.ThresholdStr = XStrDup(Value);
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("LFSM"))
        {
            Opt.Lfsm = TRUE;
            if (Value) Opt.LfsmStr = XStrDup(Value);
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("LOG") || IS("LOG+") || IS("UNILOG") || IS("UNILOG+"))
        {
            if (!Value || !*Value)
                return BadIndex;
            free(Opt.LogFile);
            Opt.LogFile = XStrDup(Value);
            Opt.LogAppend = (Name[NameLen - 1] == L'+');
            Opt.LogUnicode = (Name[0] == L'U');
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("XF") || IS("XD") || IS("IF"))
        {
            if (Value) return BadIndex;
            Mode = IS("XF") ? COLLECT_XF : IS("XD") ? COLLECT_XD : COLLECT_IF;
            continue;
        }
        if (IS("SD") || IS("DD"))
        {
            if (!Value || !*Value)
                return BadIndex;
            if (IS("SD")) { free(Opt.Src); Opt.Src = XStrDup(Value); }
            else { free(Opt.Dst); Opt.Dst = XStrDup(Value); }
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("JOB"))
        {
            if (!Value || !*Value)
                return BadIndex;
            if (!ReadJobFile(Value, Args, i + 1))
            {
                OutRes(IDS_ERR_JOB_FILE, Value);
                return BadIndex;
            }
            Mode = COLLECT_NONE;
            continue;
        }
        if (IS("SAVE"))
        {
            if (!Value || !*Value)
                return BadIndex;
            Opt.SaveJob = XStrDup(Value);
            Mode = COLLECT_NONE;
            continue;
        }

#undef FLAG
#undef IS
        return BadIndex;
    }
    return 0;
}

/* Tokenize a .RCJ job file ("::" starts a comment) and insert its tokens */
static BOOL
ReadJobFile(PCWSTR Name, STRLIST *Args, SIZE_T InsertAt)
{
    PWSTR Path, Text, Line, Next;
    HANDLE h;
    DWORD Size, Read;
    PSTR Raw;
    INT Len;

    if (wcschr(Name, L'.'))
        Path = XStrDup(Name);
    else
        Path = XConcat(Name, L".RCJ", NULL);
    h = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    free(Path);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    Size = GetFileSize(h, NULL);
    if (Size == INVALID_FILE_SIZE || Size > 16 * 1024 * 1024)
    {
        CloseHandle(h);
        return FALSE;
    }
    Raw = XAlloc(Size + 2);
    if (!ReadFile(h, Raw, Size, &Read, NULL))
        Read = 0;
    CloseHandle(h);
    Raw[Read] = Raw[Read + 1] = 0;

    if (Read >= 2 && (BYTE)Raw[0] == 0xFF && (BYTE)Raw[1] == 0xFE)
    {
        Text = XAlloc(Read + 2);
        memcpy(Text, Raw + 2, Read - 2);
    }
    else
    {
        Len = MultiByteToWideChar(CP_ACP, 0, Raw, Read, NULL, 0);
        Text = XAlloc((Len + 1) * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, Raw, Read, Text, Len);
    }
    free(Raw);

    for (Line = Text; Line && *Line; Line = Next)
    {
        PWSTR Comment, End;
        Next = wcschr(Line, L'\n');
        if (Next)
            *Next++ = 0;
        Comment = wcsstr(Line, L"::");
        if (Comment)
            *Comment = 0;
        while (*Line == L' ' || *Line == L'\t')
            Line++;
        End = Line + wcslen(Line);
        while (End > Line && (End[-1] == L' ' || End[-1] == L'\t' || End[-1] == L'\r'))
            *--End = 0;
        if (!*Line)
            continue;
        /* Option lines may carry their value after whitespace; file lines are whole names */
        if (*Line == L'/')
        {
            PWSTR Sp = Line;
            while (*Sp && *Sp != L' ' && *Sp != L'\t')
                Sp++;
            *Sp = 0;
        }
        StrListInsert(Args, InsertAt++, Line);
    }
    free(Text);
    return TRUE;
}

/* Build the canonical "Options :" string */
static PWSTR
BuildOptionString(BOOL Valid)
{
    WCHAR Buf[4096];
    WCHAR Tmp[128];
    Buf[0] = 0;

#define ADD(s)      StringCchCatW(Buf, ARRAYSIZE(Buf), s L" ")
#define ADDIF(c, s) if (c) ADD(s)
#define ADDF(fmt, ...) do { StringCchPrintfW(Tmp, ARRAYSIZE(Tmp), fmt L" ", __VA_ARGS__); StringCchCatW(Buf, ARRAYSIZE(Buf), Tmp); } while (0)

    if (Valid && !Opt.Files.Count)
        ADD(L"*.*");
    ADDIF(Opt.No256, L"/256");
    ADDIF(Opt.Fat, L"/FAT");
    ADDIF(Opt.ReportAllExtra, L"/X");
    ADDIF(Opt.FatTimes, L"/FFT");
    ADDIF(Opt.DstComp, L"/DST");
    ADDIF(Opt.Verbose, L"/V");
    ADDIF(Opt.TimeStamps, L"/TS");
    ADDIF(Opt.FullPath, L"/FP");
    ADDIF(Opt.Bytes, L"/BYTES");
    ADDIF(Opt.NoSize, L"/NS");
    ADDIF(Opt.NoClass, L"/NC");
    ADDIF(Opt.NoDirList, L"/NDL");
    ADDIF(Opt.NoFileList, L"/NFL");
    ADDIF(Opt.NoJobSummary, L"/NJS");
    ADDIF(Opt.ListOnly, L"/L");
    if (Opt.LogFile)
        ADDF(L"/%ls%ls:%ls", Opt.LogUnicode ? L"UNILOG" : L"LOG", Opt.LogAppend ? L"+" : L"", Opt.LogFile);
    ADDIF(Opt.Tee, L"/TEE");
    ADDIF(Opt.Unicode, L"/UNICODE");
    ADDIF(Opt.Debug, L"/DEBUG");
    ADDIF(Opt.Subdirs, L"/S");
    ADDIF(Opt.Empty, L"/E");
    if (Opt.CopyAll)
    {
        ADD(L"/COPYALL");
    }
    else
    {
        if (Opt.NoDCopy)
            ADD(L"/NODCOPY");
        else
            ADDF(L"/DCOPY:%ls", Opt.DCopyFlags);
        ADDF(L"/COPY:%ls", Opt.CopyFlags);
    }
    ADDIF(Opt.SecFix, L"/SECFIX");
    ADDIF(Opt.TimFix, L"/TIMFIX");
    if (Opt.Move)
        ADD(L"/MOVE");
    else
        ADDIF(Opt.MoveFiles, L"/MOV");
    ADDIF(Opt.Purge, L"/PURGE");
    ADDIF(Opt.Mirror, L"/MIR");
    ADDIF(Opt.Create, L"/CREATE");
    ADDIF(Opt.RestartBackup, L"/ZB");
    else ADDIF(Opt.Backup, L"/B");
    else ADDIF(Opt.Restartable, L"/Z");
    ADDIF(Opt.Unbuffered, L"/J");
    ADDIF(Opt.EfsRaw, L"/EFSRAW");
    ADDIF(Opt.NoOffload, L"/NOOFFLOAD");
    ADDIF(Opt.Compress, L"/COMPRESS");
    ADDIF(Opt.SparseOpt, L"/SPARSE");
    ADDIF(Opt.NoClone, L"/NOCLONE");
    ADDIF(Opt.NoProgress, L"/NP");
    ADDIF(Opt.Eta, L"/ETA");
    ADDIF(Opt.IncludeSame, L"/IS");
    ADDIF(Opt.IncludeModified, L"/IM");
    ADDIF(Opt.IncludeTweaked, L"/IT");
    ADDIF(Opt.ExcludeLonely, L"/XL");
    ADDIF(Opt.ExcludeExtra, L"/XX");
    ADDIF(Opt.ExcludeOlder, L"/XO");
    ADDIF(Opt.ExcludeNewer, L"/XN");
    ADDIF(Opt.ExcludeChanged, L"/XC");
    if (Opt.ExcludeJunctions)
        ADD(L"/XJ");
    else
    {
        ADDIF(Opt.ExcludeJunctionDirs, L"/XJD");
        ADDIF(Opt.ExcludeJunctionFiles, L"/XJF");
    }
    if (Opt.ResetArchive)
        ADD(L"/M");
    else
        ADDIF(Opt.ArchiveOnly, L"/A");
    if (Opt.AttrInclude) ADDF(L"/IA:%ls", Opt.AttrIncludeStr);
    if (Opt.AttrExclude) ADDF(L"/XA:%ls", Opt.AttrExcludeStr);
    if (Opt.AttrAdd) ADDF(L"/A+:%ls", Opt.AttrAddStr);
    if (Opt.AttrRemove) ADDF(L"/A-:%ls", Opt.AttrRemoveStr);
    if (Opt.HasMax) ADDF(L"/MAX:%I64u", Opt.MaxSize);
    if (Opt.HasMin) ADDF(L"/MIN:%I64u", Opt.MinSize);
    if (Opt.HasMaxAge) ADDF(L"/MAXAGE:%lu", Opt.MaxAge);
    if (Opt.HasMinAge) ADDF(L"/MINAGE:%lu", Opt.MinAge);
    if (Opt.HasMaxLad) ADDF(L"/MAXLAD:%lu", Opt.MaxLad);
    if (Opt.HasMinLad) ADDF(L"/MINLAD:%lu", Opt.MinLad);
    if (Opt.Level) ADDF(L"/LEV:%lu", Opt.Level);
    ADDIF(Opt.SymLinks, L"/SL");
    ADDIF(Opt.SJ, L"/SJ");
    if (Opt.Threads) ADDF(L"/MT:%lu", Opt.Threads);
    if (Opt.MonStr) ADDF(L"/MON:%ls", Opt.MonStr);
    if (Opt.MotStr) ADDF(L"/MOT:%ls", Opt.MotStr);
    if (Opt.RhStr) ADDF(L"/RH:%ls", Opt.RhStr);
    ADDIF(Opt.PerFile, L"/PF");
    if (Opt.IpgStr) ADDF(L"/IPG:%ls", Opt.IpgStr);
    if (Opt.IoMaxStr) ADDF(L"/IOMAXSIZE:%ls", Opt.IoMaxStr);
    if (Opt.IoRateStr) ADDF(L"/IORATE:%ls", Opt.IoRateStr);
    if (Opt.ThresholdStr) ADDF(L"/THRESHOLD:%ls", Opt.ThresholdStr);
    ADDIF(Opt.Tbd, L"/TBD");
    if (Opt.Lfsm)
    {
        if (Opt.LfsmStr) ADDF(L"/LFSM:%ls", Opt.LfsmStr);
        else ADD(L"/LFSM");
    }
    ADDF(L"/R:%lu", Opt.Retries);
    ADDF(L"/W:%lu", Opt.Wait);

#undef ADD
#undef ADDIF
#undef ADDF
    return XStrDup(Buf);
}

static void
PrintList(UINT LabelId, const STRLIST *List)
{
    SIZE_T i;
    Emit(Res(LabelId));
    for (i = 0; i < List->Count; i++)
        Out(L"%ls\n\t    ", List->Items[i]);
}

static void
PrintHeader(BOOL Valid)
{
    WCHAR Date[256];
    PWSTR Options;
    STRLIST Star = {0};

    Emit(Res(IDS_BANNER));
    FormatLongDate(Date, ARRAYSIZE(Date));
    OutRes(IDS_HDR_STARTED, Date);
    if (Valid)
    {
        OutRes(IDS_HDR_SOURCE, Opt.SrcFull ? Opt.SrcFull : L"");
        OutRes(IDS_HDR_DEST, Opt.DstFull ? Opt.DstFull : L"");
    }
    else
    {
        OutRes(IDS_HDR_SOURCE_ERR, Opt.SrcFull ? Opt.SrcFull : L"");
        OutRes(IDS_HDR_DEST_ERR, Opt.DstFull ? Opt.DstFull : L"");
    }

    if (!Valid)
    {
        Emit(Res(IDS_HDR_FILES));
        Emit(L"\n");
    }
    else
    {
        if (Opt.Files.Count)
            PrintList(IDS_HDR_FILES, &Opt.Files);
        else
        {
            StrListAdd(&Star, L"*.*");
            PrintList(IDS_HDR_FILES, &Star);
            free(Star.Items[0]);
            free(Star.Items);
        }
        if (Opt.XFiles.Count)
            PrintList(IDS_HDR_EXC_FILES, &Opt.XFiles);
        if (Opt.XDirs.Count)
            PrintList(IDS_HDR_EXC_DIRS, &Opt.XDirs);
    }

    Options = BuildOptionString(Valid);
    OutRes(IDS_HDR_OPTIONS, Options);
    free(Options);
}

static void
PrintSummary(ULONGLONG Elapsed)
{
    static const UINT RowIds[] = { IDS_SUM_DIRS, IDS_SUM_FILES, IDS_SUM_BYTES };
    WCHAR Cells[COL_COUNT][32];
    WCHAR T[4][32];
    WCHAR Date[256];
    INT r, c;

    Emit(Res(IDS_SUM_COLUMNS));
    for (r = 0; r < ROW_COUNT; r++)
    {
        for (c = 0; c < COL_COUNT; c++)
        {
            if (r == ROW_BYTES)
                FormatSize(Stats[r][c], Cells[c], ARRAYSIZE(Cells[c]), FALSE);
            else
                StringCchPrintfW(Cells[c], ARRAYSIZE(Cells[c]), L"%I64u", Stats[r][c]);
        }
        OutRes(RowIds[r], Cells[0], Cells[1], Cells[2], Cells[3], Cells[4], Cells[5]);
    }

    FormatDuration(Elapsed, T[0], ARRAYSIZE(T[0]));
    FormatDuration(CopyTicks, T[1], ARRAYSIZE(T[1]));
    FormatDuration(FailTicks, T[2], ARRAYSIZE(T[2]));
    FormatDuration(ExtraTicks, T[3], ARRAYSIZE(T[3]));
    OutRes(IDS_SUM_TIMES, T[0], T[1], T[2], T[3]);

    if (Stats[ROW_BYTES][COL_COPIED] && !Opt.ListOnly)
    {
        ULONGLONG Ms = CopyTicks ? CopyTicks : 1;
        ULONGLONG Bps = Stats[ROW_BYTES][COL_COPIED] * 1000 / Ms;
        ULONGLONG MbMin = Bps * 60 * 1000 / (1024 * 1024);   /* thousandths */
        WCHAR Mb[32];
        StringCchPrintfW(Mb, ARRAYSIZE(Mb), L"%I64u.%03I64u", MbMin / 1000, MbMin % 1000);
        OutRes(IDS_SUM_SPEED_BYTES, Bps);
        OutRes(IDS_SUM_SPEED_MB, Mb);
    }

    FormatLongDate(Date, ARRAYSIZE(Date));
    OutRes(IDS_SUM_ENDED, Date);
}

static void
PrintUsage(void)
{
    UINT Id;
    for (Id = IDS_USAGE; Id <= IDS_USAGE4; Id++)
        Emit(Res(Id));
}

/* Full path with a trailing backslash */
static PWSTR
FullDirectoryPath(PCWSTR Path)
{
    DWORD Len;
    PWSTR Full, Ret;

    Len = GetFullPathNameW(Path, 0, NULL, NULL);
    if (!Len)
        return XConcat(Path, L"\\", NULL);
    Full = XAlloc((Len + 2) * sizeof(WCHAR));
    if (!GetFullPathNameW(Path, Len + 1, Full, NULL))
    {
        free(Full);
        return XConcat(Path, L"\\", NULL);
    }
    Len = (DWORD)wcslen(Full);
    if (Len && Full[Len - 1] != L'\\')
    {
        Full[Len] = L'\\';
        Full[Len + 1] = 0;
    }
    Ret = XStrDup(Full);
    free(Full);
    return Ret;
}

static void
SaveJobFile(void)
{
    PWSTR Path, Options, Tok;
    HANDLE h;
    SIZE_T i;

    if (wcschr(Opt.SaveJob, L'.'))
        Path = XStrDup(Opt.SaveJob);
    else
        Path = XConcat(Opt.SaveJob, L".RCJ", NULL);
    h = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        ReportError(GetLastError(), IDS_CTX_CREATE_DIR, Path);
        free(Path);
        return;
    }

    {
        HANDLE OldLog = hLog;
        BOOL OldConsole = ToConsole, OldUni = Opt.LogUnicode;
        hLog = h;
        ToConsole = FALSE;
        Opt.LogUnicode = FALSE;

        Out(L":: Robocopy Job %ls\n\n", Opt.SaveJob);
        if (Opt.Src && !Opt.NoSD)
            Out(L":: Source Directory :\n\t/SD:%ls\n\n", Opt.SrcFull ? Opt.SrcFull : Opt.Src);
        if (Opt.Dst && !Opt.NoDD)
            Out(L":: Destination Directory :\n\t/DD:%ls\n\n", Opt.DstFull ? Opt.DstFull : Opt.Dst);
        if (Opt.Files.Count)
        {
            Out(L":: Include These Files :\n\t/IF\n");
            for (i = 0; i < Opt.Files.Count; i++)
                Out(L"\t\t%ls\n", Opt.Files.Items[i]);
            Out(L"\n");
        }
        if (Opt.XDirs.Count)
        {
            Out(L":: Exclude These Directories :\n\t/XD\n");
            for (i = 0; i < Opt.XDirs.Count; i++)
                Out(L"\t\t%ls\n", Opt.XDirs.Items[i]);
            Out(L"\n");
        }
        if (Opt.XFiles.Count)
        {
            Out(L":: Exclude These Files :\n\t/XF\n");
            for (i = 0; i < Opt.XFiles.Count; i++)
                Out(L"\t\t%ls\n", Opt.XFiles.Items[i]);
            Out(L"\n");
        }
        Out(L":: Options :\n");
        Options = BuildOptionString(TRUE);
        for (Tok = wcstok(Options, L" "); Tok; Tok = wcstok(NULL, L" "))
        {
            if (Tok[0] == L'/')
                Out(L"\t%ls\n", Tok);
        }
        free(Options);

        hLog = OldLog;
        ToConsole = OldConsole;
        Opt.LogUnicode = OldUni;
    }
    CloseHandle(h);
    OutRes(IDS_SAVED_JOB, Path);
    free(Path);
}

int
wmain(int argc, WCHAR *argv[])
{
    STRLIST Args = {0};
    SIZE_T Bad;
    INT i;
    DWORD Attr, Error;
    ULONGLONG Start;
    DIRCTX Root;
    INT Result;
    FILETIME Now;

    ConInitStdStreams();
    Start = (ULONGLONG)GetTickCount();
    GetSystemTimeAsFileTime(&Now);
    NowFt = Ft64(&Now);

    /* Defaults: /COPY:DAT /DCOPY:DA /R:1000000 /W:30 */
    Opt.CopyD = Opt.CopyA = Opt.CopyT = TRUE;
    StringCchCopyW(Opt.CopyFlags, ARRAYSIZE(Opt.CopyFlags), L"DAT");
    Opt.DCopyD = Opt.DCopyA = TRUE;
    StringCchCopyW(Opt.DCopyFlags, ARRAYSIZE(Opt.DCopyFlags), L"DA");
    Opt.Retries = 1000000;
    Opt.Wait = 30;

    for (i = 1; i < argc; i++)
        StrListAdd(&Args, argv[i]);

    if (argc < 2)
    {
        Emit(Res(IDS_BANNER));
        PrintUsage();
        return RC_FATAL;
    }

    for (i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"/?") || !wcscmp(argv[i], L"-?"))
        {
            Emit(Res(IDS_BANNER));
            PrintUsage();
            return RC_FATAL;
        }
    }

    Bad = ParseArguments(&Args, (SIZE_T)(argc - 1));

    if (Opt.Src)
        Opt.SrcFull = FullDirectoryPath(Opt.Src);
    if (Opt.Dst)
        Opt.DstFull = FullDirectoryPath(Opt.Dst);

    if (Bad)
    {
        PrintHeader(FALSE);
        OutRes(IDS_ERR_INVALID_PARAM, (INT)Bad, Bad <= Args.Count ? Args.Items[Bad - 1] : L"");
        Emit(Res(IDS_SIMPLE_USAGE));
        return RC_FATAL;
    }

    if (Opt.HasMaxAge) Opt.MaxAgeFt = AgeToFileTime(Opt.MaxAge);
    if (Opt.HasMinAge) Opt.MinAgeFt = AgeToFileTime(Opt.MinAge);
    if (Opt.HasMaxLad) Opt.MaxLadFt = AgeToFileTime(Opt.MaxLad);
    if (Opt.HasMinLad) Opt.MinLadFt = AgeToFileTime(Opt.MinLad);

    if (Opt.Unicode && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR)
    {
        DWORD Written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\xFF\xFE", 2, &Written, NULL);
        ConStreamSetMode(StdOut, UTF16Text, 0);
    }

    if (Opt.LogFile)
    {
        hLog = CreateFileW(Opt.LogFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           Opt.LogAppend ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hLog == INVALID_HANDLE_VALUE)
        {
            hLog = NULL;
            OutRes(IDS_ERR_LOG_FILE, Opt.LogFile);
            return RC_FATAL;
        }
        if (Opt.LogAppend)
            SetFilePointer(hLog, 0, NULL, FILE_END);
        if (Opt.LogUnicode && GetFileSize(hLog, NULL) == 0)
        {
            DWORD Written;
            WriteFile(hLog, "\xFF\xFE", 2, &Written, NULL);
        }
        ToConsole = Opt.Tee;
    }

    if (Opt.SaveJob)
        SaveJobFile();

    if (!Opt.Src)
    {
        if (!Opt.NoJobHeader)
            PrintHeader(TRUE);
        OutRes(IDS_ERR_NO_SOURCE);
        Emit(Res(IDS_SIMPLE_USAGE));
        return RC_FATAL;
    }
    if (!Opt.Dst)
    {
        if (!Opt.NoJobHeader)
            PrintHeader(TRUE);
        OutRes(IDS_ERR_NO_DEST);
        Emit(Res(IDS_SIMPLE_USAGE));
        return RC_FATAL;
    }

    if (!Opt.NoJobHeader)
        PrintHeader(TRUE);

    if (Opt.Quit)
        return 0;

    /* The source must be an accessible directory */
    Attr = XGetAttributes(Opt.SrcFull);
    if (Attr == INVALID_FILE_ATTRIBUTES || !(Attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        Error = (Attr == INVALID_FILE_ATTRIBUTES) ? GetLastError() : ERROR_DIRECTORY;
        ReportError(Error, IDS_CTX_ACCESS_SRC, Opt.SrcFull);
        return RC_FATAL;
    }

    ZeroMemory(&Root, sizeof(Root));
    Root.Src = Opt.SrcFull;
    Root.Dst = Opt.DstFull;
    Root.Level = 1;
    Root.SrcAttr = Attr;
    {
        WIN32_FILE_ATTRIBUTE_DATA Data;
        PWSTR p = ApiPath(Opt.SrcFull);
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &Data))
        {
            Root.SrcCreate = Data.ftCreationTime;
            Root.SrcAccess = Data.ftLastAccessTime;
            Root.SrcWrite = Data.ftLastWriteTime;
        }
        free(p);
    }

    Attr = XGetAttributes(Opt.DstFull);
    if (Attr != INVALID_FILE_ATTRIBUTES && !(Attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        ReportError(ERROR_DIRECTORY, IDS_CTX_ACCESS_DST, Opt.DstFull);
        return RC_FATAL;
    }
    Root.DstExisted = Root.DstReady = (Attr != INVALID_FILE_ATTRIBUTES);
    if (!Root.DstExisted && !Opt.ListOnly)
    {
        if (!XCreateDirectoryTree(Opt.DstFull))
        {
            ReportError(GetLastError(), IDS_CTX_CREATE_DST, Opt.DstFull);
            return RC_FATAL;
        }
        if (Opt.DCopyA && !Opt.NoDCopy)
        {
            DWORD a = Root.SrcAttr & (ATTR_SETTABLE & ~FILE_ATTRIBUTE_NORMAL);
            if (a)
                XSetAttributes(Opt.DstFull, a);
        }
        Root.DstReady = TRUE;
    }

    ProcessDirectory(&Root);

    if (Fatal)
        return RC_FATAL;

    if (!Opt.NoJobSummary)
        PrintSummary((DWORD)((DWORD)GetTickCount() - (DWORD)Start));

    Result = 0;
    if (Stats[ROW_FILES][COL_COPIED])
        Result |= RC_COPIED;
    if (Stats[ROW_FILES][COL_EXTRAS] || Stats[ROW_DIRS][COL_EXTRAS])
        Result |= RC_EXTRA;
    if (Stats[ROW_FILES][COL_MISMATCH] || Stats[ROW_DIRS][COL_MISMATCH])
        Result |= RC_MISMATCH;
    if (Stats[ROW_FILES][COL_FAILED] || Stats[ROW_DIRS][COL_FAILED])
        Result |= RC_FAILED;

    if (hLog)
        CloseHandle(hLog);
    return Result;
}

/* EOF */
