/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that the non-relocatable system DLLs fit their image slots
 *
 * The loader refuses to relocate kernel32.dll and user32.dll
 * (STATUS_ILLEGAL_DLL_RELOCATION), and ntdll.dll is mapped first in every
 * process. Their preferred image ranges must therefore be disjoint, or no
 * Win32 process (csrss first) can start. kernel32 once outgrew its slot into
 * ntdll's because the 3.3 MB default sort table was linked into it; the table
 * is a system file (sortdefault.nls) that kernel32 maps when it needs it.
 */

#include <kmt_test.h>
#include <winnls.h>

typedef struct _IMAGE_RANGE
{
    PCWSTR Name;
    ULONG_PTR Base;
    ULONG_PTR End;
} IMAGE_RANGE, *PIMAGE_RANGE;

static
BOOLEAN
GetPreferredRange(
    _Inout_ PIMAGE_RANGE Range)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    IMAGE_DOS_HEADER DosHeader;
    IMAGE_NT_HEADERS NtHeaders;
    DWORD Read;
    BOOL Ok;

    GetSystemDirectoryW(Path, RTL_NUMBER_OF(Path));
    StringCchCatW(Path, RTL_NUMBER_OF(Path), L"\\");
    StringCchCatW(Path, RTL_NUMBER_OF(Path), Range->Name);

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Cannot open %ls: %lu\n", Path, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = ReadFile(File, &DosHeader, sizeof(DosHeader), &Read, NULL) &&
         Read == sizeof(DosHeader) &&
         DosHeader.e_magic == IMAGE_DOS_SIGNATURE &&
         SetFilePointer(File, DosHeader.e_lfanew, NULL, FILE_BEGIN) == (DWORD)DosHeader.e_lfanew &&
         ReadFile(File, &NtHeaders, sizeof(NtHeaders), &Read, NULL) &&
         Read == sizeof(NtHeaders) &&
         NtHeaders.Signature == IMAGE_NT_SIGNATURE;
    CloseHandle(File);
    ok(Ok, "Cannot read the PE headers of %ls\n", Path);
    if (!Ok)
        return FALSE;

    Range->Base = (ULONG_PTR)NtHeaders.OptionalHeader.ImageBase;
    Range->End = Range->Base + NtHeaders.OptionalHeader.SizeOfImage;
    return TRUE;
}

START_TEST(SystemDllLayout)
{
    IMAGE_RANGE Ranges[] =
    {
        { L"ntdll.dll" },
        { L"kernel32.dll" },
        { L"user32.dll" },
    };
    ULONG i, j;
    WCHAR Upper[] = L"ABC", Lower[] = L"abc", Other[] = L"abd";
    BYTE Key1[64], Key2[64];
    INT Len1, Len2;

    for (i = 0; i < RTL_NUMBER_OF(Ranges); i++)
    {
        if (!GetPreferredRange(&Ranges[i]))
            return;
        trace("%ls: %p-%p\n", Ranges[i].Name, (PVOID)Ranges[i].Base, (PVOID)Ranges[i].End);
    }

    for (i = 0; i < RTL_NUMBER_OF(Ranges); i++)
    {
        for (j = i + 1; j < RTL_NUMBER_OF(Ranges); j++)
        {
            ok(Ranges[i].End <= Ranges[j].Base || Ranges[j].End <= Ranges[i].Base,
               "%ls (%p-%p) overlaps %ls (%p-%p)\n",
               Ranges[i].Name, (PVOID)Ranges[i].Base, (PVOID)Ranges[i].End,
               Ranges[j].Name, (PVOID)Ranges[j].Base, (PVOID)Ranges[j].End);
        }
    }

    /* Each of them is loaded at its preferred base in this process */
    for (i = 0; i < RTL_NUMBER_OF(Ranges); i++)
    {
        HMODULE Module = LoadLibraryW(Ranges[i].Name);

        ok((ULONG_PTR)Module == Ranges[i].Base, "%ls is loaded at %p, not at %p\n",
           Ranges[i].Name, Module, (PVOID)Ranges[i].Base);
        if (Module)
            FreeLibrary(Module);
    }

    /* The default sort table kernel32 maps from sortdefault.nls */
    ok_eq_int(CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, Upper, -1, Lower, -1), CSTR_EQUAL);
    ok_eq_int(CompareStringW(LOCALE_INVARIANT, 0, Lower, -1, Upper, -1), CSTR_LESS_THAN);
    ok_eq_int(CompareStringW(LOCALE_INVARIANT, 0, Other, -1, Lower, -1), CSTR_GREATER_THAN);

    Len1 = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY | NORM_IGNORECASE, Upper, -1, (PWSTR)Key1, sizeof(Key1));
    Len2 = LCMapStringW(LOCALE_INVARIANT, LCMAP_SORTKEY | NORM_IGNORECASE, Lower, -1, (PWSTR)Key2, sizeof(Key2));
    ok(Len1 > 0, "LCMapStringW failed: %lu\n", GetLastError());
    ok_eq_int(Len2, Len1);
    if (Len1 > 0 && Len1 == Len2)
        ok(!memcmp(Key1, Key2, Len1), "Case-insensitive sort keys differ\n");
}
