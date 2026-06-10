/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     HidP_* report accessors operating on the Windows-compatible
 *              "HidP KDR" preparsed data blob.
 * COPYRIGHT:   Copyright 2015 Aric Stewart
 *              Adapted for ReactOS from Wine's dlls/hid/hidp.c
 */

#define _HIDPI_
#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntddk.h>
#include <hidpddi.h>

#include "parser.h"
#include "hidparser.h"
#include "hidp.h"

#define NDEBUG
#include <debug.h>

static LONG
HidP_MulDiv(
    IN LONG Number,
    IN LONG Numerator,
    IN LONG Denominator)
{
    LONGLONG Ret;

    if (Denominator == 0)
        return -1;

    Ret = (LONGLONG)Number * Numerator;
    /* round to nearest, half away from zero, like the Win32 MulDiv */
    if ((Ret < 0) ^ (Denominator < 0))
        Ret -= Denominator / 2;
    else
        Ret += Denominator / 2;

    return (LONG)(Ret / Denominator);
}

static NTSTATUS get_value_caps_range( struct hid_preparsed_data *preparsed, HIDP_REPORT_TYPE report_type, ULONG report_len,
                                      const struct hid_value_caps **caps, const struct hid_value_caps **caps_end )
{
    if (!preparsed || (RtlCompareMemory( preparsed->magic, "HidP KDR", 8 ) != 8)) return HIDP_STATUS_INVALID_PREPARSED_DATA;

    switch (report_type)
    {
    case HidP_Input:
        if (report_len && report_len != preparsed->input_report_byte_length)
            return HIDP_STATUS_INVALID_REPORT_LENGTH;
        *caps = HID_INPUT_VALUE_CAPS( preparsed );
        *caps_end = *caps + preparsed->input_caps_count;
        break;
    case HidP_Output:
        if (report_len && report_len != preparsed->output_report_byte_length)
            return HIDP_STATUS_INVALID_REPORT_LENGTH;
        *caps = HID_OUTPUT_VALUE_CAPS( preparsed );
        *caps_end = *caps + preparsed->output_caps_count;
        break;
    case HidP_Feature:
        if (report_len && report_len != preparsed->feature_report_byte_length)
            return HIDP_STATUS_INVALID_REPORT_LENGTH;
        *caps = HID_FEATURE_VALUE_CAPS( preparsed );
        *caps_end = *caps + preparsed->feature_caps_count;
        break;
    default:
        return HIDP_STATUS_INVALID_REPORT_TYPE;
    }

    return HIDP_STATUS_SUCCESS;
}

#define USAGE_MASK  0xffff
#define USAGE_ANY  0x10000

struct caps_filter
{
    BOOLEAN buttons;
    BOOLEAN values;
    BOOLEAN array;
    ULONG   usage_page;
    USHORT  collection;
    ULONG   usage;
    UCHAR   report_id;
};

static BOOLEAN match_value_caps( const struct hid_value_caps *caps, const struct caps_filter *filter )
{
    if (!caps->usage_min && !caps->usage_max) return FALSE;
    if (filter->buttons && !(caps->flags & HID_VALUE_CAPS_IS_BUTTON)) return FALSE;
    if (filter->values && (caps->flags & HID_VALUE_CAPS_IS_BUTTON)) return FALSE;
    if (filter->usage_page != USAGE_ANY && (filter->usage_page & USAGE_MASK) != caps->usage_page) return FALSE;
    if (filter->collection && filter->collection != caps->link_collection) return FALSE;
    if (filter->usage == USAGE_ANY) return TRUE;
    return caps->usage_min <= (filter->usage & USAGE_MASK) && caps->usage_max >= (filter->usage & USAGE_MASK);
}

typedef NTSTATUS (*enum_value_caps_callback)( const struct hid_value_caps *caps, void *user );

static NTSTATUS enum_value_caps( struct hid_preparsed_data *preparsed, HIDP_REPORT_TYPE report_type,
                                 ULONG report_len, const struct caps_filter *filter,
                                 enum_value_caps_callback callback, void *user, USHORT *count )
{
    const struct hid_value_caps *caps, *caps_end;
    BOOLEAN is_range, incompatible = FALSE;
    LONG remaining = *count;
    NTSTATUS status;

    for (status = get_value_caps_range( preparsed, report_type, report_len, &caps, &caps_end );
         status == HIDP_STATUS_SUCCESS && caps != caps_end; caps++)
    {
        is_range = caps->flags & HID_VALUE_CAPS_IS_RANGE;
        if (!match_value_caps( caps, filter )) continue;
        if (filter->report_id && caps->report_id != filter->report_id) incompatible = TRUE;
        else if (filter->array && (is_range || caps->report_count <= 1)) return HIDP_STATUS_NOT_VALUE_ARRAY;
        else if (remaining-- > 0) status = callback( caps, user );
    }

    if (status == HIDP_STATUS_NULL) status = HIDP_STATUS_SUCCESS;
    if (status != HIDP_STATUS_SUCCESS) return status;

    *count -= remaining;
    if (*count == 0) return incompatible ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
    if (remaining < 0) return HIDP_STATUS_BUFFER_TOO_SMALL;
    return HIDP_STATUS_SUCCESS;
}

