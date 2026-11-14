/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for loading IL-only .NET images
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

/*
 * A minimal IL-only "any CPU" DLL: a PE32 header for i386, a COR20 header
 * with COMIMAGE_FLAGS_ILONLY and an entry point that is an int3. A 64-bit
 * process loads it like Windows does: the header is converted to PE32+ and
 * the entry point is never called.
 */
#include <pshpack1.h>
typedef struct _TEST_IMAGE
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS32 Nt;
    IMAGE_SECTION_HEADER Section;
} TEST_IMAGE;
#include <poppack.h>

#define TEST_FILE_ALIGNMENT     0x200
#define TEST_SECTION_ALIGNMENT  0x1000
#define TEST_ENTRY_OFFSET       0x100

static
BOOL
WriteILOnlyImage(
    _In_ PCWSTR FileName,
    _In_ DWORD CorFlags)
{
    UCHAR File[2 * TEST_FILE_ALIGNMENT];
    TEST_IMAGE *Image = (TEST_IMAGE *)File;
    IMAGE_COR20_HEADER *Cor;
    HANDLE Handle;
    DWORD Written;
    BOOL Ret;

    ZeroMemory(File, sizeof(File));

    Image->Dos.e_magic = IMAGE_DOS_SIGNATURE;
    Image->Dos.e_lfanew = FIELD_OFFSET(TEST_IMAGE, Nt);

    Image->Nt.Signature = IMAGE_NT_SIGNATURE;
    Image->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    Image->Nt.FileHeader.NumberOfSections = 1;
    Image->Nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    Image->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
                                           IMAGE_FILE_32BIT_MACHINE |
                                           IMAGE_FILE_DLL;

    Image->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Image->Nt.OptionalHeader.MajorLinkerVersion = 8;
    Image->Nt.OptionalHeader.SizeOfCode = TEST_FILE_ALIGNMENT;
    Image->Nt.OptionalHeader.AddressOfEntryPoint = TEST_SECTION_ALIGNMENT + TEST_ENTRY_OFFSET;
    Image->Nt.OptionalHeader.BaseOfCode = TEST_SECTION_ALIGNMENT;
    Image->Nt.OptionalHeader.ImageBase = 0x10000000;
    Image->Nt.OptionalHeader.SectionAlignment = TEST_SECTION_ALIGNMENT;
    Image->Nt.OptionalHeader.FileAlignment = TEST_FILE_ALIGNMENT;
    Image->Nt.OptionalHeader.MajorOperatingSystemVersion = 4;
    Image->Nt.OptionalHeader.MajorSubsystemVersion = 4;
    Image->Nt.OptionalHeader.SizeOfImage = 2 * TEST_SECTION_ALIGNMENT;
    Image->Nt.OptionalHeader.SizeOfHeaders = TEST_FILE_ALIGNMENT;
    Image->Nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Image->Nt.OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                                                  IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
                                                  IMAGE_DLLCHARACTERISTICS_NO_SEH;
    Image->Nt.OptionalHeader.SizeOfStackReserve = 0x100000;
    Image->Nt.OptionalHeader.SizeOfStackCommit = 0x1000;
    Image->Nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
    Image->Nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
    Image->Nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    Image->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress = TEST_SECTION_ALIGNMENT;
    Image->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].Size = sizeof(IMAGE_COR20_HEADER);

    memcpy(Image->Section.Name, ".text", 5);
    Image->Section.Misc.VirtualSize = TEST_FILE_ALIGNMENT;
    Image->Section.VirtualAddress = TEST_SECTION_ALIGNMENT;
    Image->Section.SizeOfRawData = TEST_FILE_ALIGNMENT;
    Image->Section.PointerToRawData = TEST_FILE_ALIGNMENT;
    Image->Section.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    Cor = (IMAGE_COR20_HEADER *)&File[TEST_FILE_ALIGNMENT];
    Cor->cb = sizeof(*Cor);
    Cor->MajorRuntimeVersion = 2;
    Cor->MinorRuntimeVersion = 5;
    Cor->Flags = CorFlags;

    /* The entry point must never run */
    File[TEST_FILE_ALIGNMENT + TEST_ENTRY_OFFSET] = 0xcc;

    Handle = CreateFileW(FileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(Handle, File, sizeof(File), &Written, NULL) && (Written == sizeof(File));
    CloseHandle(Handle);
    return Ret;
}

START_TEST(ILOnlyImage)
{
    WCHAR TempPath[MAX_PATH], FileName[MAX_PATH];
    PIMAGE_NT_HEADERS NtHeaders;
    HMODULE Module;

    GetTempPathW(_countof(TempPath), TempPath);
    GetTempFileNameW(TempPath, L"ilo", 0, FileName);

    /* An "any CPU" image loads in a process of either bitness */
    ok(WriteILOnlyImage(FileName, COMIMAGE_FLAGS_ILONLY), "Cannot write %ls: %lu\n", FileName, GetLastError());
    SetLastError(0xdeadbeef);
    Module = LoadLibraryW(FileName);
    ok(Module != NULL, "LoadLibraryW failed with %lu\n", GetLastError());
    if (Module)
    {
        NtHeaders = RtlImageNtHeader(Module);
        ok(NtHeaders != NULL, "No NT header\n");
#ifdef _WIN64
        /* The header was converted to the native format */
        if (NtHeaders)
        {
            ok_hex(NtHeaders->OptionalHeader.Magic, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
            ok_hex(NtHeaders->FileHeader.SizeOfOptionalHeader, sizeof(IMAGE_OPTIONAL_HEADER64));
            ok_hex(NtHeaders->OptionalHeader.SizeOfImage, 2 * TEST_SECTION_ALIGNMENT);
            ok_hex(NtHeaders->OptionalHeader.NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
            ok_hex(NtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress,
                   TEST_SECTION_ALIGNMENT);
            ok(memcmp(IMAGE_FIRST_SECTION(NtHeaders)->Name, ".text", 5) == 0, "Section table was not moved\n");
        }
#endif
        ok(FreeLibrary(Module), "FreeLibrary failed with %lu\n", GetLastError());
    }

#ifdef _WIN64
    /* An image that requires a 32-bit process does not load */
    ok(WriteILOnlyImage(FileName, COMIMAGE_FLAGS_ILONLY | COMIMAGE_FLAGS_32BITREQUIRED),
       "Cannot write %ls: %lu\n", FileName, GetLastError());
    SetLastError(0xdeadbeef);
    Module = LoadLibraryW(FileName);
    ok(Module == NULL, "LoadLibraryW succeeded\n");
    ok_err(ERROR_BAD_EXE_FORMAT);
    if (Module)
        FreeLibrary(Module);
#endif

    DeleteFileW(FileName);
}
