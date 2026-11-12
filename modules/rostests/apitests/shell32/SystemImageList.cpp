/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the system image list mapping and SFGAO_HASSUBFOLDER
 */

#include "shelltest.h"
#include <shellutils.h>
#include <undocshell.h>
#include <strsafe.h>

static WCHAR s_szBase[MAX_PATH];

static void MakePath(PWSTR pszOut, PCWSTR pszRel)
{
    StringCchCopyW(pszOut, MAX_PATH, s_szBase);
    PathAppendW(pszOut, pszRel);
}

static void CreateDir(PCWSTR pszRel, DWORD dwAttrs = 0)
{
    WCHAR szPath[MAX_PATH];
    MakePath(szPath, pszRel);
    ok(CreateDirectoryW(szPath, NULL), "CreateDirectoryW(%ls) failed: %lu\n", szPath, GetLastError());
    if (dwAttrs)
        SetFileAttributesW(szPath, dwAttrs);
}

static void CreateFileEmpty(PCWSTR pszRel)
{
    WCHAR szPath[MAX_PATH];
    MakePath(szPath, pszRel);
    HANDLE hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%ls) failed: %lu\n", szPath, GetLastError());
    CloseHandle(hFile);
}

static HRESULT BindToBase(IShellFolder **ppsf)
{
    CComHeapPtr<ITEMIDLIST> pidl;
    HRESULT hr = SHParseDisplayName(s_szBase, NULL, &pidl, 0, NULL);
    if (FAILED(hr))
        return hr;
    CComPtr<IShellFolder> psfDesktop;
    hr = SHGetDesktopFolder(&psfDesktop);
    if (FAILED(hr))
        return hr;
    return psfDesktop->BindToObject(pidl, NULL, IID_PPV_ARG(IShellFolder, ppsf));
}

static SFGAOF GetChildAttributes(IShellFolder *psf, PCWSTR pszName, SFGAOF sfgao)
{
    CComHeapPtr<ITEMIDLIST> pidl;
    HRESULT hr = psf->ParseDisplayName(NULL, NULL, const_cast<PWSTR>(pszName), NULL, &pidl, NULL);
    ok(hr == S_OK, "ParseDisplayName(%ls) hr = 0x%lx\n", pszName, hr);
    if (FAILED(hr))
        return 0;
    PCUITEMID_CHILD pidlChild = pidl;
    hr = psf->GetAttributesOf(1, &pidlChild, &sfgao);
    ok(hr == S_OK, "GetAttributesOf(%ls) hr = 0x%lx\n", pszName, hr);
    return sfgao;
}

