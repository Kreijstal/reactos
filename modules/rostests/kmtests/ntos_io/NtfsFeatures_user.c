/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - black-box NTFS feature tests, user part
 * PROGRAMMER:      ReactOS Team
 *
 * Every NTFS feature test is a pure kernel-mode test hosted by kmtest_drv;
 * each feature area is its own entry so a crash in one cannot hide the rest.
 */

#include <kmt_test.h>

START_TEST(NtfsCompression)
{
    KmtRunKernelTest("NtfsCompression");
}

START_TEST(NtfsCreateSemantics)
{
    KmtRunKernelTest("NtfsCreateSemantics");
}

START_TEST(NtfsDataPath)
{
    KmtRunKernelTest("NtfsDataPath");
}

START_TEST(NtfsEa)
{
    KmtRunKernelTest("NtfsEa");
}

START_TEST(NtfsFsctl)
{
    KmtRunKernelTest("NtfsFsctl");
}

START_TEST(NtfsInfoClasses)
{
    KmtRunKernelTest("NtfsInfoClasses");
}

START_TEST(NtfsLinks)
{
    KmtRunKernelTest("NtfsLinks");
}

START_TEST(NtfsObjectId)
{
    KmtRunKernelTest("NtfsObjectId");
}

START_TEST(NtfsQuota)
{
    KmtRunKernelTest("NtfsQuota");
}

START_TEST(NtfsReparse)
{
    KmtRunKernelTest("NtfsReparse");
}

START_TEST(NtfsSecurity)
{
    KmtRunKernelTest("NtfsSecurity");
}

START_TEST(NtfsSparse)
{
    KmtRunKernelTest("NtfsSparse");
}

START_TEST(NtfsStreams)
{
    KmtRunKernelTest("NtfsStreams");
}

START_TEST(NtfsUsn)
{
    KmtRunKernelTest("NtfsUsn");
}

START_TEST(NtfsVolumeControl)
{
    KmtRunKernelTest("NtfsVolumeControl");
}

START_TEST(NtfsVolumeDirty)
{
    KmtRunKernelTest("NtfsVolumeDirty");
}
