/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Modern APIs required by desktop applications
 */

#include <apitest.h>
#include <fileapi.h>

static void test_counter(void)
{
    BOOL (WINAPI *Counter)(PLARGE_INTEGER);
    BOOL (WINAPI *Frequency)(PLARGE_INTEGER);
    VOID (WINAPI *InterruptTime)(PULONGLONG);
    LARGE_INTEGER Before, Value, After, Freq, Reference;
    ULONGLONG Time1 = 0, Time2 = 0;
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    Counter = (void *)GetProcAddress(Ntdll, "RtlQueryPerformanceCounter");
    Frequency = (void *)GetProcAddress(Ntdll, "RtlQueryPerformanceFrequency");
    InterruptTime = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryUnbiasedInterruptTimePrecise");
    if (Counter && Frequency)
    {
        QueryPerformanceCounter(&Before);
        ok(Counter(&Value), "Counter query failed\n");
        QueryPerformanceCounter(&After);
        ok(Before.QuadPart <= Value.QuadPart && Value.QuadPart <= After.QuadPart, "Counter uses a different clock\n");
        ok(Frequency(&Freq), "Frequency query failed\n");
        QueryPerformanceFrequency(&Reference);
        ok(Freq.QuadPart > 0 && Freq.QuadPart == Reference.QuadPart, "Unexpected frequency\n");
    }
    else
        win_skip("RTL performance counter API unavailable\n");
    if (!InterruptTime)
    {
        win_skip("Precise interrupt time API unavailable\n");
        return;
    }
    InterruptTime(&Time1);
    Sleep(20);
    InterruptTime(&Time2);
    ok(Time1 > 0 && Time2 >= Time1, "Interrupt time is not monotonic\n");
}