/* copy count bits from src, starting at (-shift) bit if < 0, to dst starting at (shift) bit if > 0 */
static void copy_bits( unsigned char *dst, const unsigned char *src, int count, int shift )
{
    unsigned char bits, mask;
    size_t src_shift = shift < 0 ? (-shift & 7) : 0;
    size_t dst_shift = shift > 0 ? (shift & 7) : 0;
    if (shift < 0) src += -shift / 8;
    if (shift > 0) dst += shift / 8;

    if (src_shift == 0 && dst_shift == 0)
    {
        RtlCopyMemory( dst, src, count / 8 );
        dst += count / 8;
        src += count / 8;
        count &= 7;
    }

    if (!count) return;

    bits = *dst << (8 - dst_shift);
    count += dst_shift;

    while (count > 8)
    {
        *dst = bits >> (8 - dst_shift);
        bits = *(unsigned short *)src++ >> src_shift;
        *dst++ |= bits << dst_shift;
        count -= 8;
    }

    bits >>= (8 - dst_shift);
    if (count <= 8 - src_shift) bits |= (*src >> src_shift) << dst_shift;
    else bits |= (*(unsigned short *)src >> src_shift) << dst_shift;

    mask = (1 << count) - 1;
    *dst = (bits & mask) | (*dst & ~mask);
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetCaps(
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    OUT PHIDP_CAPS  Capabilities)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct hid_value_caps *it, *end;

    if (!preparsed || (RtlCompareMemory( preparsed->magic, "HidP KDR", 8 ) != 8)) return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Capabilities->Usage = preparsed->usage;
    Capabilities->UsagePage = preparsed->usage_page;
    Capabilities->InputReportByteLength = preparsed->input_report_byte_length;
    Capabilities->OutputReportByteLength = preparsed->output_report_byte_length;
    Capabilities->FeatureReportByteLength = preparsed->feature_report_byte_length;
    Capabilities->NumberLinkCollectionNodes = preparsed->number_link_collection_nodes;
    Capabilities->NumberInputButtonCaps = 0;
    Capabilities->NumberInputValueCaps = 0;
    Capabilities->NumberInputDataIndices = 0;
    Capabilities->NumberOutputButtonCaps = 0;
    Capabilities->NumberOutputValueCaps = 0;
    Capabilities->NumberOutputDataIndices = 0;
    Capabilities->NumberFeatureButtonCaps = 0;
    Capabilities->NumberFeatureValueCaps = 0;
    Capabilities->NumberFeatureDataIndices = 0;

    for (it = HID_INPUT_VALUE_CAPS( preparsed ), end = it + preparsed->input_caps_count;
         it != end; ++it)
    {
        if (!it->usage_min && !it->usage_max) continue;
        if (it->flags & HID_VALUE_CAPS_IS_BUTTON) Capabilities->NumberInputButtonCaps++;
        else Capabilities->NumberInputValueCaps++;
        if (!(it->flags & HID_VALUE_CAPS_IS_RANGE)) Capabilities->NumberInputDataIndices++;
        else Capabilities->NumberInputDataIndices += it->data_index_max - it->data_index_min + 1;
    }

    for (it = HID_OUTPUT_VALUE_CAPS( preparsed ), end = it + preparsed->output_caps_count;
         it != end; ++it)
    {
        if (!it->usage_min && !it->usage_max) continue;
        if (it->flags & HID_VALUE_CAPS_IS_BUTTON) Capabilities->NumberOutputButtonCaps++;
        else Capabilities->NumberOutputValueCaps++;
        if (!(it->flags & HID_VALUE_CAPS_IS_RANGE)) Capabilities->NumberOutputDataIndices++;
        else Capabilities->NumberOutputDataIndices += it->data_index_max - it->data_index_min + 1;
    }

    for (it = HID_FEATURE_VALUE_CAPS( preparsed ), end = it + preparsed->feature_caps_count;
         it != end; ++it)
    {
        if (!it->usage_min && !it->usage_max) continue;
        if (it->flags & HID_VALUE_CAPS_IS_BUTTON) Capabilities->NumberFeatureButtonCaps++;
        else Capabilities->NumberFeatureValueCaps++;
        if (!(it->flags & HID_VALUE_CAPS_IS_RANGE)) Capabilities->NumberFeatureDataIndices++;
        else Capabilities->NumberFeatureDataIndices += it->data_index_max - it->data_index_min + 1;
    }

    return HIDP_STATUS_SUCCESS;
}

struct usage_value_params
{
    BOOLEAN array;
    USAGE usage;
    void *value_buf;
    USHORT value_len;
    void *report_buf;
};

static LONG sign_extend( ULONG value, const struct hid_value_caps *caps )
{
    ULONG sign = 1 << (caps->bit_size - 1);
    if (sign <= 1 || caps->logical_min >= 0) return value;
    return value - ((value & sign) << 1);
}

static NTSTATUS get_usage_value( const struct hid_value_caps *caps, void *user )
{
    unsigned char *report_buf, start_bit = caps->start_bit;
    ULONG bit_count = caps->bit_size, bit_offset = 0;
    struct usage_value_params *params = user;

    if (params->array) bit_count *= caps->report_count;
    else bit_offset = (params->usage - caps->usage_min) * caps->bit_size;

    if ((bit_count + 7) / 8 > params->value_len) return HIDP_STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory( params->value_buf, params->value_len );

    report_buf = (unsigned char *)params->report_buf + caps->start_byte + bit_offset / 8;
    copy_bits( params->value_buf, report_buf, bit_count, -(int)(start_bit + bit_offset % 8) );

    return HIDP_STATUS_NULL;
}

