/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the thread processor and process-wide barrier functions
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_GetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER);
typedef BOOL (WINAPI *FN_SetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER);
typedef BOOL (WINAPI *FN_GetNumaProcessorNodeEx)(PPROCESSOR_NUMBER, PUSHORT);
typedef BOOL (WINAPI *FN_QueryThreadCycleTime)(HANDLE, PULONG64);
typedef VOID (WINAPI *FN_FlushProcessWriteBuffers)(VOID);
typedef HRESULT (WINAPI *FN_WerRegisterRuntimeExceptionModule)(PCWSTR, PVOID);

static
DWORD
WINAPI
IdleThread(PVOID Parameter)
{
    return WaitForSingleObject((HANDLE)Parameter, INFINITE);
}

static
VOID
TestIdealProcessor(
    _In_ FN_GetThreadIdealProcessorEx pGet,
    _In_ FN_SetThreadIdealProcessorEx pSet)
{
    PROCESSOR_NUMBER Ideal, Previous, Current;
    SYSTEM_INFO SystemInfo;
    HANDLE Event, Thread;
    BOOL Ret;

    GetSystemInfo(&SystemInfo);

    /* The current thread reads its ideal processor from its TEB */
    memset(&Current, 0xcc, sizeof(Current));
    Ret = pGet(GetCurrentThread(), &Current);
    ok(Ret, "GetThreadIdealProcessorEx failed with %lu\n", GetLastError());
    ok_int(Current.Group, 0);
    ok(Current.Number < SystemInfo.dwNumberOfProcessors, "Number %u\n", Current.Number);
    ok_int(Current.Reserved, 0);

    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, IdleThread, Event, 0, NULL);
    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!Thread)
        return;

    /* Setting returns the previous ideal processor */
    Ideal.Group = 0;
    Ideal.Number = (BYTE)(SystemInfo.dwNumberOfProcessors - 1);
    Ideal.Reserved = 0;
    memset(&Previous, 0xcc, sizeof(Previous));
    Ret = pSet(Thread, &Ideal, &Previous);
    ok(Ret, "SetThreadIdealProcessorEx failed with %lu\n", GetLastError());
    ok_int(Previous.Group, 0);
    ok(Previous.Number < SystemInfo.dwNumberOfProcessors, "Number %u\n", Previous.Number);
    ok_int(Previous.Reserved, 0);

    memset(&Current, 0xcc, sizeof(Current));
    Ret = pGet(Thread, &Current);
    ok(Ret, "GetThreadIdealProcessorEx failed with %lu\n", GetLastError());
    ok_int(Current.Group, 0);
    ok_int(Current.Number, Ideal.Number);
    ok_int(Current.Reserved, 0);

    /* Setting it again reports the value just set */
    Ideal.Number = 0;
    Ret = pSet(Thread, &Ideal, &Previous);
    ok(Ret, "SetThreadIdealProcessorEx failed with %lu\n", GetLastError());
    ok_int(Previous.Number, (BYTE)(SystemInfo.dwNumberOfProcessors - 1));

    /* Nonexistent groups are rejected */
    Ideal.Group = 1;
    SetLastError(0xdeadbeef);
    Ret = pSet(Thread, &Ideal, NULL);
    ok(!Ret, "SetThreadIdealProcessorEx succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);

    SetEvent(Event);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
    CloseHandle(Event);

    /* SetThreadIdealProcessor returns the previous ideal processor too */
    ok(SetThreadIdealProcessor(GetCurrentThread(), 0) != (DWORD)-1,
       "SetThreadIdealProcessor failed with %lu\n", GetLastError());
    ok_long(SetThreadIdealProcessor(GetCurrentThread(), MAXIMUM_PROCESSORS), 0);
    ok(pGet(GetCurrentThread(), &Current), "GetThreadIdealProcessorEx failed\n");
    ok_int(Current.Number, 0);
}

