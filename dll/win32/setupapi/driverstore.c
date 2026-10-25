/*
 * PROJECT:     ReactOS Setup API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A minimal driver store for the packages of OEM INF files
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * Since Vista, SetupCopyOEMInf stages the whole driver package (the INF and
 * the files it installs) in the driver store, and devices get their files
 * from there. Installers count on this: Wintun, for one, deletes the
 * directory it extracted its package to right after SetupCopyOEMInf.
 *
 * A package published as %SystemRoot%\Inf\oemN.inf lives in
 * %SystemRoot%\System32\DriverStore\FileRepository\<inf>_<arch>_oemN, with
 * the files laid out as under the directory of the original INF.
 */

#include "setupapi_private.h"

#if defined(_M_AMD64)
#define DRIVER_STORE_ARCH L"amd64"
#elif defined(_M_ARM64)
#define DRIVER_STORE_ARCH L"arm64"
#elif defined(_M_ARM)
#define DRIVER_STORE_ARCH L"arm"
#else
#define DRIVER_STORE_ARCH L"x86"
#endif

static
BOOL
GetFileRepository(
    _Out_writes_(Chars) PWSTR Path,
    _In_ DWORD Chars)
{
    UINT Length = GetSystemDirectoryW(Path, Chars);

    if (Length == 0 || Length + 32 >= Chars)
        return FALSE;
    wcscat(Path, L"\\DriverStore\\FileRepository");
    return TRUE;
}

/* The base name of an OEM INF, without its directory and its extension */
static
BOOL
GetOemInfBaseName(
    _In_ PCWSTR OemInf,
    _Out_writes_(Chars) PWSTR Name,
    _In_ DWORD Chars)
{
    PCWSTR p = wcsrchr(OemInf, L'\\');
    PCWSTR Dot;

    p = p ? p + 1 : OemInf;
    Dot = wcsrchr(p, L'.');
    if (!Dot || _wcsicmp(Dot, L".inf") || (DWORD)(Dot - p) >= Chars || Dot == p)
        return FALSE;
    memcpy(Name, p, (Dot - p) * sizeof(WCHAR));
    Name[Dot - p] = UNICODE_NULL;
    return TRUE;
}

