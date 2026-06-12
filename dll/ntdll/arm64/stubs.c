/*
 * PROJECT:     ReactOS NTDLL ARM64
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     ARM64 stubs until full ntdll arm64 is implemented
 */

#include <ntdll.h>

void _local_unwind2(void) {}
void _global_unwind2(void) {}
int _except_handler2(void) { return 1; }
int _except_handler3(void) { return 1; }

NTSTATUS
NTAPI
RtlQueueApcWow64Thread(
    _In_ HANDLE ThreadHandle,
    _In_ PKNORMAL_ROUTINE ApcRoutine,
    _In_opt_ PVOID NormalContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    (VOID)ThreadHandle;
    (VOID)ApcRoutine;
    (VOID)NormalContext;
    (VOID)SystemArgument1;
    (VOID)SystemArgument2;
    return STATUS_NOT_IMPLEMENTED;
}