static NTSTATUS get_scaled_usage_value( const struct hid_value_caps *caps, void *user )
{
    struct usage_value_params *params = user;
    LONG signed_value, *value = params->value_buf;
    ULONG unsigned_value = 0;
    NTSTATUS status;

    params->value_buf = &unsigned_value;
    params->value_len = sizeof(unsigned_value);
    if ((status = get_usage_value( caps, params )) != HIDP_STATUS_NULL) return status;

    if (sizeof(LONG) > params->value_len) return HIDP_STATUS_BUFFER_TOO_SMALL;
    signed_value = sign_extend( unsigned_value, caps );

    if (caps->logical_min > caps->logical_max || caps->physical_min > caps->physical_max)
        return HIDP_STATUS_BAD_LOG_PHY_VALUES;
    if (caps->logical_min > signed_value || caps->logical_max < signed_value)
        return HIDP_STATUS_VALUE_OUT_OF_RANGE;

    if (!caps->physical_min && !caps->physical_max) *value = signed_value;
    else *value = caps->physical_min + HidP_MulDiv( signed_value - caps->logical_min, caps->physical_max - caps->physical_min,
                                                    caps->logical_max - caps->logical_min );
    return HIDP_STATUS_NULL;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetScaledUsageValue(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    OUT PLONG  UsageValue,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    *UsageValue = 0;
    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.usage = Usage;
    params.value_buf = UsageValue;
    params.value_len = sizeof(*UsageValue);
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, get_scaled_usage_value, &params, &count );
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetUsageValue(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    OUT PULONG  UsageValue,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.usage = Usage;
    params.value_buf = UsageValue;
    params.value_len = sizeof(*UsageValue);
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, get_usage_value, &params, &count );
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetUsageValueArray(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    OUT PCHAR  UsageValue,
    IN USHORT  UsageValueByteLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.array = TRUE;
    params.usage = Usage;
    params.value_buf = UsageValue;
    params.value_len = UsageValueByteLength;
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.array = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, get_usage_value, &params, &count );
}

struct get_usage_params
{
    USAGE *usages;
    USAGE *usages_end;
    char *report_buf;
};

static NTSTATUS get_usage( const struct hid_value_caps *caps, void *user )
{
    const struct hid_value_caps *end = caps;
    ULONG index_min, index_max, bit, last;
    struct get_usage_params *params = user;
    unsigned char *report_buf;
    UCHAR index;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;
        index_max = index_min + caps->usage_max - caps->usage_min;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (!(index = report_buf[bit / 8]) || index < index_min || index > index_max) continue;
            if (params->usages < params->usages_end) *params->usages = caps->usage_min + index - index_min;
            params->usages++;
        }
        return HIDP_STATUS_SUCCESS;
    }

    for (bit = caps->start_bit, last = bit + caps->usage_max - caps->usage_min; bit <= last; ++bit)
    {
        if (!(report_buf[bit / 8] & (1 << (bit % 8)))) continue;
        if (params->usages < params->usages_end) *params->usages = caps->usage_min + bit - caps->start_bit;
        params->usages++;
    }

    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetUsages(
    IN HIDP_REPORT_TYPE ReportType,
    IN USAGE UsagePage,
    IN USHORT LinkCollection  OPTIONAL,
    OUT PUSAGE UsageList,
    IN OUT PULONG UsageLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR Report,
    IN ULONG ReportLength)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct get_usage_params params;
    struct caps_filter filter;
    NTSTATUS status;
    USHORT limit = -1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.usages = UsageList;
    params.usages_end = UsageList + *UsageLength;
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = USAGE_ANY;
    filter.report_id = Report[0];

    status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, get_usage, &params, &limit );
    *UsageLength = params.usages - UsageList;
    if (status != HIDP_STATUS_SUCCESS) return status;

    if (params.usages > params.usages_end) return HIDP_STATUS_BUFFER_TOO_SMALL;
    return status;
}

HIDAPI
NTSTATUS
NTAPI
HidP_InitializeReportForID(
    IN HIDP_REPORT_TYPE  ReportType,
    IN UCHAR  ReportID,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    const struct hid_value_caps *caps, *end;
    NTSTATUS status;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    status = get_value_caps_range( preparsed, ReportType, ReportLength, &caps, &end );
    if (status != HIDP_STATUS_SUCCESS) return status;

    while (caps != end && (caps->report_id != ReportID || (!caps->usage_min && !caps->usage_max))) caps++;
    if (caps == end) return HIDP_STATUS_REPORT_DOES_NOT_EXIST;

    RtlZeroMemory( Report, ReportLength );
    Report[0] = ReportID;
    return HIDP_STATUS_SUCCESS;
}

static NTSTATUS get_usage_list_length( const struct hid_value_caps *caps, void *data )
{
    *(ULONG *)data += caps->report_count;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
ULONG
NTAPI
HidP_MaxUsageListLength(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage  OPTIONAL,
    IN PHIDP_PREPARSED_DATA  PreparsedData)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT limit = -1;
    ULONG count = 0;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = UsagePage | USAGE_ANY;
    filter.usage = USAGE_ANY;

    enum_value_caps( preparsed, ReportType, 0, &filter, get_usage_list_length, &count, &limit );
    return count;
}

static NTSTATUS set_usage_value( const struct hid_value_caps *caps, void *user )
{
    unsigned char *report_buf, start_bit = caps->start_bit;
    ULONG bit_count = caps->bit_size, bit_offset = 0;
    struct usage_value_params *params = user;

    if (params->array) bit_count *= caps->report_count;
    else bit_offset = (params->usage - caps->usage_min) * caps->bit_size;

    if ((bit_count + 7) / 8 > params->value_len) return HIDP_STATUS_BUFFER_TOO_SMALL;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte + bit_offset / 8;
    copy_bits( report_buf, params->value_buf, bit_count, start_bit + bit_offset % 8 );

    return HIDP_STATUS_NULL;
}

