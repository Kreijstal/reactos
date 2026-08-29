/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     xHCI bulk Normal TRB regression tests
 */

#include <kmt_test.h>
#include <windef.h>
#include <usb.h>
#include <hubbusif.h>
#include <usbdlib.h>
#include <drivers/usbport/usbmport.h>
#include <usb/xhcispec.h>
#include "hardware.h"

/* Take the prototype and XHCI_MAX_BULK_NORMAL_TRBS from the driver's own
 * header.  This used to be a hand-copied declaration, which silently went
 * stale when the builder gained its MaxPacketSize and IsInTransfer
 * parameters: on amd64 the wrong prototype still linked and the test then
 * called the function with garbage in the two new arguments, and on i386 it
 * only surfaced as an unresolved _XHCI_BuildBulkNormalTrbs@20. */
#include "usbxhcip.h"

#define TRB_TYPE(_Trb) (((_Trb).GenericTRB.Word3 >> 10) & 0x3F)
#define TRB_CH(_Trb) (((_Trb).GenericTRB.Word3 & (1 << 4)) != 0)
#define TRB_IOC(_Trb) (((_Trb).GenericTRB.Word3 & (1 << 5)) != 0)

typedef struct _TEST_SG_LIST_16
{
    USBPORT_SCATTER_GATHER_LIST Header;
    USBPORT_SCATTER_GATHER_ELEMENT Extra[14];
} TEST_SG_LIST_16, *PTEST_SG_LIST_16;

static
VOID
SetSgElement(IN PUSBPORT_SCATTER_GATHER_LIST SgList,
             IN ULONG Index,
             IN ULONGLONG PhysicalAddress,
             IN ULONG Length,
             IN ULONG Offset)
{
    SgList->SgElement[Index].SgPhysicalAddress.QuadPart = PhysicalAddress;
    SgList->SgElement[Index].SgTransferLength = Length;
    SgList->SgElement[Index].SgOffset = Offset;
}

static
VOID
CheckNormalTrb(IN PXHCI_TRB Trb,
               IN ULONGLONG PhysicalAddress,
               IN ULONG Length,
               IN BOOLEAN ChainBit,
               IN BOOLEAN IocBit)
{
    ok_eq_uint(Trb->GenericTRB.Word0, (ULONG)(PhysicalAddress & 0xFFFFFFFF));
    ok_eq_uint(Trb->GenericTRB.Word1, (ULONG)(PhysicalAddress >> 32));
    ok_eq_uint(Trb->GenericTRB.Word2, Length);
    ok_eq_uint(TRB_TYPE(*Trb), NORMAL_TRB);
    ok(TRB_CH(*Trb) == ChainBit, "CH bit is %d, expected %d\n", TRB_CH(*Trb), ChainBit);
    ok(TRB_IOC(*Trb) == IocBit, "IOC bit is %d, expected %d\n", TRB_IOC(*Trb), IocBit);
}

