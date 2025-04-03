#include "wow64win_private.h"

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include "../../../win32ss/include/callback.h"

#define DEFINE_USER32_CALLBACK(id, value, fn) NTSTATUS WINAPI wow64win_Nt ## fn (PVOID, ULONG);
#include "../../../win32ss/include/u32cb.h"  
#undef DEFINE_USER32_CALLBACK 

NTSTATUS WINAPI (*UserCallbacks[])(PVOID Arguments, ULONG ArgumentLength) = 
{
#define DEFINE_USER32_CALLBACK(id, value, fn) wow64win_Nt ## fn,
#include "../../../win32ss/include/u32cb.h"  
#undef DEFINE_USER32_CALLBACK 
};

__declspec(allocate(".text"))
static unsigned char ReadFsDwordImpl[] =
{
    0x64, 0x8B, 0x01, /* mov eax, fs:[rcx] */
    0xC3              /* ret */
};

static ULONG __readfsdword(ULONG x)
{
    typedef ULONG(*__readfsdwordImplType)(ULONG);
    return ((__readfsdwordImplType)ReadFsDwordImpl)(x);
}

static
PULONG
GetKernelCallbackTable32()
{
    return UlongToPtr(((PPEB32)(ULONG_PTR)NtCurrentTeb32()->ProcessEnvironmentBlock)->KernelCallbackTable);
}

/* TODO: move back to wow64.dll */
NTSTATUS 
WINAPI 
Wow64KiUserCallbackDispatcher(ULONG nCallback, 
                              PVOID IN pArgs, 
                              ULONG nArgLen, 
                              PVOID* OUT ppReturn, 
                              PULONG OUT pnRetLen)
{    
    USER_CALLBACK_FRAME frame;
    ULONG Args64[2];
    
    Args64[0] = PtrToUlong(pArgs);
    Args64[1] = nArgLen;
    
    frame.prev_frame = NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA];
    frame.temp_list  = NtCurrentTeb()->TlsSlots[WOW64_TLS_TEMPLIST];
    frame.ret_ptr    = ppReturn;
    frame.ret_len    = pnRetLen;
    frame.temp_list  = NULL;
    
    NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA] = &frame;
    
    if (!setjmp(frame.jmpbuf))
    {
        if (nArgLen > 0)
        {
            DPRINT1("Nontrivial user callback %d with %d args\n", nCallback, nArgLen / 4);
        }
        
        Call32(GetKernelCallbackTable32()[nCallback], 2, Args64);
    }
   
    NtCurrentTeb()->TlsSlots[WOW64_TLS_USERCALLBACKDATA] = frame.prev_frame;
    return frame.status;
}

/* TODO: This is more or less what Wine's wow64_NtUserCallWinProc does, 
         adapt it to ReactOS maybe. */
NTSTATUS
WINAPI
wow64win_NtUser32CallWindowProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    return wow64_NtUserCallWinProc(Arguments, ArgumentLength);
}

NTSTATUS
WINAPI
wow64win_NtUser32CallSendAsyncProcForKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32LoadSysMenuTemplateForKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32SetupDefaultCursors(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallHookProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallEventProcFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallLoadMenuFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallClientThreadSetupFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    ULONG nRetLen = 0;
    PVOID pResult;
    NTSTATUS Status;
    
    Status = Wow64KiUserCallbackDispatcher(NumUser32CallClientThreadSetupFromKernel,
                                           Arguments,
                                           ArgumentLength,
                                           &pResult,
                                           &nRetLen);

    
    NtCurrentPeb32()->GdiSharedHandleTable = PtrToUlong(NtCurrentPeb()->GdiSharedHandleTable);
    NtCurrentPeb32()->GdiDCAttributeList = NtCurrentPeb()->GdiDCAttributeList;
    
    return Status;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallClientLoadLibraryFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallGetCharsetInfo(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallCopyImageFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallSetWndIconsFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32DeliverUserAPC(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallDDEPostFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallDDEGetFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallOBMFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallLPKFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallUMPDFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallImmProcessKeyFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
wow64win_NtUser32CallImmLoadLayoutFromKernel(PVOID Arguments, ULONG ArgumentLength)
{
    //__debugbreak();
    return STATUS_NOT_IMPLEMENTED;
}

ATOM
WINAPI
wow64_NtUserGetClassInfo(UINT* pArgs)
{
    HINSTANCE hInstance = get_ptr(&pArgs);
    UNICODE_STRING32* ClassName32 = get_ptr(&pArgs);
    WNDCLASSEXW32* lpWndClassEx32 = get_ptr(&pArgs);
    ULONG *ppszMenuName32 = get_ptr(&pArgs);
    BOOL bAnsi = get_ulong(&pArgs);
    
    UNICODE_STRING ClassName = { 0 };
    LPWSTR pszMenuName = NULL;
    WNDCLASSEXW wndClassEx = { 0 };
    
    ATOM Atom;

    wndClassEx.cbSize = sizeof(wndClassEx);
    
    Atom = NtUserGetClassInfo(hInstance,
                              unicode_str_32to64( &ClassName, ClassName32 ), 
                              &wndClassEx,
                              &pszMenuName,
                              bAnsi);
    if (Atom == 0)
    {
        return Atom;
    }

    lpWndClassEx32->style = wndClassEx.style;
    lpWndClassEx32->lpfnWndProc = PtrToUlong(wndClassEx.lpfnWndProc);
    lpWndClassEx32->cbClsExtra = wndClassEx.cbClsExtra;
    lpWndClassEx32->cbWndExtra = wndClassEx.cbWndExtra;
    lpWndClassEx32->hInstance = PtrToUlong(wndClassEx.hInstance);
    lpWndClassEx32->hIcon = HandleToUlong(wndClassEx.hIcon);
    lpWndClassEx32->hCursor = HandleToUlong(wndClassEx.hCursor);
    lpWndClassEx32->hbrBackground = HandleToUlong(wndClassEx.hbrBackground);
    lpWndClassEx32->lpszMenuName = PtrToUlong(wndClassEx.lpszMenuName);
    lpWndClassEx32->lpszClassName = PtrToUlong(wndClassEx.lpszClassName);
    lpWndClassEx32->hIconSm = HandleToUlong(wndClassEx.hIconSm);

    if (ppszMenuName32 != NULL)
    {
        *ppszMenuName32 = PtrToUlong(pszMenuName);
    }
    
    return Atom;
}