#ifdef _M_AMD64
static void test_growable_table(void)
{
    DWORD (WINAPI *Add)(PVOID *, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
    VOID (WINAPI *Grow)(PVOID, DWORD);
    VOID (WINAPI *Delete)(PVOID);
    PRUNTIME_FUNCTION (WINAPI *Lookup)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    RUNTIME_FUNCTION Functions[2] = {{0}};
    PVOID Handle = NULL;
    BYTE *Code;
    DWORD Status;
    DWORD64 Base;

    Add = (void *)GetProcAddress(Ntdll, "RtlAddGrowableFunctionTable");
    Grow = (void *)GetProcAddress(Ntdll, "RtlGrowFunctionTable");
    Delete = (void *)GetProcAddress(Ntdll, "RtlDeleteGrowableFunctionTable");
    Lookup = (void *)GetProcAddress(Ntdll, "RtlLookupFunctionEntry");
    ok(Add && Grow && Delete && Lookup, "Missing growable table API\n");
    if (!Add || !Grow || !Delete || !Lookup) return;
    Code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(Code != NULL, "VirtualAlloc failed\n");
    if (!Code) return;
    Functions[0].BeginAddress = 0;
    Functions[0].EndAddress = 16;
    Functions[1].BeginAddress = 16;
    Functions[1].EndAddress = 32;
    Status = Add(&Handle, Functions, 0, 2, (ULONG_PTR)Code, (ULONG_PTR)Code + 4096);
    ok_hex(Status, 0);
    ok(Handle != NULL, "No table handle\n");
    if (!Status && Handle)
    {
        ok(Lookup((ULONG_PTR)Code + 8, &Base, NULL) == NULL, "Zero-entry table is visible\n");
        Grow(Handle, 1);
        ok(Lookup((ULONG_PTR)Code + 8, &Base, NULL) == &Functions[0], "First entry missing after growth\n");
        ok(Base == (ULONG_PTR)Code, "Wrong image base\n");
        ok(Lookup((ULONG_PTR)Code + 24, &Base, NULL) == NULL, "Second entry visible before growth\n");
        Grow(Handle, 2);
        ok(Lookup((ULONG_PTR)Code + 24, &Base, NULL) == &Functions[1], "Second entry missing after growth\n");
        Delete(Handle);
        ok(Lookup((ULONG_PTR)Code + 8, &Base, NULL) == NULL, "Deleted table remains visible\n");
    }
    VirtualFree(Code, 0, MEM_RELEASE);
}
#endif

static void test_create_file2(void)
{
    HANDLE (WINAPI *Create)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
    CREATEFILE2_EXTENDED_PARAMETERS Parameters = {0};
    WCHAR Temp[MAX_PATH], Path[MAX_PATH];
    HANDLE File;
    DWORD Count;
    char Buffer[8] = {0};

    Create = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFile2");
    ok(Create != NULL, "Missing CreateFile2\n");
    if (!Create) return;
    GetTempPathW(ARRAYSIZE(Temp), Temp);
    if (!GetTempFileNameW(Temp, L"ros", 0, Path))
    {
        skip("No writable temp directory: %lu\n", GetLastError());
        return;
    }
    Parameters.dwSize = sizeof(Parameters);
    Parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    File = Create(Path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, &Parameters);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile2 failed: %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        ok(WriteFile(File, "firefox", 7, &Count, NULL) && Count == 7, "Write failed\n");
        SetFilePointer(File, 0, NULL, FILE_BEGIN);
        ok(ReadFile(File, Buffer, 7, &Count, NULL) && Count == 7, "Read failed\n");
        ok(!memcmp(Buffer, "firefox", 7), "Data changed\n");
        CloseHandle(File);
    }
    File = Create(Path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Default parameters failed: %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    Parameters.dwSize = 0;
    File = Create(Path, GENERIC_READ, 0, OPEN_EXISTING, &Parameters);
    ok(File == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER,
       "Invalid structure size accepted: %p, %lu\n", File, GetLastError());
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    DeleteFileW(Path);
}

static void test_cpu_sets(void)
{
    BOOL (WINAPI *Query)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    PSYSTEM_CPU_SET_INFORMATION Info, Current;
    ULONG Length = 0, Offset = 0, Count = 0;
    BOOL Result;

    Query = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    ok(Query != NULL, "Missing CPU-set API\n");
    if (!Query) return;
    Result = Query(NULL, 0, &Length, NULL, 0);
    ok(!Result && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Bad size query: %d, %lu\n", Result, GetLastError());
    ok(Length >= sizeof(*Info), "No CPU records\n");
    if (Length < sizeof(*Info)) return;
    Info = HeapAlloc(GetProcessHeap(), 0, Length);
    ok(Info != NULL, "Allocation failed\n");
    if (!Info) return;
    Result = Query(Info, Length, &Length, NULL, 0);
    ok(Result, "CPU-set query failed: %lu\n", GetLastError());
    if (Result)
    {
        while (Offset < Length)
        {
            Current = (void *)((BYTE *)Info + Offset);
            ok(Current->Size >= sizeof(*Info) && Current->Size <= Length - Offset, "Malformed CPU-set record\n");
            if (Current->Size < sizeof(*Info) || Current->Size > Length - Offset) break;
            ok(Current->Type == CpuSetInformation, "Unexpected CPU record type\n");
            Count++;
            Offset += Current->Size;
        }
        ok(Count > 0 && Offset == Length, "Incomplete CPU-set enumeration\n");
    }
    HeapFree(GetProcessHeap(), 0, Info);
}

static void test_wer(void)
{
    HRESULT (WINAPI *Register)(PCWSTR, PVOID);
    HRESULT (WINAPI *Unregister)(PCWSTR, PVOID);
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HRESULT Result;
    static const WCHAR Name[] = L"reactos-win10-apitest.dll";

    Register = (void *)GetProcAddress(Kernel32, "WerRegisterRuntimeExceptionModule");
    Unregister = (void *)GetProcAddress(Kernel32, "WerUnregisterRuntimeExceptionModule");
    ok(Register && Unregister, "Missing WER API\n");
    if (!Register || !Unregister) return;
    Result = Register(Name, (PVOID)1);
    ok_hex(Result, S_OK);
    if (SUCCEEDED(Result))
    {
        ok_hex(Register(Name, (PVOID)1), S_OK);
        ok_hex(Unregister(Name, (PVOID)1), S_OK);
        ok_hex(Register(Name, (PVOID)1), S_OK);
        ok_hex(Unregister(Name, (PVOID)1), S_OK);
    }
}

static void test_mitigation(void)
{
    BOOL (WINAPI *GetPolicy)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    BOOL (WINAPI *SetPolicy)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    PROCESS_MITIGATION_DEP_POLICY Dep;
    ULONGLONG Mask[2] = {0};
    BOOL Result;
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");

    GetPolicy = (void *)GetProcAddress(Kernel32, "GetProcessMitigationPolicy");
    SetPolicy = (void *)GetProcAddress(Kernel32, "SetProcessMitigationPolicy");
    ok(GetPolicy && SetPolicy, "Missing mitigation API\n");
    if (!GetPolicy || !SetPolicy) return;
    FillMemory(&Dep, sizeof(Dep), 0xcc);
    Result = GetPolicy(GetCurrentProcess(), ProcessDEPPolicy, &Dep, sizeof(Dep));
    ok(Result, "DEP query failed: %lu\n", GetLastError());
    if (Result) ok(!(Dep.Flags & ~3u) && Dep.Permanent <= 1, "Uninitialized DEP result\n");
    Result = GetPolicy(GetCurrentProcess(), ProcessMitigationOptionsMask, Mask, sizeof(Mask));
    ok(Result && (Mask[0] & 1), "DEP absent from supported mitigations\n");
    Result = SetPolicy(MaxProcessMitigationPolicy, &Dep, sizeof(Dep));
    ok(!Result && GetLastError() == ERROR_INVALID_PARAMETER, "Invalid policy accepted\n");
}

START_TEST(core)
{
    test_counter();
#ifdef _M_AMD64
    test_growable_table();
#endif
    test_create_file2();
    test_cpu_sets();
    test_wer();
    test_mitigation();
}