static NTSTATUS set_scaled_usage_value( const struct hid_value_caps *caps, void *user )
{
    struct usage_value_params *params = user;
    LONG value, log_range, phy_range;

    if (caps->logical_min > caps->logical_max) return HIDP_STATUS_BAD_LOG_PHY_VALUES;
    if (caps->physical_min > caps->physical_max) return HIDP_STATUS_BAD_LOG_PHY_VALUES;

    if (sizeof(LONG) > params->value_len) return HIDP_STATUS_BUFFER_TOO_SMALL;
    value = *(LONG *)params->value_buf;

    if (caps->physical_min || caps->physical_max)
    {
        log_range = (caps->logical_max - caps->logical_min + 1) / 2;
        phy_range = (caps->physical_max - caps->physical_min + 1) / 2;
        value = value - caps->physical_min;
        value = (log_range * value) / phy_range;
        value = caps->logical_min + value;
    }

    params->value_buf = &value;
    params->value_len = sizeof(value);
    return set_usage_value( caps, params );
}

HIDAPI
NTSTATUS
NTAPI
HidP_SetScaledUsageValue(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    IN LONG  UsageValue,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.usage = Usage;
    params.value_buf = &UsageValue;
    params.value_len = sizeof(UsageValue);
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, set_scaled_usage_value, &params, &count );
}

HIDAPI
NTSTATUS
NTAPI
HidP_SetUsageValue(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    IN ULONG  UsageValue,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.usage = Usage;
    params.value_buf = &UsageValue;
    params.value_len = sizeof(UsageValue);
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, set_usage_value, &params, &count );
}

HIDAPI
NTSTATUS
NTAPI
HidP_SetUsageValueArray(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection  OPTIONAL,
    IN USAGE  Usage,
    IN PCHAR  UsageValue,
    IN USHORT  UsageValueByteLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct usage_value_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT count = 1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    RtlZeroMemory( &params, sizeof(params) );
    params.array = TRUE;
    params.usage = Usage;
    params.value_buf = UsageValue;
    params.value_len = UsageValueByteLength;
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.array = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = Usage;
    filter.report_id = Report[0];

    return enum_value_caps( preparsed, ReportType, ReportLength, &filter, set_usage_value, &params, &count );
}

struct set_usage_params
{
    USAGE usage;
    char *report_buf;
};

static NTSTATUS set_usage( const struct hid_value_caps *caps, void *user )
{
    const struct hid_value_caps *end = caps;
    struct set_usage_params *params = user;
    ULONG index_min, bit, last;
    unsigned char *report_buf;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (report_buf[bit / 8]) continue;
            report_buf[bit / 8] = index_min + params->usage - caps->usage_min;
            break;
        }

        if (bit > last) return HIDP_STATUS_BUFFER_TOO_SMALL;
        return HIDP_STATUS_NULL;
    }

    bit = caps->start_bit + params->usage - caps->usage_min;
    report_buf[bit / 8] |= (1 << (bit % 8));
    return HIDP_STATUS_NULL;
}

HIDAPI
NTSTATUS
NTAPI
HidP_SetUsages(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN PUSAGE  UsageList,
    IN OUT PULONG  UsageLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct set_usage_params params;
    struct caps_filter filter;
    NTSTATUS status;
    USHORT limit = 1;
    ULONG i, count = *UsageLength;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = USAGE_ANY;
    filter.report_id = Report[0];

    for (i = 0; i < count; ++i)
    {
        params.usage = filter.usage = UsageList[i];
        status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, set_usage, &params, &limit );
        if (status != HIDP_STATUS_SUCCESS)
        {
            *UsageLength = i;
            return status;
        }
    }

    return HIDP_STATUS_SUCCESS;
}

struct unset_usage_params
{
    USAGE usage;
    char *report_buf;
    BOOLEAN found;
};

static NTSTATUS unset_usage( const struct hid_value_caps *caps, void *user )
{
    ULONG index, index_min, index_max, bit, last;
    const struct hid_value_caps *end = caps;
    struct unset_usage_params *params = user;
    unsigned char *report_buf;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;
        index_max = index_min + caps->usage_max - caps->usage_min;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (!(index = report_buf[bit / 8]) || index < index_min || index > index_max) continue;
            report_buf[bit / 8] = 0;
            params->found = TRUE;
            break;
        }

        return HIDP_STATUS_NULL;
    }

    bit = caps->start_bit + params->usage - caps->usage_min;
    if (report_buf[bit / 8] & (1 << (bit % 8))) params->found = TRUE;
    report_buf[bit / 8] &= ~(1 << (bit % 8));
    return HIDP_STATUS_NULL;
}

HIDAPI
NTSTATUS
NTAPI
HidP_UnsetUsages(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN PUSAGE  UsageList,
    IN OUT PULONG  UsageLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct unset_usage_params params;
    struct caps_filter filter;
    NTSTATUS status;
    USHORT limit = 1;
    ULONG i, count = *UsageLength;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.report_buf = Report;
    params.found = FALSE;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = UsagePage;
    filter.collection = LinkCollection;
    filter.usage = USAGE_ANY;
    filter.report_id = Report[0];

    for (i = 0; i < count; ++i)
    {
        params.usage = filter.usage = UsageList[i];
        status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, unset_usage, &params, &limit );
        if (status != HIDP_STATUS_SUCCESS)
        {
            *UsageLength = i;
            return status;
        }
    }

    if (!params.found) return HIDP_STATUS_BUTTON_NOT_PRESSED;
    return HIDP_STATUS_SUCCESS;
}