/* The directory of the package published as OemInf, if there is one */
BOOL
SETUP_GetDriverPackageDirectory(
    _In_ PCWSTR OemInf,
    _Out_writes_(Chars) PWSTR Directory,
    _In_ DWORD Chars)
{
    WCHAR Pattern[MAX_PATH], OemName[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE Find;
    SIZE_T Length;

    if (!GetOemInfBaseName(OemInf, OemName, ARRAYSIZE(OemName)) ||
        !GetFileRepository(Pattern, ARRAYSIZE(Pattern)))
        return FALSE;
    Length = wcslen(Pattern);
    if (Length + 4 + wcslen(OemName) >= ARRAYSIZE(Pattern))
        return FALSE;
    wcscat(Pattern, L"\\*_");
    wcscat(Pattern, OemName);

    Find = FindFirstFileW(Pattern, &FindData);
    if (Find == INVALID_HANDLE_VALUE)
        return FALSE;
    FindClose(Find);
    if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;

    if (Length + 1 + wcslen(FindData.cFileName) >= Chars)
        return FALSE;
    Pattern[Length] = UNICODE_NULL;
    wcscpy(Directory, Pattern);
    wcscat(Directory, L"\\");
    wcscat(Directory, FindData.cFileName);
    return TRUE;
}

/* Make the directories of Path, the last component excluded */
static
VOID
CreateParentDirectories(
    _Inout_ PWSTR Path)
{
    PWSTR p;

    for (p = Path + 3; *p; p++)
    {
        if (*p != L'\\')
            continue;
        *p = UNICODE_NULL;
        CreateDirectoryW(Path, NULL);
        *p = L'\\';
    }
}

static
BOOL
JoinPath(
    _Out_writes_(Chars) PWSTR Path,
    _In_ DWORD Chars,
    _In_ PCWSTR Base,
    _In_opt_ PCWSTR Sub1,
    _In_opt_ PCWSTR Sub2,
    _In_ PCWSTR Name)
{
    int Result = _snwprintf(Path, Chars, L"%s%s%s%s%s\\%s", Base,
                            (Sub1 && *Sub1 && *Sub1 != L'\\') ? L"\\" : L"", Sub1 ? Sub1 : L"",
                            (Sub2 && *Sub2 && *Sub2 != L'\\') ? L"\\" : L"", Sub2 ? Sub2 : L"",
                            Name);
    return Result >= 0 && (DWORD)Result < Chars;
}

/* Copy the files of one SourceDisksFiles section into the package */
static
VOID
StageSourceDisksFiles(
    _In_ HINF hInf,
    _In_ PCWSTR Section,
    _In_ PCWSTR DisksSection,
    _In_ PCWSTR SourceDirectory,
    _In_ PCWSTR PackageDirectory)
{
    WCHAR FileName[MAX_PATH], DiskId[32], SubDir[MAX_PATH], DiskPath[MAX_PATH];
    WCHAR Source[MAX_PATH], Destination[MAX_PATH];
    INFCONTEXT Context, DiskContext;

    if (!SetupFindFirstLineW(hInf, Section, NULL, &Context))
        return;
    do
    {
        if (!SetupGetStringFieldW(&Context, 0, FileName, ARRAYSIZE(FileName), NULL) ||
            !SetupGetStringFieldW(&Context, 1, DiskId, ARRAYSIZE(DiskId), NULL))
            continue;
        if (!SetupGetStringFieldW(&Context, 2, SubDir, ARRAYSIZE(SubDir), NULL))
            SubDir[0] = UNICODE_NULL;

        /* The path of the disk, relative to the INF */
        DiskPath[0] = UNICODE_NULL;
        if (SetupFindFirstLineW(hInf, DisksSection, DiskId, &DiskContext) &&
            !SetupGetStringFieldW(&DiskContext, 4, DiskPath, ARRAYSIZE(DiskPath), NULL))
        {
            DiskPath[0] = UNICODE_NULL;
        }

        if (!JoinPath(Source, ARRAYSIZE(Source), SourceDirectory, DiskPath, SubDir, FileName) ||
            !JoinPath(Destination, ARRAYSIZE(Destination), PackageDirectory, DiskPath, SubDir, FileName))
            continue;
        CreateParentDirectories(Destination);
        if (!CopyFileW(Source, Destination, FALSE))
            WARN("Cannot stage %s: %lu\n", debugstr_w(Source), GetLastError());
    } while (SetupFindNextLine(&Context, &Context));
}

/*
 * Copy the package of SourceInf, published as OemInf, into the driver
 * store: the INF, its catalog and the files of its SourceDisksFiles
 * sections. Files that are missing are left out.
 */
BOOL
SETUP_StageDriverPackage(
    _In_ PCWSTR SourceInf,
    _In_ PCWSTR OemInf)
{
    WCHAR SourceDirectory[MAX_PATH], PackageDirectory[MAX_PATH], Path[MAX_PATH];
    WCHAR OemName[MAX_PATH], InfName[MAX_PATH], Catalog[MAX_PATH];
    PWSTR FilePart;
    HINF hInf;
    DWORD Length;

    TRACE("Staging %s as %s\n", debugstr_w(SourceInf), debugstr_w(OemInf));

    Length = GetFullPathNameW(SourceInf, ARRAYSIZE(SourceDirectory), SourceDirectory, &FilePart);
    if (Length == 0 || Length >= ARRAYSIZE(SourceDirectory) || !FilePart)
        return FALSE;
    wcscpy(InfName, FilePart);
    FilePart[-1] = UNICODE_NULL;

    if (!GetOemInfBaseName(OemInf, OemName, ARRAYSIZE(OemName)) ||
        !GetFileRepository(PackageDirectory, ARRAYSIZE(PackageDirectory)))
        return FALSE;

    /* A package published again under the same name replaces the old one */
    if (SETUP_GetDriverPackageDirectory(OemInf, Path, ARRAYSIZE(Path)))
        SETUP_DeleteDriverPackage(OemInf);

    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s_%s_%s", PackageDirectory, InfName,
                   DRIVER_STORE_ARCH, OemName) < 0)
        return FALSE;
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    wcscpy(PackageDirectory, Path);
    wcscat(Path, L"\\");
    CreateParentDirectories(Path);
    if (!CreateDirectoryW(PackageDirectory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;

    if (!JoinPath(Path, ARRAYSIZE(Path), PackageDirectory, NULL, NULL, InfName) ||
        !CopyFileW(SourceInf, Path, FALSE))
    {
        SETUP_DeleteDriverPackage(OemInf);
        return FALSE;
    }

    hInf = SetupOpenInfFileW(SourceInf, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
        return TRUE;

    StageSourceDisksFiles(hInf, L"SourceDisksFiles", L"SourceDisksNames",
                          SourceDirectory, PackageDirectory);
    StageSourceDisksFiles(hInf, L"SourceDisksFiles." DRIVER_STORE_ARCH, L"SourceDisksNames." DRIVER_STORE_ARCH,
                          SourceDirectory, PackageDirectory);
    StageSourceDisksFiles(hInf, L"SourceDisksFiles." DRIVER_STORE_ARCH, L"SourceDisksNames",
                          SourceDirectory, PackageDirectory);

    /* The catalog that signs the package */
    if (SetupGetLineTextW(NULL, hInf, L"Version", L"CatalogFile", Catalog, ARRAYSIZE(Catalog), NULL) ||
        SetupGetLineTextW(NULL, hInf, L"Version", L"CatalogFile.NT" DRIVER_STORE_ARCH, Catalog,
                          ARRAYSIZE(Catalog), NULL))
    {
        WCHAR Source[MAX_PATH];

        if (JoinPath(Source, ARRAYSIZE(Source), SourceDirectory, NULL, NULL, Catalog) &&
            JoinPath(Path, ARRAYSIZE(Path), PackageDirectory, NULL, NULL, Catalog))
        {
            CopyFileW(Source, Path, FALSE);
        }
    }

    SetupCloseInfFile(hInf);
    return TRUE;
}

static
VOID
DeleteDirectoryTree(
    _In_ PCWSTR Directory)
{
    WCHAR Path[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE Find;

    if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\*", Directory) < 0)
        return;
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    Find = FindFirstFileW(Path, &FindData);
    if (Find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(FindData.cFileName, L".") || !wcscmp(FindData.cFileName, L".."))
                continue;
            if (_snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s", Directory, FindData.cFileName) < 0)
                continue;
            Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteDirectoryTree(Path);
            else
            {
                SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(Path);
            }
        } while (FindNextFileW(Find, &FindData));
        FindClose(Find);
    }
    RemoveDirectoryW(Directory);
}

/* Remove the package published as OemInf from the driver store */
VOID
SETUP_DeleteDriverPackage(
    _In_ PCWSTR OemInf)
{
    WCHAR Directory[MAX_PATH];

    if (SETUP_GetDriverPackageDirectory(OemInf, Directory, ARRAYSIZE(Directory)))
    {
        TRACE("Deleting %s\n", debugstr_w(Directory));
        DeleteDirectoryTree(Directory);
    }
}

/***********************************************************************
 *      SetupGetInfDriverStoreLocationW  (SETUPAPI.@)
 *
 * The INF of the driver store package that a published INF (oemN.inf, by
 * name or by path) comes from.
 */
BOOL
WINAPI
SetupGetInfDriverStoreLocationW(
    _In_ PCWSTR FileName,
    _In_opt_ PSP_ALTPLATFORM_INFO AlternatePlatformInfo,
    _In_opt_ PCWSTR LocaleName,
    _Out_writes_opt_(ReturnBufferSize) PWSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    WCHAR Directory[MAX_PATH], Pattern[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    PWSTR Underscore;
    HANDLE Find;
    DWORD Size;

    TRACE("%s %p %s %p %lu %p\n", debugstr_w(FileName), AlternatePlatformInfo,
          debugstr_w(LocaleName), ReturnBuffer, ReturnBufferSize, RequiredSize);

    if (!FileName || (!ReturnBuffer && ReturnBufferSize))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (AlternatePlatformInfo)
        FIXME("Alternate platforms are not supported\n");

    if (!SETUP_GetDriverPackageDirectory(FileName, Directory, ARRAYSIZE(Directory)))
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    /* <inf>_<arch>_oemN holds <inf> */
    wcscpy(Pattern, wcsrchr(Directory, L'\\') + 1);
    Underscore = wcsrchr(Pattern, L'_');
    if (Underscore)
        *Underscore = UNICODE_NULL;
    Underscore = wcsrchr(Pattern, L'_');
    if (Underscore)
        *Underscore = UNICODE_NULL;
    if (wcslen(Directory) + 1 + wcslen(Pattern) >= ARRAYSIZE(Directory))
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    wcscat(Directory, L"\\");
    wcscat(Directory, Pattern);
    Find = FindFirstFileW(Directory, &FindData);
    if (Find == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    FindClose(Find);

    Size = (DWORD)wcslen(Directory) + 1;
    if (RequiredSize)
        *RequiredSize = Size;
    if (!ReturnBuffer)
        return TRUE;
    if (ReturnBufferSize < Size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    wcscpy(ReturnBuffer, Directory);
    return TRUE;
}
