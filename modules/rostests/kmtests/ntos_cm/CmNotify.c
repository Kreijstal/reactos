/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for registry change notifications
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>

#define TEST_KEY L"\\Registry\\Machine\\SOFTWARE\\KmtestCmNotify"

static
NTSTATUS
CreateVolatileKey(
    _Out_ PHANDLE Key,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    return ZwCreateKey(Key, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL,
                       REG_OPTION_VOLATILE, NULL);
}

static
NTSTATUS
CreateEventHandle(
    _Out_ PHANDLE Event)
{
    OBJECT_ATTRIBUTES ObjectAttributes;

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwCreateEvent(Event, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
}

/* A change below two watched keys completes both watches */
static
VOID
TestNestedWatchers(VOID)
{
    HANDLE Parent = NULL, Child = NULL, ParentEvent = NULL, ChildEvent = NULL;
    IO_STATUS_BLOCK ParentIosb, ChildIosb;
    UNICODE_STRING ValueName;
    LARGE_INTEGER Timeout;
    ULONG Value = 1;
    NTSTATUS Status;

    Status = CreateVolatileKey(&Parent, NULL, TEST_KEY);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = CreateVolatileKey(&Child, Parent, L"Child");
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = CreateEventHandle(&ParentEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateEventHandle(&ChildEvent);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!ParentEvent || !ChildEvent)
        goto Cleanup;

    /* The parent watches its tree, the child itself; the parent arms first */
    Status = ZwNotifyChangeKey(Parent, ParentEvent, NULL, NULL, &ParentIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, TRUE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);
    Status = ZwNotifyChangeKey(Child, ChildEvent, NULL, NULL, &ChildIosb,
                               REG_NOTIFY_CHANGE_LAST_SET, FALSE, NULL, 0, TRUE);
    ok_eq_hex(Status, STATUS_PENDING);

    RtlInitUnicodeString(&ValueName, L"Value");
    Status = ZwSetValueKey(Child, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);

    Timeout.QuadPart = -10000LL * 1000;
    Status = ZwWaitForSingleObject(ParentEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Parent watch not completed (0x%08lx)\n", Status);
    Status = ZwWaitForSingleObject(ChildEvent, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Child watch not completed (0x%08lx)\n", Status);

Cleanup:
    /* Closing the keys cancels a watch that is still pending */
    if (Child)
    {
        ZwDeleteKey(Child);
        ZwClose(Child);
    }
    if (Parent)
    {
        ZwDeleteKey(Parent);
        ZwClose(Parent);
    }
    if (ChildEvent)
        ZwClose(ChildEvent);
    if (ParentEvent)
        ZwClose(ParentEvent);
}

START_TEST(CmNotify)
{
    TestNestedWatchers();
}