static NTSTATUS get_button_caps( const struct hid_value_caps *caps, void *user )
{
    HIDP_BUTTON_CAPS **iter = user, *dst = *iter;
    RtlZeroMemory( dst, sizeof(*dst) );
    dst->UsagePage = caps->usage_page;
    dst->ReportID = caps->report_id;
    dst->LinkCollection = caps->link_collection;
    dst->LinkUsagePage = caps->link_usage_page;
    dst->LinkUsage = caps->link_usage;
    dst->BitField = caps->bit_field;
    dst->IsAlias = FALSE;
    dst->IsAbsolute = (caps->flags & HID_VALUE_CAPS_IS_ABSOLUTE) ? 1 : 0;
    dst->IsRange = (caps->flags & HID_VALUE_CAPS_IS_RANGE) ? 1 : 0;
    if (!dst->IsRange)
    {
        dst->NotRange.Usage = caps->usage_min;
        dst->NotRange.DataIndex = caps->data_index_min;
    }
    else
    {
        dst->Range.UsageMin = caps->usage_min;
        dst->Range.UsageMax = caps->usage_max;
        dst->Range.DataIndexMin = caps->data_index_min;
        dst->Range.DataIndexMax = caps->data_index_max;
    }
    dst->IsStringRange = (caps->flags & HID_VALUE_CAPS_IS_STRING_RANGE) ? 1 : 0;
    if (!dst->IsStringRange)
        dst->NotRange.StringIndex = caps->string_min;
    else
    {
        dst->Range.StringMin = caps->string_min;
        dst->Range.StringMax = caps->string_max;
    }
    dst->IsDesignatorRange = (caps->flags & HID_VALUE_CAPS_IS_DESIGNATOR_RANGE) ? 1 : 0;
    if (!dst->IsDesignatorRange)
        dst->NotRange.DesignatorIndex = caps->designator_min;
    else
    {
        dst->Range.DesignatorMin = caps->designator_min;
        dst->Range.DesignatorMax = caps->designator_max;
    }
    *iter += 1;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetSpecificButtonCaps(
    IN HIDP_REPORT_TYPE ReportType,
    IN USAGE UsagePage,
    IN USHORT LinkCollection,
    IN USAGE Usage,
    OUT PHIDP_BUTTON_CAPS ButtonCaps,
    IN OUT PUSHORT ButtonCapsLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = UsagePage | USAGE_ANY;
    filter.collection = LinkCollection;
    filter.usage = Usage | USAGE_ANY;

    return enum_value_caps( preparsed, ReportType, 0, &filter, get_button_caps, &ButtonCaps, ButtonCapsLength );
}

static NTSTATUS get_value_caps( const struct hid_value_caps *caps, void *user )
{
    HIDP_VALUE_CAPS **iter = user, *dst = *iter;
    RtlZeroMemory( dst, sizeof(*dst) );
    dst->UsagePage = caps->usage_page;
    dst->ReportID = caps->report_id;
    dst->LinkCollection = caps->link_collection;
    dst->LinkUsagePage = caps->link_usage_page;
    dst->LinkUsage = caps->link_usage;
    dst->BitField = caps->bit_field;
    dst->IsAlias = FALSE;
    dst->IsAbsolute = (caps->flags & HID_VALUE_CAPS_IS_ABSOLUTE) ? 1 : 0;
    dst->HasNull = HID_VALUE_CAPS_HAS_NULL( caps );
    dst->BitSize = caps->bit_size;
    dst->UnitsExp = caps->units_exp;
    dst->Units = caps->units;
    dst->LogicalMin = caps->logical_min;
    dst->LogicalMax = caps->logical_max;
    dst->PhysicalMin = caps->physical_min;
    dst->PhysicalMax = caps->physical_max;
    dst->IsRange = (caps->flags & HID_VALUE_CAPS_IS_RANGE) ? 1 : 0;
    if (!dst->IsRange)
    {
        dst->ReportCount = caps->report_count;
        dst->NotRange.Usage = caps->usage_min;
        dst->NotRange.DataIndex = caps->data_index_min;
    }
    else
    {
        dst->ReportCount = 1;
        dst->Range.UsageMin = caps->usage_min;
        dst->Range.UsageMax = caps->usage_max;
        dst->Range.DataIndexMin = caps->data_index_min;
        dst->Range.DataIndexMax = caps->data_index_max;
    }
    dst->IsStringRange = (caps->flags & HID_VALUE_CAPS_IS_STRING_RANGE) ? 1 : 0;
    if (!dst->IsStringRange)
        dst->NotRange.StringIndex = caps->string_min;
    else
    {
        dst->Range.StringMin = caps->string_min;
        dst->Range.StringMax = caps->string_max;
    }
    dst->IsDesignatorRange = (caps->flags & HID_VALUE_CAPS_IS_DESIGNATOR_RANGE) ? 1 : 0;
    if (!dst->IsDesignatorRange)
        dst->NotRange.DesignatorIndex = caps->designator_min;
    else
    {
        dst->Range.DesignatorMin = caps->designator_min;
        dst->Range.DesignatorMax = caps->designator_max;
    }
    *iter += 1;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetSpecificValueCaps(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USAGE  UsagePage,
    IN USHORT  LinkCollection,
    IN USAGE  Usage,
    OUT PHIDP_VALUE_CAPS  ValueCaps,
    IN OUT PUSHORT  ValueCapsLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.values = TRUE;
    filter.usage_page = UsagePage | USAGE_ANY;
    filter.collection = LinkCollection;
    filter.usage = Usage | USAGE_ANY;

    return enum_value_caps( preparsed, ReportType, 0, &filter, get_value_caps, &ValueCaps, ValueCapsLength );
}

struct get_usage_and_page_params
{
    USAGE_AND_PAGE *usages;
    USAGE_AND_PAGE *usages_end;
    char *report_buf;
};

static NTSTATUS get_usage_and_page( const struct hid_value_caps *caps, void *user )
{
    struct get_usage_and_page_params *params = user;
    const struct hid_value_caps *end = caps;
    ULONG index_min, index_max, bit, last;
    unsigned char *report_buf;
    UCHAR index;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;
        index_max = index_min + caps->usage_max - caps->usage_min;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (!(index = report_buf[bit / 8]) || index < index_min || index > index_max) continue;
            if (params->usages < params->usages_end)
            {
                params->usages->UsagePage = caps->usage_page;
                params->usages->Usage = caps->usage_min + index - index_min;
            }
            params->usages++;
        }
        return HIDP_STATUS_SUCCESS;
    }

    for (bit = caps->start_bit, last = bit + caps->usage_max - caps->usage_min; bit <= last; bit++)
    {
        if (!(report_buf[bit / 8] & (1 << (bit % 8)))) continue;
        if (params->usages < params->usages_end)
        {
            params->usages->UsagePage = caps->usage_page;
            params->usages->Usage = caps->usage_min + bit - caps->start_bit;
        }
        params->usages++;
    }

    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetUsagesEx(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USHORT  LinkCollection,
    OUT PUSAGE_AND_PAGE  ButtonList,
    IN OUT ULONG  *UsageLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct get_usage_and_page_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    NTSTATUS status;
    USHORT limit = -1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.usages = ButtonList;
    params.usages_end = ButtonList + *UsageLength;
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.buttons = TRUE;
    filter.usage_page = USAGE_ANY;
    filter.collection = LinkCollection;
    filter.usage = USAGE_ANY;
    filter.report_id = Report[0];

    status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, get_usage_and_page, &params, &limit );
    *UsageLength = params.usages - ButtonList;
    if (status != HIDP_STATUS_SUCCESS) return status;

    if (params.usages > params.usages_end) return HIDP_STATUS_BUFFER_TOO_SMALL;
    return status;
}

