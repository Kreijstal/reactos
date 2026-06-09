#ifndef _HIDPARSER_H_
#define _HIDPARSER_H_

#include <wdm.h>
#define _HIDPI_
#define _HIDPI_NO_FUNCTION_MACROS_
#include <hidpddi.h>

#include "hidparser.h"
#include "hidp.h"

/*
 * The HID parser produces and consumes a Windows-binary-compatible preparsed
 * data blob (the "HidP KDR" format). The on-blob structures and the parser
 * itself are adapted from the Wine project (dlls/hidparse.sys, dlls/hid),
 * Copyright 2015 Aric Stewart, Copyright 2021 Rémi Bernon for CodeWeavers.
 */

struct hid_collection_node
{
    USAGE usage;
    USAGE usage_page;
    USHORT parent;
    USHORT number_of_children;
    USHORT next_sibling;
    USHORT first_child;
    ULONG collection_type;
};

struct hid_value_caps
{
    USHORT usage_page;
    UCHAR report_id;
    UCHAR start_bit;
    USHORT bit_size;
    USHORT report_count;
    USHORT start_byte;
    USHORT total_bits;
    ULONG bit_field;
    USHORT end_byte;
    USHORT link_collection;
    USAGE link_usage_page;
    USAGE link_usage;
    ULONG flags;
    ULONG padding[8];
    USAGE usage_min;
    USAGE usage_max;
    USHORT string_min;
    USHORT string_max;
    USHORT designator_min;
    USHORT designator_max;
    USHORT data_index_min;
    USHORT data_index_max;
    USHORT null_value;
    USHORT unknown;
    LONG logical_min;
    LONG logical_max;
    LONG physical_min;
    LONG physical_max;
    LONG units;
    LONG units_exp;
};

/* named array continues on next caps */
#define HID_VALUE_CAPS_ARRAY_HAS_MORE       0x01
#define HID_VALUE_CAPS_IS_CONSTANT          0x02
#define HID_VALUE_CAPS_IS_BUTTON            0x04
#define HID_VALUE_CAPS_IS_ABSOLUTE          0x08
#define HID_VALUE_CAPS_IS_RANGE             0x10
#define HID_VALUE_CAPS_IS_STRING_RANGE      0x40
#define HID_VALUE_CAPS_IS_DESIGNATOR_RANGE  0x80

#define HID_VALUE_CAPS_HAS_NULL(x) (((x)->bit_field & 0x40) != 0)
#define HID_VALUE_CAPS_IS_ARRAY(c) (((c)->bit_field & 2) == 0)

struct hid_preparsed_data
{
    char magic[8];
    USAGE usage;
    USAGE usage_page;
    USHORT unknown[2];
    USHORT input_caps_start;
    USHORT input_caps_count;
    USHORT input_caps_end;
    USHORT input_report_byte_length;
    USHORT output_caps_start;
    USHORT output_caps_count;
    USHORT output_caps_end;
    USHORT output_report_byte_length;
    USHORT feature_caps_start;
    USHORT feature_caps_count;
    USHORT feature_caps_end;
    USHORT feature_report_byte_length;
    USHORT caps_size;
    USHORT number_link_collection_nodes;
    struct hid_value_caps value_caps[1];
    /* struct hid_collection_node nodes[1] */
};

#define HID_INPUT_VALUE_CAPS(d) ((d)->value_caps + (d)->input_caps_start)
#define HID_OUTPUT_VALUE_CAPS(d) ((d)->value_caps + (d)->output_caps_start)
#define HID_FEATURE_VALUE_CAPS(d) ((d)->value_caps + (d)->feature_caps_start)
#define HID_COLLECTION_NODES(d) (struct hid_collection_node *)((char *)(d)->value_caps + (d)->caps_size)

#endif /* _HIDPARSER_H_ */