static void TestHasSubFolder(IShellFolder *psf)
{
    CreateDir(L"Empty");
    CreateDir(L"FilesOnly");
    CreateFileEmpty(L"FilesOnly\\a.txt");
    CreateFileEmpty(L"FilesOnly\\b.txt");
    CreateDir(L"WithSub");
    CreateFileEmpty(L"WithSub\\a.txt");
    CreateDir(L"WithSub\\Sub");
    CreateDir(L"SuperHiddenSub");
    CreateDir(L"SuperHiddenSub\\Sub", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

    SFGAOF sfgao = SFGAO_FOLDER | SFGAO_HASSUBFOLDER;
    ok_long(GetChildAttributes(psf, L"Empty", sfgao) & sfgao, SFGAO_FOLDER);
    ok_long(GetChildAttributes(psf, L"FilesOnly", sfgao) & sfgao, SFGAO_FOLDER);
    ok_long(GetChildAttributes(psf, L"WithSub", sfgao) & sfgao, SFGAO_FOLDER | SFGAO_HASSUBFOLDER);

    SHELLSTATE ss = {};
    SHGetSetSettings(&ss, SSF_SHOWSUPERHIDDEN, FALSE);
    if (!ss.fShowSuperHidden) /* A super-hidden subfolder is not shown, so it does not count */
        ok_long(GetChildAttributes(psf, L"SuperHiddenSub", sfgao) & sfgao, SFGAO_FOLDER);
}

static void TestShellIcon(IShellFolder *psf)
{
    /* The file system folder answers the plain-folder icon through IShellIcon, without an extractor */
    CComPtr<IShellIcon> psi;
    HRESULT hr = psf->QueryInterface(IID_PPV_ARG(IShellIcon, &psi));
    ok(hr == S_OK, "QueryInterface(IShellIcon) hr = 0x%lx\n", hr);
    if (FAILED(hr))
        return;

    CComHeapPtr<ITEMIDLIST> pidl;
    hr = psf->ParseDisplayName(NULL, NULL, const_cast<PWSTR>(L"Empty"), NULL, &pidl, NULL);
    ok(hr == S_OK, "ParseDisplayName hr = 0x%lx\n", hr);
    if (FAILED(hr))
        return;

    int iIcon = -1;
    hr = psi->GetIconOf(pidl, 0, &iIcon);
    ok(hr == S_OK, "GetIconOf hr = 0x%lx\n", hr);
    ok_int(iIcon, SHMapPIDLToSystemImageListIndex(psf, pidl, NULL));
    ok(iIcon > 0, "iIcon = %d\n", iIcon);
}

static void TestShortcutIconCached(IShellFolder *psf)
{
    /* A shortcut to a document gets its icon location as "*" + GIL_NOTFILENAME
     * (the index is already a system image list index). Mapping it must not
     * add a new image on every call. */
    WCHAR szTarget[MAX_PATH], szLink[MAX_PATH];
    CreateFileEmpty(L"target.txt");
    MakePath(szTarget, L"target.txt");
    MakePath(szLink, L"doc.lnk");

    CComPtr<IShellLinkW> psl;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARG(IShellLinkW, &psl));
    ok(hr == S_OK, "CoCreateInstance(CLSID_ShellLink) hr = 0x%lx\n", hr);
    if (FAILED(hr))
        return;
    psl->SetPath(szTarget);
    CComPtr<IPersistFile> ppf;
    hr = psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf));
    ok(hr == S_OK, "QueryInterface(IPersistFile) hr = 0x%lx\n", hr);
    if (FAILED(hr))
        return;
    hr = ppf->Save(szLink, TRUE);
    ok(hr == S_OK, "Save hr = 0x%lx\n", hr);

    CComHeapPtr<ITEMIDLIST> pidl;
    hr = psf->ParseDisplayName(NULL, NULL, const_cast<PWSTR>(L"doc.lnk"), NULL, &pidl, NULL);
    ok(hr == S_OK, "ParseDisplayName hr = 0x%lx\n", hr);
    if (FAILED(hr))
        return;

    int iFirst = SHMapPIDLToSystemImageListIndex(psf, pidl, NULL);
    ok(iFirst > 0, "iFirst = %d\n", iFirst);
    for (int i = 0; i < 3; ++i)
        ok_int(SHMapPIDLToSystemImageListIndex(psf, pidl, NULL), iFirst);
}

static void SetProgIdName(PCWSTR pszName)
{
    RegSetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.shtestext", REG_SZ, L"shtestext_auto_file", 0);
    RegSetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\shtestext_auto_file", REG_SZ, pszName, 0);
}

static void TestTypeNameFollowsAssociations(void)
{
    /* A cached file class must follow association changes announced with SHCNE_ASSOCCHANGED */
    SHFILEINFOW sfi;
    SetProgIdName(L"Shell Test Type A");
    ZeroMemory(&sfi, sizeof(sfi));
    SHGetFileInfoW(L"x.shtestext", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
    ok(!wcscmp(sfi.szTypeName, L"Shell Test Type A"), "szTypeName = '%ls'\n", sfi.szTypeName);

    SetProgIdName(L"Shell Test Type B");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);
    ZeroMemory(&sfi, sizeof(sfi));
    SHGetFileInfoW(L"x.shtestext", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES);
    ok(!wcscmp(sfi.szTypeName, L"Shell Test Type B"), "szTypeName = '%ls'\n", sfi.szTypeName);

    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.shtestext");
    SHDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\shtestext_auto_file");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, NULL, NULL);
}

static void Cleanup(void)
{
    WCHAR szFrom[MAX_PATH + 1] = {};
    StringCchCopyW(szFrom, MAX_PATH, s_szBase);
    SHFILEOPSTRUCTW op = { NULL, FO_DELETE, szFrom, NULL, FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT };
    SHFileOperationW(&op);
}

START_TEST(SystemImageList)
{
    CoInitialize(NULL);

    GetTempPathW(_countof(s_szBase), s_szBase);
    PathAppendW(s_szBase, L"shell32_SystemImageList");
    Cleanup();
    if (!CreateDirectoryW(s_szBase, NULL))
    {
        skip("Cannot create %ls: %lu\n", s_szBase, GetLastError());
        CoUninitialize();
        return;
    }

    {
        CComPtr<IShellFolder> psf;
        HRESULT hr = BindToBase(&psf);
        ok(hr == S_OK, "BindToBase hr = 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            TestHasSubFolder(psf);
            TestShellIcon(psf);
            TestShortcutIconCached(psf);
            TestTypeNameFollowsAssociations();
        }
    }

    Cleanup();
    CoUninitialize();
}