static NTSTATUS count_data( const struct hid_value_caps *caps, void *user )
{
    BOOLEAN is_button = caps->flags & HID_VALUE_CAPS_IS_BUTTON;
    BOOLEAN is_range = caps->flags & HID_VALUE_CAPS_IS_RANGE;
    if (is_range || is_button) *(ULONG *)user += caps->report_count;
    else *(ULONG *)user += 1;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
ULONG
NTAPI
HidP_MaxDataListLength(
    IN HIDP_REPORT_TYPE  ReportType,
    IN PHIDP_PREPARSED_DATA  PreparsedData)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    USHORT limit = -1;
    ULONG count = 0;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.usage_page = USAGE_ANY;
    filter.usage = USAGE_ANY;

    enum_value_caps( preparsed, ReportType, 0, &filter, count_data, &count, &limit );
    return count;
}

struct find_all_data_params
{
    HIDP_DATA *data;
    HIDP_DATA *data_end;
    char *report_buf;
};

static NTSTATUS find_all_data( const struct hid_value_caps *caps, void *user )
{
    struct find_all_data_params *params = user;
    HIDP_DATA *data = params->data, *data_end = params->data_end;
    ULONG index_min, index_max, bit, last, bit_count;
    const struct hid_value_caps *end = caps;
    unsigned char *report_buf;
    UCHAR index;

    if (!caps->bit_size) return HIDP_STATUS_SUCCESS;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;
        index_max = index_min + caps->usage_max - caps->usage_min;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (!(index = report_buf[bit / 8]) || index < index_min || index > index_max) continue;
            if (data < data_end)
            {
                data->DataIndex = caps->data_index_min + index - index_min;
                data->On = 1;
            }
            data++;
        }
    }
    else if (caps->flags & HID_VALUE_CAPS_IS_BUTTON)
    {
        for (bit = caps->start_bit, last = bit + caps->usage_max - caps->usage_min; bit <= last; bit++)
        {
            if (!(report_buf[bit / 8] & (1 << (bit % 8)))) continue;
            if (data < data_end)
            {
                data->DataIndex = caps->data_index_min + bit - caps->start_bit;
                data->On = 1;
            }
            data++;
        }
    }
    else if (caps->report_count == 1)
    {
        if (data < data_end)
        {
            data->DataIndex = caps->data_index_min;
            data->RawValue = 0;
            bit_count = caps->bit_size * caps->report_count;
            if ((bit_count + 7) / 8 > sizeof(data->RawValue)) return HIDP_STATUS_BUFFER_TOO_SMALL;
            copy_bits( (void *)&data->RawValue, report_buf, bit_count, -caps->start_bit );
        }
        data++;
    }

    params->data = data;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetData(
    IN HIDP_REPORT_TYPE  ReportType,
    OUT PHIDP_DATA  DataList,
    IN OUT PULONG  DataLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct find_all_data_params params;
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct caps_filter filter;
    NTSTATUS status;
    USHORT limit = -1;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.data = DataList;
    params.data_end = DataList + *DataLength;
    params.report_buf = Report;

    RtlZeroMemory( &filter, sizeof(filter) );
    filter.usage_page = USAGE_ANY;
    filter.usage = USAGE_ANY;
    filter.report_id = Report[0];

    status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, find_all_data, &params, &limit );
    *DataLength = params.data - DataList;
    if (status != HIDP_STATUS_SUCCESS) return status;

    if (params.data > params.data_end) return HIDP_STATUS_BUFFER_TOO_SMALL;
    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetLinkCollectionNodes(
    OUT PHIDP_LINK_COLLECTION_NODE  LinkCollectionNodes,
    IN OUT PULONG  LinkCollectionNodesLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct hid_collection_node *collections = HID_COLLECTION_NODES( preparsed );
    ULONG i, count, capacity = *LinkCollectionNodesLength;

    if (!preparsed || (RtlCompareMemory( preparsed->magic, "HidP KDR", 8 ) != 8)) return HIDP_STATUS_INVALID_PREPARSED_DATA;

    count = *LinkCollectionNodesLength = preparsed->number_link_collection_nodes;
    if (capacity < count) return HIDP_STATUS_BUFFER_TOO_SMALL;

    for (i = 0; i < count; ++i)
    {
        LinkCollectionNodes[i].LinkUsagePage = collections[i].usage_page;
        LinkCollectionNodes[i].LinkUsage = collections[i].usage;
        LinkCollectionNodes[i].Parent = collections[i].parent;
        LinkCollectionNodes[i].CollectionType = collections[i].collection_type;
        LinkCollectionNodes[i].FirstChild = collections[i].first_child;
        LinkCollectionNodes[i].NextSibling = collections[i].next_sibling;
        LinkCollectionNodes[i].NumberOfChildren = collections[i].number_of_children;
        LinkCollectionNodes[i].IsAlias = 0;
    }

    return HIDP_STATUS_SUCCESS;
}

