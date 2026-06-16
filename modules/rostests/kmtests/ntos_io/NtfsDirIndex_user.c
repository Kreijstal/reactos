/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS directory $I30 index, user part
 * PROGRAMMER:      ReactOS Team
 */

#include <kmt_test.h>

START_TEST(NtfsDirIndex)
{
    /* Pure kernel-mode test, hosted by the bundled kmtest_drv. */
    KmtRunKernelTest("NtfsDirIndex");
}