static
VOID
TestNumaProcessorNode(
    _In_ FN_GetNumaProcessorNodeEx pGetNode)
{
    PROCESSOR_NUMBER Processor = { 0 };
    USHORT Node;
    BOOL Ret;

    Node = 0xcccc;
    Ret = pGetNode(&Processor, &Node);
    ok(Ret, "GetNumaProcessorNodeEx failed with %lu\n", GetLastError());
    ok_int(Node, 0);

    Processor.Number = 64;
    Node = 0xcccc;
    SetLastError(0xdeadbeef);
    Ret = pGetNode(&Processor, &Node);
    ok(!Ret, "GetNumaProcessorNodeEx succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);
    ok_int(Node, 0xffff);

    Processor.Number = 0;
    Processor.Reserved = 1;
    Node = 0xcccc;
    SetLastError(0xdeadbeef);
    Ret = pGetNode(&Processor, &Node);
    ok(!Ret, "GetNumaProcessorNodeEx succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);
    ok_int(Node, 0xffff);
}

static
VOID
TestWerRegistration(
    _In_ FN_WerRegisterRuntimeExceptionModule pRegister)
{
    WCHAR Path[MAX_PATH];
    HRESULT hr;
    ULONG i;

    ok_hex(pRegister(NULL, NULL), E_INVALIDARG);

    /* Registering the same module and context again succeeds */
    hr = pRegister(L"C:\\XState_apitest\\module0.dll", (PVOID)1);
    ok_hex(hr, S_OK);
    hr = pRegister(L"C:\\XState_apitest\\module0.dll", (PVOID)1);
    ok_hex(hr, S_OK);

    /* At most 16 modules */
    for (i = 1; i < 16; i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"C:\\XState_apitest\\module%lu.dll", i);
        hr = pRegister(Path, (PVOID)1);
        ok_hex(hr, S_OK);
    }
    hr = pRegister(L"C:\\XState_apitest\\module16.dll", (PVOID)1);
    ok_hex(hr, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

    /* The duplicate check still wins once the list is full */
    hr = pRegister(L"C:\\XState_apitest\\module0.dll", (PVOID)1);
    ok_hex(hr, S_OK);
}

START_TEST(ThreadProcessor)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_GetThreadIdealProcessorEx pGetThreadIdealProcessorEx;
    FN_SetThreadIdealProcessorEx pSetThreadIdealProcessorEx;
    FN_GetNumaProcessorNodeEx pGetNumaProcessorNodeEx;
    FN_QueryThreadCycleTime pQueryThreadCycleTime;
    FN_FlushProcessWriteBuffers pFlushProcessWriteBuffers;
    FN_WerRegisterRuntimeExceptionModule pWerRegisterRuntimeExceptionModule;
    ULONG64 Cycles1, Cycles2;
    volatile ULONG Spin;

    pGetThreadIdealProcessorEx = (FN_GetThreadIdealProcessorEx)GetProcAddress(Kernel32, "GetThreadIdealProcessorEx");
    pSetThreadIdealProcessorEx = (FN_SetThreadIdealProcessorEx)GetProcAddress(Kernel32, "SetThreadIdealProcessorEx");
    pGetNumaProcessorNodeEx = (FN_GetNumaProcessorNodeEx)GetProcAddress(Kernel32, "GetNumaProcessorNodeEx");
    pQueryThreadCycleTime = (FN_QueryThreadCycleTime)GetProcAddress(Kernel32, "QueryThreadCycleTime");
    pFlushProcessWriteBuffers = (FN_FlushProcessWriteBuffers)GetProcAddress(Kernel32, "FlushProcessWriteBuffers");
    pWerRegisterRuntimeExceptionModule = (FN_WerRegisterRuntimeExceptionModule)
        GetProcAddress(Kernel32, "WerRegisterRuntimeExceptionModule");

    if (pGetThreadIdealProcessorEx && pSetThreadIdealProcessorEx)
        TestIdealProcessor(pGetThreadIdealProcessorEx, pSetThreadIdealProcessorEx);
    else
        ok(GetNTVersion() < _WIN32_WINNT_WIN7, "Get/SetThreadIdealProcessorEx are not exported\n");

    if (pGetNumaProcessorNodeEx)
        TestNumaProcessorNode(pGetNumaProcessorNodeEx);
    else
        ok(GetNTVersion() < _WIN32_WINNT_WIN7, "GetNumaProcessorNodeEx is not exported\n");

    if (pQueryThreadCycleTime)
    {
        ok(pQueryThreadCycleTime(GetCurrentThread(), &Cycles1), "QueryThreadCycleTime failed with %lu\n", GetLastError());
        for (Spin = 0; Spin < 10000000; Spin++);
        ok(pQueryThreadCycleTime(GetCurrentThread(), &Cycles2), "QueryThreadCycleTime failed with %lu\n", GetLastError());
        ok(Cycles2 > Cycles1, "Cycles did not advance: %I64u -> %I64u\n", Cycles1, Cycles2);
        SetLastError(0xdeadbeef);
        ok(!pQueryThreadCycleTime(NULL, &Cycles1), "QueryThreadCycleTime succeeded\n");
        ok_err(ERROR_INVALID_HANDLE);
    }
    else
    {
        ok(GetNTVersion() < _WIN32_WINNT_VISTA, "QueryThreadCycleTime is not exported\n");
    }

    if (pFlushProcessWriteBuffers)
        pFlushProcessWriteBuffers();
    else
        ok(GetNTVersion() < _WIN32_WINNT_VISTA, "FlushProcessWriteBuffers is not exported\n");

    if (pWerRegisterRuntimeExceptionModule)
        TestWerRegistration(pWerRegisterRuntimeExceptionModule);
    else
        ok(GetNTVersion() < _WIN32_WINNT_WIN7, "WerRegisterRuntimeExceptionModule is not exported\n");
}
