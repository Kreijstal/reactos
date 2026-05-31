/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64 fiber entry points
 */

#include <k32.h>

#define NDEBUG
#include <debug.h>

DECLSPEC_NORETURN
VOID
WINAPI
BaseFiberStartup(VOID)
{
    UNIMPLEMENTED;
    DbgBreakPoint();
    ExitThread(ERROR_CALL_NOT_IMPLEMENTED);
}

VOID
WINAPI
SwitchToFiber(IN PVOID Fiber)
{
    UNREFERENCED_PARAMETER(Fiber);
    UNIMPLEMENTED;
    DbgBreakPoint();
}
