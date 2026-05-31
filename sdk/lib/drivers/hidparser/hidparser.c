/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        lib/drivers/hidparser/hidparser.c
 * PURPOSE:     HID Parser
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#include "parser.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
HidParser_GetCollectionDescription(
    IN PHIDP_REPORT_DESCRIPTOR ReportDesc,
    IN ULONG DescLength,
    IN POOL_TYPE PoolType,
    OUT PHIDP_DEVICE_DESC DeviceDescription)
{
    NTSTATUS ParserStatus;
    ULONG CollectionCount;
    ULONG Index;
    ULONG ReportIDsCount;
    ULONG ReportIDsIndex;
    PVOID ParserContext;

    //
    // zero description up front, so that all failure paths return a clean
    // descriptor to the caller (Windows leaves no garbage on failure)
    //
    ZeroFunction(DeviceDescription, sizeof(HIDP_DEVICE_DESC));

    //
    // first parse the report descriptor
    //
    ParserStatus = HidParser_ParseReportDescriptor(ReportDesc, DescLength, &ParserContext);
    if (ParserStatus != HIDP_STATUS_SUCCESS)
    {
        //
        // failed to parse report descriptor
        //
        DebugFunction("[HIDPARSER] Failed to parse report descriptor with %x\n", ParserStatus);
        return ParserStatus;
    }

    //
    // get collection count
    //
    CollectionCount = HidParser_NumberOfTopCollections(ParserContext);
    if (CollectionCount == 0)
    {
        //
        // no top level collections found
        //
        ASSERT(FALSE);
        return STATUS_NO_DATA_DETECTED;
    }

    //
    // allocate collection
    //
    DeviceDescription->CollectionDesc = (PHIDP_COLLECTION_DESC)AllocFunction(sizeof(HIDP_COLLECTION_DESC) * CollectionCount);
    if (!DeviceDescription->CollectionDesc)
    {
        //
        // no memory
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for(Index = 0; Index < CollectionCount; Index++)
    {
        ULONG InputLength, OutputLength, FeatureLength;

        //
        // set preparsed data length and build the collection context
        //
        DeviceDescription->CollectionDesc[Index].PreparsedDataLength = HidParser_GetContextSize(ParserContext, Index);
        ParserStatus = HidParser_BuildContext(ParserContext, Index, DeviceDescription->CollectionDesc[Index].PreparsedDataLength, (PVOID*)&DeviceDescription->CollectionDesc[Index].PreparsedData);
        if (ParserStatus != HIDP_STATUS_SUCCESS)
        {
            //
            // no memory
            //
            FreeFunction(DeviceDescription->CollectionDesc);
            return ParserStatus;
        }

        //
        // init collection description
        //
        DeviceDescription->CollectionDesc[Index].CollectionNumber = Index + 1;

        //
        // get collection usage page
        //
        ParserStatus = HidParser_GetCollectionUsagePage((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, &DeviceDescription->CollectionDesc[Index].Usage, &DeviceDescription->CollectionDesc[Index].UsagePage);
        if (ParserStatus != HIDP_STATUS_SUCCESS)
        {
            // collection not found
            FreeFunction(DeviceDescription->CollectionDesc);
            return ParserStatus;
        }

        //
        // windows prepends the report id byte to every report that exists,
        // regardless of whether the descriptor explicitly declared one.  A
        // report type that has no report at all stays at length 0.
        //
        InputLength = HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_INPUT);
        OutputLength = HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_OUTPUT);
        FeatureLength = HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_FEATURE);

        InputLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_INPUT) ? 1 : 0);
        OutputLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_OUTPUT) ? 1 : 0);
        FeatureLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_FEATURE) ? 1 : 0);

        if (HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_INPUT) != 0)
            InputLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_INPUT) == FALSE ? 1 : 0);
        if (HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_OUTPUT) != 0)
            OutputLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_OUTPUT) == FALSE ? 1 : 0);
        if (HidParser_GetReportLength((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_FEATURE) != 0)
            FeatureLength += (HidParser_UsesReportId((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, HID_REPORT_TYPE_FEATURE) == FALSE ? 1 : 0);

        DeviceDescription->CollectionDesc[Index].InputLength = InputLength;
        DeviceDescription->CollectionDesc[Index].OutputLength = OutputLength;
        DeviceDescription->CollectionDesc[Index].FeatureLength = FeatureLength;
    }

    //
    // the report id table spans every distinct report id of every collection,
    // not one entry per collection: a single collection may declare several
    // report ids (e.g. a joystick whose input/output/feature reports are keyed
    // by id).  count them first so the table can be sized exactly.
    //
    ReportIDsCount = 0;
    for(Index = 0; Index < CollectionCount; Index++)
    {
        ReportIDsCount += HidParser_GetReportIds((PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData, NULL, 0);
    }

    //
    // allocate report description
    //
    DeviceDescription->ReportIDs = (PHIDP_REPORT_IDS)AllocFunction(sizeof(HIDP_REPORT_IDS) * ReportIDsCount);
    if (!DeviceDescription->ReportIDs)
    {
        //
        // no memory
        //
        FreeFunction(DeviceDescription->CollectionDesc);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // fill in one report id entry per distinct report id, tagged with the
    // collection it belongs to
    //
    ReportIDsIndex = 0;
    for(Index = 0; Index < CollectionCount; Index++)
    {
        PVOID Context = (PVOID)DeviceDescription->CollectionDesc[Index].PreparsedData;
        UCHAR ReportIds[256];
        ULONG IdCount, IdIndex;
        BOOLEAN UsesReportId;

        IdCount = HidParser_GetReportIds(Context, ReportIds, RTL_NUMBER_OF(ReportIds));

        //
        // a device that declares any report id prepends an id byte to all of
        // its reports
        //
        UsesReportId = (HidParser_UsesReportId(Context, HID_REPORT_TYPE_INPUT) ||
                        HidParser_UsesReportId(Context, HID_REPORT_TYPE_OUTPUT) ||
                        HidParser_UsesReportId(Context, HID_REPORT_TYPE_FEATURE));

        for(IdIndex = 0; IdIndex < IdCount; IdIndex++)
        {
            UCHAR ReportId = ReportIds[IdIndex];
            ULONG InputLength = HidParser_GetReportLengthByReportId(Context, HID_REPORT_TYPE_INPUT, ReportId);
            ULONG OutputLength = HidParser_GetReportLengthByReportId(Context, HID_REPORT_TYPE_OUTPUT, ReportId);
            ULONG FeatureLength = HidParser_GetReportLengthByReportId(Context, HID_REPORT_TYPE_FEATURE, ReportId);

            DeviceDescription->ReportIDs[ReportIDsIndex].CollectionNumber = Index + 1;
            DeviceDescription->ReportIDs[ReportIDsIndex].ReportID = ReportId;
            DeviceDescription->ReportIDs[ReportIDsIndex].InputLength = InputLength + ((InputLength != 0 && UsesReportId) ? 1 : 0);
            DeviceDescription->ReportIDs[ReportIDsIndex].OutputLength = OutputLength + ((OutputLength != 0 && UsesReportId) ? 1 : 0);
            DeviceDescription->ReportIDs[ReportIDsIndex].FeatureLength = FeatureLength + ((FeatureLength != 0 && UsesReportId) ? 1 : 0);
            ReportIDsIndex++;
        }
    }

    //
    // store collection & report count
    //
    DeviceDescription->CollectionDescLength = CollectionCount;
    DeviceDescription->ReportIDsLength = ReportIDsCount;

    //
    // done
    //
    return STATUS_SUCCESS;
}

VOID
NTAPI
HidParser_FreeCollectionDescription(
    IN PHIDP_DEVICE_DESC   DeviceDescription)
{
    ULONG Index;

    //
    // first free all context
    //
    for(Index = 0; Index < DeviceDescription->CollectionDescLength; Index++)
    {
        //
        // free collection context
        //
        FreeFunction(DeviceDescription->CollectionDesc[Index].PreparsedData);
    }

    //
    // now free collection description
    //
    FreeFunction(DeviceDescription->CollectionDesc);

    //
    // free report description
    //
    FreeFunction(DeviceDescription->ReportIDs);
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetCaps(
    IN PVOID CollectionContext,
    OUT PHIDP_CAPS  Capabilities)
{
    //
    // zero capabilities
    //
    ZeroFunction(Capabilities, sizeof(HIDP_CAPS));

    //
    // init capabilities
    //
    HidParser_GetCollectionUsagePage(CollectionContext, &Capabilities->Usage, &Capabilities->UsagePage);
    Capabilities->InputReportByteLength = HidParser_GetReportLength(CollectionContext, HID_REPORT_TYPE_INPUT);
    Capabilities->OutputReportByteLength = HidParser_GetReportLength(CollectionContext, HID_REPORT_TYPE_OUTPUT);
    Capabilities->FeatureReportByteLength = HidParser_GetReportLength(CollectionContext, HID_REPORT_TYPE_FEATURE);

    //
    // always pre-prend report id
    //
    Capabilities->InputReportByteLength = (Capabilities->InputReportByteLength > 0 ? Capabilities->InputReportByteLength + 1 : 0);
    Capabilities->OutputReportByteLength = (Capabilities->OutputReportByteLength > 0 ? Capabilities->OutputReportByteLength + 1 : 0);
    Capabilities->FeatureReportByteLength = (Capabilities->FeatureReportByteLength > 0 ? Capabilities->FeatureReportByteLength + 1 : 0);

    //
    // get number of link collection nodes
    //
    Capabilities->NumberLinkCollectionNodes = HidParser_GetTotalCollectionCount(CollectionContext);

    //
    // get data indices
    //
    Capabilities->NumberInputDataIndices = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_INPUT, TRUE);
    Capabilities->NumberOutputDataIndices = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_OUTPUT, TRUE);
    Capabilities->NumberFeatureDataIndices = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_FEATURE, TRUE);

    //
    // get value caps
    //
    Capabilities->NumberInputValueCaps = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_INPUT, FALSE);
    Capabilities->NumberOutputValueCaps = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_OUTPUT, FALSE);
    Capabilities->NumberFeatureValueCaps = HidParser_GetReportItemTypeCountFromReportType(CollectionContext, HID_REPORT_TYPE_FEATURE, FALSE);


    //
    // get button caps
    //
    Capabilities->NumberInputButtonCaps = HidParser_GetReportItemCountFromReportType(CollectionContext, HID_REPORT_TYPE_INPUT);
    Capabilities->NumberOutputButtonCaps = HidParser_GetReportItemCountFromReportType(CollectionContext, HID_REPORT_TYPE_OUTPUT);
    Capabilities->NumberFeatureButtonCaps = HidParser_GetReportItemCountFromReportType(CollectionContext, HID_REPORT_TYPE_FEATURE);

    //
    // done
    //
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
ULONG
NTAPI
HidParser_MaxUsageListLength(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage  OPTIONAL)
{
    //
    // FIXME test what should be returned when usage page is not defined
    //
    if (UsagePage == HID_USAGE_PAGE_UNDEFINED)
    {
        //
        // implement me
        //
        UNIMPLEMENTED;

        //
        // invalid report
        //
        return 0;
    }

    if (ReportType == HidP_Input)
    {
        //
        // input report
        //
        return HidParser_GetMaxUsageListLengthWithReportAndPage(CollectionContext, HID_REPORT_TYPE_INPUT, UsagePage);
    }
    else if (ReportType == HidP_Output)
    {
        //
        // input report
        //
        return HidParser_GetMaxUsageListLengthWithReportAndPage(CollectionContext, HID_REPORT_TYPE_OUTPUT, UsagePage);
    }
    else if (ReportType == HidP_Feature)
    {
        //
        // input report
        //
        return HidParser_GetMaxUsageListLengthWithReportAndPage(CollectionContext, HID_REPORT_TYPE_FEATURE, UsagePage);
    }
    else
    {
        //
        // invalid report type
        //
        return 0;
    }
}

