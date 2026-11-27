/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NetUserDel
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lmaccess.h>
#include <lmapibuf.h>
#include <lmerr.h>

START_TEST(NetUserDel)
{
    WCHAR szName[] = L"rostest_netuserdel";
    WCHAR szPassword[] = L"Ros!Test#4711";
    USER_INFO_1 UserInfo;
    LPBYTE pBuffer = NULL;
    NET_API_STATUS Status;

    Status = NetUserDel(NULL, L"rostest_no_such_user");
    ok(Status == NERR_UserNotFound, "NetUserDel(missing user) returned %lu\n", Status);

    ZeroMemory(&UserInfo, sizeof(UserInfo));
    UserInfo.usri1_name = szName;
    UserInfo.usri1_password = szPassword;
    UserInfo.usri1_priv = USER_PRIV_USER;
    UserInfo.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;

    Status = NetUserAdd(NULL, 1, (LPBYTE)&UserInfo, NULL);
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("Adding a user needs administrator rights\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd returned %lu\n", Status);
    if (Status != NERR_Success)
        return;

    /* The delete walks the alias memberships of every domain, even one that has no alias members */
    Status = NetUserDel(NULL, szName);
    ok(Status == NERR_Success, "NetUserDel returned %lu\n", Status);

    Status = NetUserGetInfo(NULL, szName, 0, &pBuffer);
    ok(Status == NERR_UserNotFound, "NetUserGetInfo after the delete returned %lu\n", Status);
    if (pBuffer != NULL)
        NetApiBufferFree(pBuffer);

    Status = NetUserDel(NULL, szName);
    ok(Status == NERR_UserNotFound, "second NetUserDel returned %lu\n", Status);
}