struct set_data_params
{
    HIDP_DATA *data;
    char *report_buf;
    BOOLEAN found;
};

static NTSTATUS set_data( const struct hid_value_caps *caps, void *user )
{
    ULONG index_min, index_max, index, lookup, bit, last, mask, bit_count;
    const struct hid_value_caps *end = caps;
    struct set_data_params *params = user;
    HIDP_DATA *data = params->data;
    unsigned char *report_buf;

    if (!caps->bit_size) return HIDP_STATUS_SUCCESS;
    if (data->DataIndex < caps->data_index_min) return HIDP_STATUS_SUCCESS;
    if (data->DataIndex > caps->data_index_max) return HIDP_STATUS_SUCCESS;
    params->found = TRUE;

    report_buf = (unsigned char *)params->report_buf + caps->start_byte;

    if (HID_VALUE_CAPS_IS_ARRAY( caps ))
    {
        if (!(caps->flags & HID_VALUE_CAPS_IS_RANGE)) return HIDP_STATUS_IS_VALUE_ARRAY;

        while (end->flags & HID_VALUE_CAPS_ARRAY_HAS_MORE) end++;
        index_min = end - caps + 1;
        index_max = index_min + caps->usage_max - caps->usage_min;
        lookup = index_min + data->DataIndex - caps->data_index_min;

        for (bit = caps->start_bit, last = bit + caps->report_count * caps->bit_size - 1; bit <= last; bit += 8)
        {
            if (!(index = report_buf[bit / 8]) || index < index_min || index > index_max) break;
            if (!data->RawValue && index == lookup) break;
        }
        if (bit > last) return data->RawValue ? HIDP_STATUS_BUFFER_TOO_SMALL : HIDP_STATUS_BUTTON_NOT_PRESSED;

        if (data->RawValue) report_buf[bit / 8] = lookup;
        else if (report_buf[bit / 8] != lookup) return HIDP_STATUS_BUTTON_NOT_PRESSED;
        else
        {
            while (bit < last) { report_buf[bit / 8] = report_buf[bit / 8 + 1]; bit++; }
            report_buf[bit / 8] = 0;
        }
    }
    else if (caps->flags & HID_VALUE_CAPS_IS_BUTTON)
    {
        bit = caps->start_bit + (data->DataIndex - caps->data_index_min) * caps->bit_size;
        mask = 1 << (bit % 8);

        if (data->On) report_buf[bit / 8] |= mask;
        else if (!(report_buf[bit / 8] & mask)) return HIDP_STATUS_BUTTON_NOT_PRESSED;
        else report_buf[bit / 8] &= ~mask;
    }
    else if (caps->report_count == 1)
    {
        bit_count = caps->bit_size * caps->report_count;
        copy_bits( report_buf, (void *)&data->RawValue, bit_count, -caps->start_bit );
    }

    return HIDP_STATUS_SUCCESS;
}

HIDAPI
NTSTATUS
NTAPI
HidP_SetData(
    IN HIDP_REPORT_TYPE  ReportType,
    IN PHIDP_DATA  DataList,
    IN OUT PULONG  DataLength,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    IN OUT PCHAR  Report,
    IN ULONG  ReportLength)
{
    struct hid_preparsed_data *preparsed = (struct hid_preparsed_data *)PreparsedData;
    struct set_data_params params;
    HIDP_DATA *data_end = DataList + *DataLength;
    NTSTATUS status = HIDP_STATUS_SUCCESS;

    if (!ReportLength) return HIDP_STATUS_INVALID_REPORT_LENGTH;

    params.report_buf = Report;
    params.found = FALSE;

    for (params.data = DataList; params.data < data_end; params.data++)
    {
        struct caps_filter filter;
        USHORT limit = -1;

        RtlZeroMemory( &filter, sizeof(filter) );
        filter.usage_page = USAGE_ANY;
        filter.usage = USAGE_ANY;
        filter.report_id = Report[0];

        status = enum_value_caps( preparsed, ReportType, ReportLength, &filter, set_data, &params, &limit );
        if (status != HIDP_STATUS_SUCCESS || !params.found) break;
    }
    *DataLength = params.data - DataList;

    if (!params.found) return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    return status;
}