#undef HidParser_GetButtonCaps

HIDAPI
NTSTATUS
NTAPI
HidParser_GetButtonCaps(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE ReportType,
    IN PHIDP_BUTTON_CAPS ButtonCaps,
    IN PUSHORT ButtonCapsLength)
{
    return HidParser_GetSpecificButtonCaps(CollectionContext, ReportType, HID_USAGE_PAGE_UNDEFINED, HIDP_LINK_COLLECTION_UNSPECIFIED, HID_USAGE_PAGE_UNDEFINED, ButtonCaps, (PULONG)ButtonCapsLength);
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetSpecificValueCaps(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    OUT PHIDP_VALUE_CAPS  ValueCaps,
    IN OUT PUSHORT  ValueCapsLength)
{
    NTSTATUS ParserStatus;

    //
    // FIXME: implement searching in specific collection
    //
    ASSERT(LinkCollection == HIDP_LINK_COLLECTION_UNSPECIFIED);

    if (ReportType == HidP_Input)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetSpecificValueCapsWithReport(CollectionContext, HID_REPORT_TYPE_INPUT, UsagePage, Usage, ValueCaps, ValueCapsLength);
    }
    else if (ReportType == HidP_Output)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetSpecificValueCapsWithReport(CollectionContext, HID_REPORT_TYPE_OUTPUT, UsagePage, Usage, ValueCaps, ValueCapsLength);
    }
    else if (ReportType == HidP_Feature)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetSpecificValueCapsWithReport(CollectionContext, HID_REPORT_TYPE_FEATURE, UsagePage, Usage, ValueCaps, ValueCapsLength);
    }
    else
    {
        //
        // invalid report type
        //
        return HIDP_STATUS_INVALID_REPORT_TYPE;
    }

    //
    // return status
    //
    return ParserStatus;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_UsageListDifference(
  IN PUSAGE  PreviousUsageList,
  IN PUSAGE  CurrentUsageList,
  OUT PUSAGE  BreakUsageList,
  OUT PUSAGE  MakeUsageList,
  IN ULONG  UsageListLength)
{
    ULONG Index, SubIndex, bFound, BreakUsageIndex = 0, MakeUsageIndex = 0;
    USAGE CurrentUsage, Usage;

    if (UsageListLength)
    {
        Index = 0;
        do
        {
            /* get current usage */
            CurrentUsage = PreviousUsageList[Index];

            /* is the end of list reached? */
            if (!CurrentUsage)
                break;

            /* start searching in current usage list */
            SubIndex = 0;
            bFound = FALSE;
            do
            {
                /* get usage of current list */
                Usage = CurrentUsageList[SubIndex];

                /* end of list reached? */
                if (!Usage)
                    break;

                /* check if it matches the current one */
                if (CurrentUsage == Usage)
                {
                    /* it does */
                    bFound = TRUE;
                    break;
                }

                /* move to next usage */
                SubIndex++;
            }while(SubIndex < UsageListLength);

            /* was the usage found ?*/
            if (!bFound)
            {
                /* store it in the break usage list */
                BreakUsageList[BreakUsageIndex] = CurrentUsage;
                BreakUsageIndex++;
            }

            /* move to next usage */
            Index++;

        }while(Index < UsageListLength);

        /* now process the new items */
        Index = 0;
        do
        {
            /* get current usage */
            CurrentUsage = CurrentUsageList[Index];

            /* is the end of list reached? */
            if (!CurrentUsage)
                break;

            /* start searching in current usage list */
            SubIndex = 0;
            bFound = FALSE;
            do
            {
                /* get usage of previous list */
                Usage = PreviousUsageList[SubIndex];

                /* end of list reached? */
                if (!Usage)
                    break;

                /* check if it matches the current one */
                if (CurrentUsage == Usage)
                {
                    /* it does */
                    bFound = TRUE;
                    break;
                }

                /* move to next usage */
                SubIndex++;
            }while(SubIndex < UsageListLength);

            /* was the usage found ?*/
            if (!bFound)
            {
                /* store it in the make usage list */
                MakeUsageList[MakeUsageIndex] = CurrentUsage;
                MakeUsageIndex++;
            }

            /* move to next usage */
            Index++;

        }while(Index < UsageListLength);
    }

    /* does the break list contain empty entries */
    if (BreakUsageIndex < UsageListLength)
    {
        /* zeroize entries */
        RtlZeroMemory(&BreakUsageList[BreakUsageIndex], sizeof(USAGE) * (UsageListLength - BreakUsageIndex));
    }

    /* does the make usage list contain empty entries */
    if (MakeUsageIndex < UsageListLength)
    {
        /* zeroize entries */
        RtlZeroMemory(&MakeUsageList[MakeUsageIndex], sizeof(USAGE) * (UsageListLength - MakeUsageIndex));
    }

    /* done */
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetUsages(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    OUT USAGE  *UsageList,
    IN OUT PULONG UsageLength,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    NTSTATUS ParserStatus;

    //
    // FIXME: implement searching in specific collection
    //
    ASSERT(LinkCollection == HIDP_LINK_COLLECTION_UNSPECIFIED);

    if (ReportType == HidP_Input)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsagesWithReport(CollectionContext, HID_REPORT_TYPE_INPUT, UsagePage, UsageList, UsageLength, Report, ReportLength);
    }
    else if (ReportType == HidP_Output)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsagesWithReport(CollectionContext, HID_REPORT_TYPE_OUTPUT, UsagePage, UsageList, UsageLength, Report, ReportLength);
    }
    else if (ReportType == HidP_Feature)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsagesWithReport(CollectionContext, HID_REPORT_TYPE_FEATURE, UsagePage, UsageList, UsageLength, Report, ReportLength);
    }
    else
    {
        //
        // invalid report type
        //
        return HIDP_STATUS_INVALID_REPORT_TYPE;
    }

    //
    // return status
    //
    return ParserStatus;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetScaledUsageValue(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    OUT PLONG  UsageValue,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    NTSTATUS ParserStatus;

    //
    // FIXME: implement searching in specific collection
    //
    ASSERT(LinkCollection == HIDP_LINK_COLLECTION_UNSPECIFIED);

    if (ReportType == HidP_Input)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetScaledUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_INPUT, UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else if (ReportType == HidP_Output)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetScaledUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_OUTPUT, UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else if (ReportType == HidP_Feature)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetScaledUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_FEATURE,  UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else
    {
        //
        // invalid report type
        //
        return HIDP_STATUS_INVALID_REPORT_TYPE;
    }

    //
    // return status
    //
    return ParserStatus;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_TranslateUsageAndPagesToI8042ScanCodes(
   IN PUSAGE_AND_PAGE  ChangedUsageList,
   IN ULONG  UsageListLength,
   IN HIDP_KEYBOARD_DIRECTION  KeyAction,
   IN OUT PHIDP_KEYBOARD_MODIFIER_STATE  ModifierState,
   IN PHIDP_INSERT_SCANCODES  InsertCodesProcedure,
   IN PVOID  InsertCodesContext)
{
    ULONG Index;
    NTSTATUS Status = HIDP_STATUS_SUCCESS;

    for(Index = 0; Index < UsageListLength; Index++)
    {
        //
        // check current usage
        //
        if (ChangedUsageList[Index].UsagePage == HID_USAGE_PAGE_KEYBOARD)
        {
            //
            // process keyboard usage
            //
            Status = HidParser_TranslateKbdUsage(ChangedUsageList[Index].Usage, KeyAction, ModifierState, InsertCodesProcedure, InsertCodesContext);
        }
        else if (ChangedUsageList[Index].UsagePage == HID_USAGE_PAGE_CONSUMER)
        {
            //
            // process consumer usage
            //
            Status = HidParser_TranslateCustUsage(ChangedUsageList[Index].Usage, KeyAction, ModifierState, InsertCodesProcedure, InsertCodesContext);
        }
        else
        {
            //
            // invalid page / end of usage list page
            //
            return HIDP_STATUS_I8042_TRANS_UNKNOWN;
        }

        //
        // check status
        //
        if (Status != HIDP_STATUS_SUCCESS)
        {
            //
            // failed
            //
            return Status;
        }
    }

    //
    // return status
    //
    return Status;
}


HIDAPI
NTSTATUS
NTAPI
HidParser_GetUsagesEx(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USHORT  LinkCollection,
    OUT PUSAGE_AND_PAGE  ButtonList,
    IN OUT ULONG  *UsageLength,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    return HidParser_GetUsages(CollectionContext, ReportType, HID_USAGE_PAGE_UNDEFINED, LinkCollection, (PUSAGE)ButtonList, UsageLength, Report, ReportLength);
}

HIDAPI
NTSTATUS
NTAPI
HidParser_UsageAndPageListDifference(
   IN PUSAGE_AND_PAGE  PreviousUsageList,
   IN PUSAGE_AND_PAGE  CurrentUsageList,
   OUT PUSAGE_AND_PAGE  BreakUsageList,
   OUT PUSAGE_AND_PAGE  MakeUsageList,
   IN ULONG  UsageListLength)
{
    ULONG Index, SubIndex, BreakUsageListIndex = 0, MakeUsageListIndex = 0, bFound;
    PUSAGE_AND_PAGE CurrentUsage, Usage;

    if (UsageListLength)
    {
        /* process removed usages */
        Index = 0;
        do
        {
            /* get usage from current index */
            CurrentUsage = &PreviousUsageList[Index];

            /* end of list reached? */
            if (CurrentUsage->Usage == 0 && CurrentUsage->UsagePage == 0)
                break;

            /* search in current list */
            SubIndex = 0;
            bFound = FALSE;
            do
            {
                /* get usage */
                Usage = &CurrentUsageList[SubIndex];

                /* end of list reached? */
                if (Usage->Usage == 0 && Usage->UsagePage == 0)
                    break;

                /* does it match */
                if (Usage->Usage == CurrentUsage->Usage && Usage->UsagePage == CurrentUsage->UsagePage)
                {
                    /* found match */
                    bFound = TRUE;
                }

                /* move to next index */
                SubIndex++;

            }while(SubIndex < UsageListLength);

            if (!bFound)
            {
                /* store it in break usage list */
                BreakUsageList[BreakUsageListIndex].Usage = CurrentUsage->Usage;
                BreakUsageList[BreakUsageListIndex].UsagePage = CurrentUsage->UsagePage;
                BreakUsageListIndex++;
            }

            /* move to next index */
            Index++;

        }while(Index < UsageListLength);

        /* process new usages */
        Index = 0;
        do
        {
            /* get usage from current index */
            CurrentUsage = &CurrentUsageList[Index];

            /* end of list reached? */
            if (CurrentUsage->Usage == 0 && CurrentUsage->UsagePage == 0)
                break;

            /* search in current list */
            SubIndex = 0;
            bFound = FALSE;
            do
            {
                /* get usage */
                Usage = &PreviousUsageList[SubIndex];

                /* end of list reached? */
                if (Usage->Usage == 0 && Usage->UsagePage == 0)
                    break;

                /* does it match */
                if (Usage->Usage == CurrentUsage->Usage && Usage->UsagePage == CurrentUsage->UsagePage)
                {
                    /* found match */
                    bFound = TRUE;
                }

                /* move to next index */
                SubIndex++;

            }while(SubIndex < UsageListLength);

            if (!bFound)
            {
                /* store it in break usage list */
                MakeUsageList[MakeUsageListIndex].Usage = CurrentUsage->Usage;
                MakeUsageList[MakeUsageListIndex].UsagePage = CurrentUsage->UsagePage;
                MakeUsageListIndex++;
            }

            /* move to next index */
            Index++;
        }while(Index < UsageListLength);
    }

    /* are there remaining free list entries */
    if (BreakUsageListIndex < UsageListLength)
    {
        /* zero them */
        RtlZeroMemory(&BreakUsageList[BreakUsageListIndex], (UsageListLength - BreakUsageListIndex) * sizeof(USAGE_AND_PAGE));
    }

    /* are there remaining free list entries */
    if (MakeUsageListIndex < UsageListLength)
    {
        /* zero them */
        RtlZeroMemory(&MakeUsageList[MakeUsageListIndex], (UsageListLength - MakeUsageListIndex) * sizeof(USAGE_AND_PAGE));
    }

    /* done */
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetSpecificButtonCaps(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    OUT PHIDP_BUTTON_CAPS  ButtonCaps,
    IN OUT PULONG  ButtonCapsLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}


HIDAPI
NTSTATUS
NTAPI
HidParser_GetData(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    OUT PHIDP_DATA  DataList,
    IN OUT PULONG  DataLength,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetExtendedAttributes(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USHORT  DataIndex,
    OUT PHIDP_EXTENDED_ATTRIBUTES  Attributes,
    IN OUT PULONG  LengthAttributes)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetLinkCollectionNodes(
    IN PVOID CollectionContext,
    OUT PHIDP_LINK_COLLECTION_NODE  LinkCollectionNodes,
    IN OUT PULONG  LinkCollectionNodesLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetUsageValue(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    OUT PULONG  UsageValue,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    NTSTATUS ParserStatus;

    //
    // FIXME: implement searching in specific collection
    //
    ASSERT(LinkCollection == HIDP_LINK_COLLECTION_UNSPECIFIED);

    if (ReportType == HidP_Input)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_INPUT, UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else if (ReportType == HidP_Output)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_OUTPUT, UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else if (ReportType == HidP_Feature)
    {
        //
        // input report
        //
        ParserStatus = HidParser_GetUsageValueWithReport(CollectionContext, HID_REPORT_TYPE_FEATURE,  UsagePage, Usage, UsageValue, Report, ReportLength);
    }
    else
    {
        //
        // invalid report type
        //
        return HIDP_STATUS_INVALID_REPORT_TYPE;
    }

    //
    // return status
    //
    return ParserStatus;
}

NTSTATUS
NTAPI
HidParser_SysPowerEvent(
    IN PVOID CollectionContext,
    IN PCHAR HidPacket,
    IN USHORT HidPacketLength,
    OUT PULONG OutputBuffer)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
HidParser_SysPowerCaps (
    IN PVOID CollectionContext,
    OUT PULONG OutputBuffer)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_GetUsageValueArray(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    OUT PCHAR  UsageValue,
    IN USHORT  UsageValueByteLength,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_UnsetUsages(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN PUSAGE  UsageList,
    IN OUT PULONG  UsageLength,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_TranslateUsagesToI8042ScanCodes(
  IN PUSAGE  ChangedUsageList,
  IN ULONG  UsageListLength,
  IN HIDP_KEYBOARD_DIRECTION  KeyAction,
  IN OUT PHIDP_KEYBOARD_MODIFIER_STATE  ModifierState,
  IN PHIDP_INSERT_SCANCODES  InsertCodesProcedure,
  IN PVOID  InsertCodesContext)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_SetUsages(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN PUSAGE  UsageList,
    IN OUT PULONG  UsageLength,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_SetUsageValueArray(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    IN PCHAR  UsageValue,
    IN USHORT  UsageValueByteLength,
    OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_SetUsageValue(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    IN ULONG  UsageValue,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_SetScaledUsageValue(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    IN LONG  UsageValue,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_SetData(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN PHIDP_DATA  DataList,
    IN OUT PULONG  DataLength,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

HIDAPI
ULONG
NTAPI
HidParser_MaxDataListLength(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return 0;
}

HIDAPI
NTSTATUS
NTAPI
HidParser_InitializeReportForID(
    IN PVOID CollectionContext,
    IN HIDP_REPORT_TYPE  ReportType,
    IN UCHAR  ReportID,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}

#undef HidParser_GetValueCaps

HIDAPI
NTSTATUS
NTAPI
HidParser_GetValueCaps(
    IN PVOID CollectionContext,
    HIDP_REPORT_TYPE ReportType,
    PHIDP_VALUE_CAPS ValueCaps,
    PULONG ValueCapsLength)
{
    UNIMPLEMENTED;
    ASSERT(FALSE);
    return STATUS_NOT_IMPLEMENTED;
}
