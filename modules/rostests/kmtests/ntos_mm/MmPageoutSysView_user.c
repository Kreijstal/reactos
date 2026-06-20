/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Regression test - the page-out trimmer must not free a clean
 *              SHARE_COUNT==0 segment page that a Cc system-space view still maps
 * COPYRIGHT:   Copyright 2026 Kreijstal
 */

#include <kmt_test.h>

#define IOCTL_START_TEST  1
#define IOCTL_FINISH_TEST 2

START_TEST(MmPageoutSysView)
{
    DWORD Ret;

    Ret = KmtLoadAndOpenDriver(L"MmPageoutSysView", FALSE);
    ok_eq_int(Ret, ERROR_SUCCESS);
    if (Ret)
        return;

    Ret = KmtSendUlongToDriver(IOCTL_START_TEST, 0);
    ok(Ret == ERROR_SUCCESS, "KmtSendUlongToDriver(START) failed: %lx\n", Ret);
    Ret = KmtSendUlongToDriver(IOCTL_FINISH_TEST, 0);
    ok(Ret == ERROR_SUCCESS, "KmtSendUlongToDriver(FINISH) failed: %lx\n", Ret);

    KmtCloseDriver();
    KmtUnloadDriver();
}