/* ---- Thin wrappers / unimplemented stubs --------------------------------- */

VOID
NTAPI
HidP_FreeCollectionDescription(
    IN PHIDP_DEVICE_DESC   DeviceDescription)
{
    HidParser_FreeCollectionDescription(DeviceDescription);
}

NTSTATUS
NTAPI
HidP_GetCollectionDescription(
    IN PHIDP_REPORT_DESCRIPTOR ReportDesc,
    IN ULONG DescLength,
    IN POOL_TYPE PoolType,
    OUT PHIDP_DEVICE_DESC DeviceDescription)
{
    return HidParser_GetCollectionDescription(ReportDesc, DescLength, PoolType, DeviceDescription);
}

#undef HidP_GetButtonCaps

HIDAPI
NTSTATUS
NTAPI
HidP_GetButtonCaps(
    HIDP_REPORT_TYPE ReportType,
    PHIDP_BUTTON_CAPS ButtonCaps,
    PUSHORT ButtonCapsLength,
    PHIDP_PREPARSED_DATA PreparsedData)
{
    return HidP_GetSpecificButtonCaps(ReportType, HID_USAGE_PAGE_UNDEFINED, 0, 0, ButtonCaps, ButtonCapsLength, PreparsedData);
}

#undef HidP_GetValueCaps

HIDAPI
NTSTATUS
NTAPI
HidP_GetValueCaps(
    HIDP_REPORT_TYPE ReportType,
    PHIDP_VALUE_CAPS ValueCaps,
    PUSHORT ValueCapsLength,
    PHIDP_PREPARSED_DATA PreparsedData)
{
    return HidP_GetSpecificValueCaps(ReportType,
                                     HID_USAGE_PAGE_UNDEFINED,
                                     HIDP_LINK_COLLECTION_UNSPECIFIED,
                                     0,
                                     ValueCaps,
                                     ValueCapsLength,
                                     PreparsedData);
}

HIDAPI
NTSTATUS
NTAPI
HidP_UsageListDifference(
    IN PUSAGE  PreviousUsageList,
    IN PUSAGE  CurrentUsageList,
    OUT PUSAGE  BreakUsageList,
    OUT PUSAGE  MakeUsageList,
    IN ULONG  UsageListLength)
{
    return HidParser_UsageListDifference(PreviousUsageList, CurrentUsageList, BreakUsageList, MakeUsageList, UsageListLength);
}

HIDAPI
NTSTATUS
NTAPI
HidP_UsageAndPageListDifference(
    IN PUSAGE_AND_PAGE  PreviousUsageList,
    IN PUSAGE_AND_PAGE  CurrentUsageList,
    OUT PUSAGE_AND_PAGE  BreakUsageList,
    OUT PUSAGE_AND_PAGE  MakeUsageList,
    IN ULONG  UsageListLength)
{
    return HidParser_UsageAndPageListDifference(PreviousUsageList, CurrentUsageList, BreakUsageList, MakeUsageList, UsageListLength);
}

HIDAPI
NTSTATUS
NTAPI
HidP_TranslateUsageAndPagesToI8042ScanCodes(
    IN PUSAGE_AND_PAGE  ChangedUsageList,
    IN ULONG  UsageListLength,
    IN HIDP_KEYBOARD_DIRECTION  KeyAction,
    IN OUT PHIDP_KEYBOARD_MODIFIER_STATE  ModifierState,
    IN PHIDP_INSERT_SCANCODES  InsertCodesProcedure,
    IN PVOID  InsertCodesContext)
{
    return HidParser_TranslateUsageAndPagesToI8042ScanCodes(ChangedUsageList, UsageListLength, KeyAction, ModifierState, InsertCodesProcedure, InsertCodesContext);
}

HIDAPI
NTSTATUS
NTAPI
HidP_TranslateUsagesToI8042ScanCodes(
    IN PUSAGE  ChangedUsageList,
    IN ULONG  UsageListLength,
    IN HIDP_KEYBOARD_DIRECTION  KeyAction,
    IN OUT PHIDP_KEYBOARD_MODIFIER_STATE  ModifierState,
    IN PHIDP_INSERT_SCANCODES  InsertCodesProcedure,
    IN PVOID  InsertCodesContext)
{
    return HidParser_TranslateUsagesToI8042ScanCodes(ChangedUsageList, UsageListLength, KeyAction, ModifierState, InsertCodesProcedure, InsertCodesContext);
}

HIDAPI
NTSTATUS
NTAPI
HidP_GetExtendedAttributes(
    IN HIDP_REPORT_TYPE  ReportType,
    IN USHORT DataIndex,
    IN PHIDP_PREPARSED_DATA  PreparsedData,
    OUT PHIDP_EXTENDED_ATTRIBUTES  Attributes,
    IN OUT PULONG  LengthAttributes)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
HidP_SysPowerEvent(
    IN PCHAR HidPacket,
    IN USHORT HidPacketLength,
    IN PHIDP_PREPARSED_DATA Ppd,
    OUT PULONG OutputBuffer)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
HidP_SysPowerCaps(
    IN PHIDP_PREPARSED_DATA Ppd,
    OUT PULONG OutputBuffer)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}