START_TEST(XhciBulkTrb)
{
    TEST_SG_LIST_16 TestSgList;
    PXHCI_TRB Trbs;
    ULONG TrbCount;
    ULONG Index;
    MPSTATUS Status;

    Trbs = ExAllocatePoolWithTag(NonPagedPool,
                                 sizeof(XHCI_TRB) * XHCI_MAX_BULK_NORMAL_TRBS,
                                 'tBhX');
    ok(Trbs != NULL, "Failed to allocate TRB test buffer\n");
    if (Trbs == NULL)
        return;

    RtlZeroMemory(&TestSgList, sizeof(TestSgList));
    RtlZeroMemory(Trbs, sizeof(XHCI_TRB) * XHCI_MAX_BULK_NORMAL_TRBS);
    TrbCount = 0xaaaaaaaa;
    Status = XHCI_BuildBulkNormalTrbs(NULL, 0, 0, FALSE, Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS, &TrbCount);
    ok_eq_uint(Status, MP_STATUS_SUCCESS);
    ok_eq_uint(TrbCount, 1);
    CheckNormalTrb(&Trbs[0], 0, 0, FALSE, TRUE);

    RtlZeroMemory(&TestSgList, sizeof(TestSgList));
    TestSgList.Header.SgElementCount = 1;
    SetSgElement(&TestSgList.Header, 0, 0x12345000ULL, 4096, 0);
    Status = XHCI_BuildBulkNormalTrbs(&TestSgList.Header,
                                      4096,
                                      0,
                                      FALSE,
                                      Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS,
                                      &TrbCount);
    ok_eq_uint(Status, MP_STATUS_SUCCESS);
    ok_eq_uint(TrbCount, 1);
    CheckNormalTrb(&Trbs[0], 0x12345000ULL, 4096, FALSE, TRUE);

    RtlZeroMemory(&TestSgList, sizeof(TestSgList));
    TestSgList.Header.SgElementCount = 16;
    for (Index = 0; Index < 16; Index++)
    {
        SetSgElement(&TestSgList.Header,
                     Index,
                     0x100000ULL + (Index * 0x2000),
                     4096,
                     Index * 4096);
    }

    Status = XHCI_BuildBulkNormalTrbs(&TestSgList.Header,
                                      16 * 4096,
                                      0,
                                      FALSE,
                                      Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS,
                                      &TrbCount);
    ok_eq_uint(Status, MP_STATUS_SUCCESS);
    ok_eq_uint(TrbCount, 16);
    for (Index = 0; Index < 16; Index++)
    {
        CheckNormalTrb(&Trbs[Index],
                       0x100000ULL + (Index * 0x2000),
                       4096,
                       Index != 15,
                       Index == 15);
    }

    RtlZeroMemory(&TestSgList, sizeof(TestSgList));
    TestSgList.Header.SgElementCount = 3;
    SetSgElement(&TestSgList.Header, 0, 0x200000ULL, 4096, 0);
    SetSgElement(&TestSgList.Header, 1, 0x202000ULL, 4096, 4096);
    SetSgElement(&TestSgList.Header, 2, 0x204000ULL, 0, 8192);
    Status = XHCI_BuildBulkNormalTrbs(&TestSgList.Header,
                                      8192,
                                      0,
                                      FALSE,
                                      Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS,
                                      &TrbCount);
    ok_eq_uint(Status, MP_STATUS_SUCCESS);
    ok_eq_uint(TrbCount, 2);
    CheckNormalTrb(&Trbs[0], 0x200000ULL, 4096, TRUE, FALSE);
    CheckNormalTrb(&Trbs[1], 0x202000ULL, 4096, FALSE, TRUE);

    RtlZeroMemory(&TestSgList, sizeof(TestSgList));
    TestSgList.Header.SgElementCount = 2;
    SetSgElement(&TestSgList.Header, 0, 0x300000ULL, 4096, 0);
    SetSgElement(&TestSgList.Header, 1, 0x302000ULL, 4096, 4096);
    TrbCount = 0xaaaaaaaa;
    Status = XHCI_BuildBulkNormalTrbs(&TestSgList.Header,
                                      12288,
                                      0,
                                      FALSE,
                                      Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS,
                                      &TrbCount);
    ok(Status != MP_STATUS_SUCCESS, "Length mismatch unexpectedly succeeded\n");
    ok_eq_uint(TrbCount, 0);

    TrbCount = 0xaaaaaaaa;
    Status = XHCI_BuildBulkNormalTrbs(NULL,
                                      4096,
                                      0,
                                      FALSE,
                                      Trbs,
                                      XHCI_MAX_BULK_NORMAL_TRBS,
                                      &TrbCount);
    ok(Status != MP_STATUS_SUCCESS, "Missing SG list unexpectedly succeeded\n");
    ok_eq_uint(TrbCount, 0);

    ExFreePoolWithTag(Trbs, 'tBhX');
}
