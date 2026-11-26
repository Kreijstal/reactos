/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Window property persistence, copying and isolation
 */

#define COBJMACROS
#include <apitest.h>
#include <objbase.h>
#include <propsys.h>

static const PROPERTYKEY AppId = {{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},5};

START_TEST(windowprops)
{
    HRESULT (WINAPI *GetStore)(HWND, REFIID, void **);
    HMODULE Shell = LoadLibraryW(L"shell32.dll");
    HWND A, B;
    IPropertyStore *First = NULL, *Second = NULL, *Other = NULL;
    PROPVARIANT Input, Output;
    WCHAR Text[] = L"ReactOS.ApiTest";
    DWORD Count;
    HRESULT Hr, Init;

    ok(Shell != NULL, "Shell load failed\n");
    if (!Shell) return;
    GetStore = (void *)GetProcAddress(Shell, "SHGetPropertyStoreForWindow");
    if (!GetStore) { win_skip("Window property API unavailable\n"); FreeLibrary(Shell); return; }
    Init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    A = CreateWindowExW(0, L"STATIC", L"Property test A", WS_POPUP, 0, 0, 10, 10,
                         NULL, NULL, GetModuleHandleW(NULL), NULL);
    B = CreateWindowExW(0, L"STATIC", L"Property test B", WS_POPUP, 0, 0, 10, 10,
                         NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(A && B, "Window creation failed: %lu\n", GetLastError());
    if (!A || !B) goto Done;
    Hr = GetStore(A, &IID_IPropertyStore, (void **)&First);
    ok_hex(Hr, S_OK);
    if (FAILED(Hr)) goto Done;
    ZeroMemory(&Input, sizeof(Input));
    Input.vt = VT_LPWSTR;
    Input.pwszVal = Text;
    Hr = IPropertyStore_SetValue(First, &AppId, &Input);
    ok_hex(Hr, S_OK);
    Text[0] = L'X';
    Hr = GetStore(A, &IID_IPropertyStore, (void **)&Second);
    ok_hex(Hr, S_OK);
    if (FAILED(Hr)) goto Done;
    ZeroMemory(&Output, sizeof(Output));
    Hr = IPropertyStore_GetValue(Second, &AppId, &Output);
    ok_hex(Hr, S_OK);
    ok(Output.vt == VT_LPWSTR && !lstrcmpW(Output.pwszVal, L"ReactOS.ApiTest"),
       "Property was not copied or immediately visible\n");
    PropVariantClear(&Output);
    ok_hex(IPropertyStore_Commit(First), S_OK);
    Hr = IPropertyStore_GetCount(Second, &Count);
    ok(Hr == S_OK && Count == 1, "Unexpected property count: %lu\n", Count);
    Hr = GetStore(B, &IID_IPropertyStore, (void **)&Other);
    ok_hex(Hr, S_OK);
    if (SUCCEEDED(Hr))
    {
        ZeroMemory(&Output, sizeof(Output));
        ok_hex(IPropertyStore_GetValue(Other, &AppId, &Output), S_OK);
        ok(Output.vt == VT_EMPTY, "Property leaked between windows\n");
        PropVariantClear(&Output);
    }
    ZeroMemory(&Input, sizeof(Input));
    ok_hex(IPropertyStore_SetValue(First, &AppId, &Input), S_OK);
    ZeroMemory(&Output, sizeof(Output));
    ok_hex(IPropertyStore_GetValue(Second, &AppId, &Output), S_OK);
    ok(Output.vt == VT_EMPTY, "Property clear failed\n");
    PropVariantClear(&Output);
Done:
    if (Other) IPropertyStore_Release(Other);
    if (Second) IPropertyStore_Release(Second);
    if (First) IPropertyStore_Release(First);
    if (B) DestroyWindow(B);
    if (A) DestroyWindow(A);
    if (SUCCEEDED(Init)) CoUninitialize();
    FreeLibrary(Shell);
}
