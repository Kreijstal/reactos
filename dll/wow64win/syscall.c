/*
 * WoW64 syscall wrapping
 *
 * Copyright 2021 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __REACTOS__
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#endif
#include "wow64win_private.h"

#ifdef __REACTOS__
#define SVC_(name,numArgs) extern NTSTATUS WINAPI wow64_Nt ## name( UINT *args );
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_

#define SVC_(name,numArgs) static int Num ## name = __COUNTER__;
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_

static NTSTATUS wow64win_Unimplemented(UINT* pArgs)
{
    DPRINT1("UNIMPLEMENTED SYSCALL\n");
    return STATUS_NOT_IMPLEMENTED;
}
#endif

#ifndef __REACTOS__
static void * const win32_syscalls[] =
#else
static void* win32_syscalls[] =
#endif
{
#ifdef __REACTOS__
#define SVC_(name, argc) wow64win_Unimplemented,
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_
#else
#define SYSCALL_ENTRY(id,name,args) wow64_ ## name,
    ALL_SYSCALLS32
#undef SYSCALL_ENTRY
#endif
};

#ifdef __REACTOS__
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(*x))
#endif

static BYTE arguments[ARRAY_SIZE(win32_syscalls)] =
{
#ifdef __REACTOS__
#define SVC_(name, argc) argc,
#include "../../../win32ss/w32ksvc32.h"
#undef SVC_
#else
#define SYSCALL_ENTRY(id,name,args) args,
    ALL_SYSCALLS32
#undef SYSCALL_ENTRY
#endif
};

#ifdef __REACTOS__
__declspec(dllexport)
#endif
const SYSTEM_SERVICE_TABLE sdwhwin32 =
{
    (ULONG_PTR *)win32_syscalls,
    NULL,
    ARRAY_SIZE(win32_syscalls),
    arguments
}; 

#ifdef __REACTOS__
static BOOL wow64_NtGdiInit(UINT* pArgs)
{
    return NtGdiInit();
}

static VOID InitServiceTable(VOID)
{
#define IMPLEMENT_SERVICE(name) do { win32_syscalls[Num ## name] = (PVOID*)wow64_Nt ## name; } while(0)

    IMPLEMENT_SERVICE(UserProcessConnect);
    IMPLEMENT_SERVICE(UserInitializeClientPfnArrays);
    IMPLEMENT_SERVICE(GdiInit);
    IMPLEMENT_SERVICE(UserCallNoParam);
    IMPLEMENT_SERVICE(UserCallOneParam);
    IMPLEMENT_SERVICE(UserRegisterClassExWOW);
    IMPLEMENT_SERVICE(UserGetClassInfo);
    IMPLEMENT_SERVICE(UserCreateWindowEx);
    IMPLEMENT_SERVICE(UserShowWindow);
    IMPLEMENT_SERVICE(UserGetMessage);

#undef IMPLEMENT_SERVICE
}

typedef struct _WNDMSG32
{
    DWORD maxMsgs;
    ULONG abMsgs;
} WNDMSG32, *PWNDMSG32;

static void 
CopyWndMsg32To64(OUT PWNDMSG pWndMsg64,
                 IN PWNDMSG32 pWndMsg32)
{
    pWndMsg64->maxMsgs = pWndMsg32->maxMsgs;
    pWndMsg64->abMsgs = UlongToPtr(pWndMsg32->abMsgs);
}

typedef struct _SHAREDINFO32
{
    ULONG psi;
    ULONG aheList;
    ULONG pDispInfo;
    ULONG ulSharedDelta;
    WNDMSG32 awmControl[FNID_NUM];
    WNDMSG32 DefWindowMsgs;
    WNDMSG32 DefWindowSpecMsgs;
} SHAREDINFO32, *PSHAREDINFO32;

typedef struct _USERCONNECT32
{
    ULONG ulVersion;
    ULONG ulCurrentVersion;
    DWORD dwDispatchCount;
    SHAREDINFO32 siClient;
} USERCONNECT32, *PUSERCONNECT32;

NTSTATUS WINAPI wow64_NtUserProcessConnect(UINT* pArgs)
{
    NTSTATUS Status;
    USERCONNECT UserConnect64;
        
    HANDLE ProcessHandle = get_handle(&pArgs);    
    PUSERCONNECT32 pUserConnect32 = get_ptr(&pArgs);
    ULONG Size = get_ulong(&pArgs);
    
    RtlZeroMemory(&UserConnect64, sizeof(UserConnect64));
    UserConnect64.ulVersion = pUserConnect32->ulVersion;
    
    if (Size != sizeof(USERCONNECT32) || pUserConnect32 == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }
    
    Status = NtUserProcessConnect(ProcessHandle, &UserConnect64, sizeof(UserConnect64));
    
    if (NT_SUCCESS(Status))
    {
        PSHAREDINFO32 si32;
        PSHAREDINFO si64;
        
        pUserConnect32->ulCurrentVersion = UserConnect64.ulCurrentVersion;
        pUserConnect32->dwDispatchCount = UserConnect64.dwDispatchCount;
        
        si32 = &pUserConnect32->siClient;
        si64 = &UserConnect64.siClient;
        
        si32->psi = PtrToUlong(si64->psi);
        si32->aheList = PtrToUlong(si64->aheList);
        si32->pDispInfo = PtrToUlong(si64->pDispInfo);
        si32->ulSharedDelta = si64->ulSharedDelta;
        
        for (int i = 0; i < FNID_NUM; i++)
        {
            CopyWndMsg32To64(&si64->awmControl[i], &si32->awmControl[i]);
        }
        
        CopyWndMsg32To64(&si64->DefWindowMsgs, &si32->DefWindowMsgs);
        CopyWndMsg32To64(&si64->DefWindowSpecMsgs, &si32->DefWindowSpecMsgs);
    }
    
    return Status;
}

#endif

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
#ifndef __REACTOS__
    LdrDisableThreadCalloutsForDll( inst );
    NtCurrentTeb()->Peb->KernelCallbackTable = user_callbacks;
#else
    NtCurrentPeb()->KernelCallbackTable = UserCallbacks;
    InitServiceTable();
#endif
    return TRUE;
}
