/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     HID report descriptor parser producing the Windows-compatible
 *              "HidP KDR" preparsed data blob.
 * COPYRIGHT:   Copyright 2021 Rémi Bernon for CodeWeavers
 *              Adapted for ReactOS from Wine's dlls/hidparse.sys/main.c
 */

#include "parser.h"

#define NDEBUG
#include <debug.h>

/* Flags that are defined in the document
   "Device Class Definition for Human Interface Devices" */
enum
{
    INPUT_DATA_CONST = 0x01, /* Data (0)             | Constant (1)       */
    INPUT_ARRAY_VAR = 0x02,  /* Array (0)            | Variable (1)       */
    INPUT_ABS_REL = 0x04,    /* Absolute (0)         | Relative (1)       */
    INPUT_WRAP = 0x08,       /* No Wrap (0)          | Wrap (1)           */
    INPUT_LINEAR = 0x10,     /* Linear (0)           | Non Linear (1)     */
    INPUT_PREFSTATE = 0x20,  /* Preferred State (0)  | No Preferred (1)   */
    INPUT_NULL = 0x40,       /* No Null position (0) | Null state(1)      */
    INPUT_VOLATILE = 0x80,   /* Non Volatile (0)     | Volatile (1)       */
    INPUT_BITFIELD = 0x100   /* Bit Field (0)        | Buffered Bytes (1) */
};

enum
{
    TAG_TYPE_MAIN = 0x0,
    TAG_TYPE_GLOBAL,
    TAG_TYPE_LOCAL,
    TAG_TYPE_RESERVED,
};

enum
{
    TAG_MAIN_INPUT = 0x08,
    TAG_MAIN_OUTPUT = 0x09,
    TAG_MAIN_FEATURE = 0x0B,
    TAG_MAIN_COLLECTION = 0x0A,
    TAG_MAIN_END_COLLECTION = 0x0C
};

enum
{
    TAG_GLOBAL_USAGE_PAGE = 0x0,
    TAG_GLOBAL_LOGICAL_MINIMUM,
    TAG_GLOBAL_LOGICAL_MAXIMUM,
    TAG_GLOBAL_PHYSICAL_MINIMUM,
    TAG_GLOBAL_PHYSICAL_MAXIMUM,
    TAG_GLOBAL_UNIT_EXPONENT,
    TAG_GLOBAL_UNIT,
    TAG_GLOBAL_REPORT_SIZE,
    TAG_GLOBAL_REPORT_ID,
    TAG_GLOBAL_REPORT_COUNT,
    TAG_GLOBAL_PUSH,
    TAG_GLOBAL_POP
};

enum
{
    TAG_LOCAL_USAGE = 0x0,
    TAG_LOCAL_USAGE_MINIMUM,
    TAG_LOCAL_USAGE_MAXIMUM,
    TAG_LOCAL_DESIGNATOR_INDEX,
    TAG_LOCAL_DESIGNATOR_MINIMUM,
    TAG_LOCAL_DESIGNATOR_MAXIMUM,
    TAG_LOCAL_STRING_INDEX = 0x7,
    TAG_LOCAL_STRING_MINIMUM,
    TAG_LOCAL_STRING_MAXIMUM,
    TAG_LOCAL_DELIMITER
};

struct hid_parser_state
{
    USAGE usage;
    USAGE usage_page;
    USHORT input_report_byte_length;
    USHORT output_report_byte_length;
    USHORT feature_report_byte_length;
    USHORT number_link_collection_nodes;

    USAGE usages_page[256];
    USAGE usages_min[256];
    USAGE usages_max[256];
    ULONG usages_size;

    struct hid_value_caps items;

    struct hid_value_caps *stack;
    ULONG                  stack_size;
    ULONG                  global_idx;
    ULONG                  collection_idx;

    struct hid_value_caps *collections;
    ULONG                  collections_size;

    struct hid_value_caps *values[3];
    ULONG                  values_size[3];

    ULONG  bit_size[3][256];
    USHORT byte_length[3];
    USHORT caps_count[3];
    USHORT empty_caps[3];
    USHORT data_count[3];
};

static PVOID
ReallocFunction(
    IN PVOID Old,
    IN ULONG OldSize,
    IN ULONG NewSize)
{
    PVOID New = AllocFunction(NewSize);
    if (New == NULL)
        return NULL;
    if (Old)
    {
        CopyFunction(New, Old, min(OldSize, NewSize));
        FreeFunction(Old);
    }
    return New;
}

static BOOLEAN
array_reserve(
    IN OUT PVOID *array,
    IN OUT PULONG array_size,
    IN ULONG index,
    IN ULONG elem_size)
{
    ULONG new_size;
    PVOID new_array;

    if (index < *array_size)
        return TRUE;

    /*
     * Grow geometrically, but never below 32: a small *array_size (e.g. 1)
     * would make (n * 3 / 2) stall at the same value through integer
     * truncation and loop forever.
     */
    new_size = *array_size;
    if (new_size < 32)
        new_size = 32;
    while (new_size <= index)
        new_size += new_size / 2;

    new_array = ReallocFunction(*array, *array_size * elem_size, new_size * elem_size);
    if (new_array == NULL)
        return FALSE;

    *array = new_array;
    *array_size = new_size;
    return TRUE;
}

static void copy_global_items( struct hid_value_caps *dst, const struct hid_value_caps *src )
{
    dst->usage_page = src->usage_page;
    dst->logical_min = src->logical_min;
    dst->logical_max = src->logical_max;
    dst->physical_min = src->physical_min;
    dst->physical_max = src->physical_max;
    dst->units_exp = src->units_exp;
    dst->units = src->units;
    dst->bit_size = src->bit_size;
    dst->report_id = src->report_id;
    dst->report_count = src->report_count;
}

static void copy_collection_items( struct hid_value_caps *dst, const struct hid_value_caps *src )
{
    dst->link_collection = src->link_collection;
    dst->link_usage_page = src->link_usage_page;
    dst->link_usage = src->link_usage;
}

static void reset_local_items( struct hid_parser_state *state )
{
    struct hid_value_caps tmp;
    copy_global_items( &tmp, &state->items );
    copy_collection_items( &tmp, &state->items );
    RtlZeroMemory( &state->items, sizeof(state->items) );
    copy_global_items( &state->items, &tmp );
    copy_collection_items( &state->items, &tmp );
    RtlZeroMemory( &state->usages_page, sizeof(state->usages_page) );
    RtlZeroMemory( &state->usages_min, sizeof(state->usages_min) );
    RtlZeroMemory( &state->usages_max, sizeof(state->usages_max) );
    state->usages_size = 0;
}

static BOOLEAN parse_global_push( struct hid_parser_state *state )
{
    if (!array_reserve( (PVOID *)&state->stack, &state->stack_size, state->global_idx, sizeof(*state->stack) ))
    {
        DPRINT1( "HID parser stack overflow!\n" );
        return FALSE;
    }

    copy_global_items( state->stack + state->global_idx, &state->items );
    state->global_idx++;
    return TRUE;
}

static BOOLEAN parse_global_pop( struct hid_parser_state *state )
{
    if (!state->global_idx)
    {
        DPRINT1( "HID parser global stack underflow!\n" );
        return FALSE;
    }

    state->global_idx--;
    copy_global_items( &state->items, state->stack + state->global_idx );
    return TRUE;
}

static BOOLEAN parse_local_usage( struct hid_parser_state *state, USAGE usage_page, USAGE usage )
{
    if (!usage_page) usage_page = state->items.usage_page;
    if (state->items.flags & HID_VALUE_CAPS_IS_RANGE) state->usages_size = 0;
    state->usages_page[state->usages_size] = usage_page;
    state->usages_min[state->usages_size] = usage;
    state->usages_max[state->usages_size] = usage;
    state->items.usage_min = usage;
    state->items.usage_max = usage;
    state->items.flags &= ~HID_VALUE_CAPS_IS_RANGE;
    if (state->usages_size++ == 255) DPRINT1( "HID parser usages stack overflow!\n" );
    return state->usages_size <= 255;
}

static void parse_local_usage_min( struct hid_parser_state *state, USAGE usage_page, USAGE usage )
{
    if (!usage_page) usage_page = state->items.usage_page;
    if (!(state->items.flags & HID_VALUE_CAPS_IS_RANGE)) state->usages_max[0] = 0;
    state->usages_page[0] = usage_page;
    state->usages_min[0] = usage;
    state->items.usage_min = usage;
    state->items.flags |= HID_VALUE_CAPS_IS_RANGE;
    state->usages_size = 1;
}

static void parse_local_usage_max( struct hid_parser_state *state, USAGE usage_page, USAGE usage )
{
    if (!usage_page) usage_page = state->items.usage_page;
    if (!(state->items.flags & HID_VALUE_CAPS_IS_RANGE)) state->usages_min[0] = 0;
    state->usages_page[0] = usage_page;
    state->usages_max[0] = usage;
    state->items.usage_max = usage;
    state->items.flags |= HID_VALUE_CAPS_IS_RANGE;
    state->usages_size = 1;
}

static BOOLEAN parse_new_collection( struct hid_parser_state *state )
{
    if (!array_reserve( (PVOID *)&state->stack, &state->stack_size, state->collection_idx, sizeof(*state->stack) ))
    {
        DPRINT1( "HID parser stack overflow!\n" );
        return FALSE;
    }

    if (!array_reserve( (PVOID *)&state->collections, &state->collections_size, state->number_link_collection_nodes, sizeof(*state->collections) ))
    {
        DPRINT1( "HID parser collections overflow!\n" );
        return FALSE;
    }

    copy_collection_items( state->stack + state->collection_idx, &state->items );
    state->collection_idx++;

    state->items.usage_min = state->usages_min[0];
    state->items.usage_max = state->usages_max[0];

    state->collections[state->number_link_collection_nodes] = state->items;
    state->items.link_collection = state->number_link_collection_nodes;
    state->items.link_usage_page = state->items.usage_page;
    state->items.link_usage = state->items.usage_min;
    if (!state->number_link_collection_nodes)
    {
        state->usage_page = state->items.usage_page;
        state->usage = state->items.usage_min;
    }
    state->number_link_collection_nodes++;

    reset_local_items( state );
    return TRUE;
}

static BOOLEAN parse_end_collection( struct hid_parser_state *state )
{
    if (!state->collection_idx)
    {
        DPRINT1( "HID parser collection stack underflow!\n" );
        return FALSE;
    }

    state->collection_idx--;
    copy_collection_items( &state->items, state->stack + state->collection_idx );
    reset_local_items( state );
    return TRUE;
}

static void add_new_value_caps( struct hid_parser_state *state, struct hid_value_caps *values,
                                LONG i, ULONG start_bit )
{
    ULONG count, usages_size = max( 1, state->usages_size );

    state->items.start_byte = start_bit / 8;
    state->items.start_bit = start_bit % 8;
    state->items.total_bits = state->items.report_count * state->items.bit_size;
    state->items.end_byte = (start_bit + state->items.total_bits + 7) / 8;
    state->items.usage_page = state->usages_page[usages_size - 1 - i];
    state->items.usage_min = state->usages_min[usages_size - 1 - i];
    state->items.usage_max = state->usages_max[usages_size - 1 - i];
    if (!state->items.usage_max && !state->items.usage_min) count = -1;
    else count = state->items.usage_max - state->items.usage_min;
    state->items.data_index_min = state->items.data_index_max + 1;
    state->items.data_index_max = state->items.data_index_min + count;
    values[i] = state->items;

    if (values[i].flags & HID_VALUE_CAPS_IS_BUTTON)
    {
        if (!HID_VALUE_CAPS_IS_ARRAY( values + i )) values[i].logical_min = 0;
        else values[i].logical_min = values[i].logical_max;
        values[i].logical_max = 0;
        values[i].physical_min = 0;
        values[i].physical_max = 0;
    }
}

static BOOLEAN parse_new_value_caps( struct hid_parser_state *state, HIDP_REPORT_TYPE type )
{
    struct hid_value_caps *values;
    USAGE usage_page = state->items.usage_page;
    USHORT report_count = state->items.report_count;
    ULONG i, usages_size = max( 1, state->usages_size );
    USHORT *byte_length = &state->byte_length[type];
    ULONG start_bit, *bit_size = &state->bit_size[type][state->items.report_id];
    BOOLEAN is_array;

    if (!*bit_size) *bit_size = 8;
    *bit_size += state->items.bit_size * state->items.report_count;
    *byte_length = max( *byte_length, (*bit_size + 7) / 8 );
    start_bit = *bit_size;

    if (!state->items.report_count)
    {
        state->empty_caps[type] += usages_size;
        reset_local_items( state );
        return TRUE;
    }

    /*
     * A constant field that carries no usage and whose report size is a whole
     * number of bytes is a reserved/padding byte (e.g. the boot-keyboard
     * reserved byte). Windows accounts for its report bits but does not expose
     * a value cap for it, unlike sub-byte constant bit padding. The report
     * length was already accumulated above, so skipping the cap keeps the
     * report layout intact while matching the Windows preparsed-data size.
     */
    if ((state->items.bit_field & INPUT_DATA_CONST) && state->usages_size == 0 &&
        state->items.bit_size != 0 && (state->items.bit_size % 8) == 0)
    {
        reset_local_items( state );
        return TRUE;
    }

    if (!array_reserve( (PVOID *)&state->values[type], &state->values_size[type],
                        state->caps_count[type] + usages_size, sizeof(*state->values[type]) ))
    {
        DPRINT1( "HID parser values overflow!\n" );
        return FALSE;
    }
    values = state->values[type] + state->caps_count[type];

    if (!(is_array = HID_VALUE_CAPS_IS_ARRAY( &state->items ))) state->items.report_count -= usages_size - 1;
    else start_bit -= state->items.report_count * state->items.bit_size;

    if (!(state->items.bit_field & INPUT_ABS_REL)) state->items.flags |= HID_VALUE_CAPS_IS_ABSOLUTE;
    if (state->items.bit_field & INPUT_DATA_CONST) state->items.flags |= HID_VALUE_CAPS_IS_CONSTANT;
    if (state->items.bit_size == 1 || is_array) state->items.flags |= HID_VALUE_CAPS_IS_BUTTON;

    if (is_array) state->items.null_value = state->items.logical_min;
    else if (!(state->items.bit_field & INPUT_NULL)) state->items.null_value = 0;
    else state->items.null_value = 1;

    state->items.data_index_max = state->data_count[type] - 1;
    for (i = 0; i < usages_size; ++i)
    {
        if (!is_array) start_bit -= state->items.report_count * state->items.bit_size;
        else if (i) state->items.flags |= HID_VALUE_CAPS_ARRAY_HAS_MORE;
        else state->items.flags &= ~HID_VALUE_CAPS_ARRAY_HAS_MORE;
        add_new_value_caps( state, values, is_array ? usages_size - i - 1 : i, start_bit );
        if (!is_array) state->items.report_count = 1;
    }
    state->caps_count[type] += usages_size;
    state->data_count[type] = state->items.data_index_max + 1;

    state->items.usage_page = usage_page;
    state->items.report_count = report_count;
    reset_local_items( state );
    return TRUE;
}

static void free_parser_state( struct hid_parser_state *state )
{
    if (state->global_idx) DPRINT1( "%lu unpopped device caps on the stack\n", state->global_idx );
    if (state->collection_idx) DPRINT1( "%lu unpopped device collection on the stack\n", state->collection_idx );
    if (state->stack) FreeFunction( state->stack );
    if (state->collections) FreeFunction( state->collections );
    if (state->values[HidP_Input]) FreeFunction( state->values[HidP_Input] );
    if (state->values[HidP_Output]) FreeFunction( state->values[HidP_Output] );
    if (state->values[HidP_Feature]) FreeFunction( state->values[HidP_Feature] );
    FreeFunction( state );
}

static struct hid_preparsed_data *build_preparsed_data( struct hid_parser_state *state )
{
    struct hid_collection_node *nodes;
    struct hid_preparsed_data *data;
    struct hid_value_caps *caps;
    ULONG i, size, caps_size;

    caps_size = state->caps_count[HidP_Input] + state->caps_count[HidP_Output] +
                state->caps_count[HidP_Feature];
    caps_size += state->empty_caps[HidP_Input] + state->empty_caps[HidP_Output] +
                 state->empty_caps[HidP_Feature];
    caps_size *= sizeof(struct hid_value_caps);

    size = caps_size + FIELD_OFFSET(struct hid_preparsed_data, value_caps[0]) +
           state->number_link_collection_nodes * sizeof(struct hid_collection_node);
    if (!(data = AllocFunction( size ))) return NULL;

    RtlCopyMemory( data->magic, "HidP KDR", 8 );
    data->usage = state->usage;
    data->usage_page = state->usage_page;
    data->input_caps_start = 0;
    data->input_caps_count = state->caps_count[HidP_Input] + state->empty_caps[HidP_Input];
    data->input_caps_end = data->input_caps_start + state->caps_count[HidP_Input];
    data->input_report_byte_length = state->byte_length[HidP_Input];
    data->output_caps_start = data->input_caps_end;
    data->output_caps_count = state->caps_count[HidP_Output] + state->empty_caps[HidP_Output];
    data->output_caps_end = data->output_caps_start + state->caps_count[HidP_Output];
    data->output_report_byte_length = state->byte_length[HidP_Output];
    data->feature_caps_start = data->output_caps_end;
    data->feature_caps_count = state->caps_count[HidP_Feature] + state->empty_caps[HidP_Feature];
    data->feature_caps_end = data->feature_caps_start + state->caps_count[HidP_Feature];
    data->feature_report_byte_length = state->byte_length[HidP_Feature];
    data->caps_size = caps_size;
    data->number_link_collection_nodes = state->number_link_collection_nodes;

    caps = HID_INPUT_VALUE_CAPS( data );
    RtlCopyMemory( caps, state->values[HidP_Input], state->caps_count[HidP_Input] * sizeof(*caps) );
    caps = HID_OUTPUT_VALUE_CAPS( data );
    RtlCopyMemory( caps, state->values[HidP_Output], state->caps_count[HidP_Output] * sizeof(*caps) );
    caps = HID_FEATURE_VALUE_CAPS( data );
    RtlCopyMemory( caps, state->values[HidP_Feature], state->caps_count[HidP_Feature] * sizeof(*caps) );

    nodes = HID_COLLECTION_NODES( data );
    for (i = 0; i < data->number_link_collection_nodes; ++i)
    {
        nodes[i].usage_page = state->collections[i].usage_page;
        nodes[i].usage = state->collections[i].usage_min;
        nodes[i].parent = state->collections[i].link_collection;
        nodes[i].collection_type = state->collections[i].bit_field;
        nodes[i].first_child = 0;
        nodes[i].next_sibling = 0;
        nodes[i].number_of_children = 0;

        if (i > 0)
        {
            nodes[i].next_sibling = nodes[nodes[i].parent].first_child;
            nodes[nodes[i].parent].first_child = i;
            nodes[nodes[i].parent].number_of_children++;
        }
    }

    return data;
}

static struct hid_preparsed_data *parse_descriptor( PUCHAR descriptor, ULONG length )
{
    struct hid_preparsed_data *data = NULL;
    struct hid_parser_state *state;
    ULONG size, value;
    LONG signed_value;
    PUCHAR ptr, end;

    if (!(state = AllocFunction( sizeof(*state) ))) return NULL;

    for (ptr = descriptor, end = descriptor + length; ptr != end; ptr += size + 1)
    {
        size = (*ptr & 0x03);
        if (size == 3) size = 4;
        if (ptr + size > end)
        {
            DPRINT1( "Need %lu bytes to read item value\n", size );
            goto done;
        }

        if (size == 0) signed_value = value = 0;
        else if (size == 1) signed_value = (CHAR)(value = *(PUCHAR)(ptr + 1));
        else if (size == 2) signed_value = (SHORT)(value = *(PUSHORT)(ptr + 1));
        else if (size == 4) signed_value = (LONG)(value = *(PULONG)(ptr + 1));
        else
        {
            DPRINT1( "Unexpected item value size %lu.\n", size );
            goto done;
        }

        state->items.bit_field = value;

#define SHORT_ITEM( tag, type ) (((tag) << 4) | ((type) << 2))
        switch (*ptr & SHORT_ITEM( 0xf, 0x3 ))
        {
        case SHORT_ITEM( TAG_MAIN_INPUT, TAG_TYPE_MAIN ):
            if (!parse_new_value_caps( state, HidP_Input )) goto done;
            break;
        case SHORT_ITEM( TAG_MAIN_OUTPUT, TAG_TYPE_MAIN ):
            if (!parse_new_value_caps( state, HidP_Output )) goto done;
            break;
        case SHORT_ITEM( TAG_MAIN_FEATURE, TAG_TYPE_MAIN ):
            if (!parse_new_value_caps( state, HidP_Feature )) goto done;
            break;
        case SHORT_ITEM( TAG_MAIN_COLLECTION, TAG_TYPE_MAIN ):
            if (!parse_new_collection( state )) goto done;
            break;
        case SHORT_ITEM( TAG_MAIN_END_COLLECTION, TAG_TYPE_MAIN ):
            if (!parse_end_collection( state )) goto done;
            break;

        case SHORT_ITEM( TAG_GLOBAL_USAGE_PAGE, TAG_TYPE_GLOBAL ):
            state->items.usage_page = value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_LOGICAL_MINIMUM, TAG_TYPE_GLOBAL ):
            state->items.logical_min = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_LOGICAL_MAXIMUM, TAG_TYPE_GLOBAL ):
            state->items.logical_max = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_PHYSICAL_MINIMUM, TAG_TYPE_GLOBAL ):
            state->items.physical_min = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_PHYSICAL_MAXIMUM, TAG_TYPE_GLOBAL ):
            state->items.physical_max = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_UNIT_EXPONENT, TAG_TYPE_GLOBAL ):
            state->items.units_exp = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_UNIT, TAG_TYPE_GLOBAL ):
            state->items.units = signed_value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_REPORT_SIZE, TAG_TYPE_GLOBAL ):
            state->items.bit_size = value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_REPORT_ID, TAG_TYPE_GLOBAL ):
            state->items.report_id = value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_REPORT_COUNT, TAG_TYPE_GLOBAL ):
            state->items.report_count = value;
            break;
        case SHORT_ITEM( TAG_GLOBAL_PUSH, TAG_TYPE_GLOBAL ):
            if (!parse_global_push( state )) goto done;
            break;
        case SHORT_ITEM( TAG_GLOBAL_POP, TAG_TYPE_GLOBAL ):
            if (!parse_global_pop( state )) goto done;
            break;

        case SHORT_ITEM( TAG_LOCAL_USAGE, TAG_TYPE_LOCAL ):
            if (!parse_local_usage( state, value >> 16, value & 0xffff )) goto done;
            break;
        case SHORT_ITEM( TAG_LOCAL_USAGE_MINIMUM, TAG_TYPE_LOCAL ):
            parse_local_usage_min( state, value >> 16, value & 0xffff );
            break;
        case SHORT_ITEM( TAG_LOCAL_USAGE_MAXIMUM, TAG_TYPE_LOCAL ):
            parse_local_usage_max( state, value >> 16, value & 0xffff );
            break;
        case SHORT_ITEM( TAG_LOCAL_DESIGNATOR_INDEX, TAG_TYPE_LOCAL ):
            state->items.designator_min = state->items.designator_max = value;
            state->items.flags &= ~HID_VALUE_CAPS_IS_DESIGNATOR_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_DESIGNATOR_MINIMUM, TAG_TYPE_LOCAL ):
            state->items.designator_min = value;
            state->items.flags |= HID_VALUE_CAPS_IS_DESIGNATOR_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_DESIGNATOR_MAXIMUM, TAG_TYPE_LOCAL ):
            state->items.designator_max = value;
            state->items.flags |= HID_VALUE_CAPS_IS_DESIGNATOR_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_STRING_INDEX, TAG_TYPE_LOCAL ):
            state->items.string_min = state->items.string_max = value;
            state->items.flags &= ~HID_VALUE_CAPS_IS_STRING_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_STRING_MINIMUM, TAG_TYPE_LOCAL ):
            state->items.string_min = value;
            state->items.flags |= HID_VALUE_CAPS_IS_STRING_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_STRING_MAXIMUM, TAG_TYPE_LOCAL ):
            state->items.string_max = value;
            state->items.flags |= HID_VALUE_CAPS_IS_STRING_RANGE;
            break;
        case SHORT_ITEM( TAG_LOCAL_DELIMITER, TAG_TYPE_LOCAL ):
            DPRINT1( "delimiter %lu not implemented!\n", value );
            goto done;

        default:
            DPRINT1( "item type %x not implemented!\n", *ptr );
            break;
        }
#undef SHORT_ITEM
    }

    data = build_preparsed_data( state );

done:
    free_parser_state( state );
    return data;
}

static PUCHAR *parse_top_level_collections( PUCHAR descriptor, ULONG length, PULONG count )
{
    PUCHAR ptr, end, *tmp, *tlcs;
    ULONG size, depth = 0, capacity = 1;

    if (!(tlcs = AllocFunction( sizeof(*tlcs) ))) return NULL;
    tlcs[0] = descriptor;
    *count = 0;

    for (ptr = descriptor, end = descriptor + length; ptr != end; ptr += size + 1)
    {
        size = (*ptr & 0x03);
        if (size == 3) size = 4;
        if (ptr + size > end)
        {
            DPRINT1( "Need %lu bytes to read item value\n", size );
            break;
        }

#define SHORT_ITEM( tag, type ) (((tag) << 4) | ((type) << 2))
        switch (*ptr & SHORT_ITEM( 0xf, 0x3 ))
        {
        case SHORT_ITEM( TAG_MAIN_COLLECTION, TAG_TYPE_MAIN ):
            if (depth++) break;
            break;
        case SHORT_ITEM( TAG_MAIN_END_COLLECTION, TAG_TYPE_MAIN ):
            if (--depth) break;
            *count = *count + 1;
            if (!array_reserve( (PVOID *)&tlcs, &capacity, *count + 1, sizeof(*tlcs) ))
            {
                DPRINT1( "Failed to allocate memory for TLCs\n" );
                return tlcs;
            }
            tmp = tlcs;
            (void)tmp;
            tlcs[*count] = ptr + size + 1;
            break;
        }
#undef SHORT_ITEM
    }

    DPRINT( "Found %lu TLCs\n", *count );
    return tlcs;
}

NTSTATUS
NTAPI
HidParser_GetCollectionDescription(
    IN PHIDP_REPORT_DESCRIPTOR ReportDesc,
    IN ULONG DescLength,
    IN POOL_TYPE PoolType,
    OUT PHIDP_DEVICE_DESC DeviceDescription)
{
    ULONG i, len, tlc_count, report_count = 0;
    PULONG input_len, output_len, feature_len, collection;
    PULONG scratch;
    struct hid_value_caps *caps, *caps_end;
    struct hid_preparsed_data *preparsed;
    PUCHAR *tlcs;

    UNREFERENCED_PARAMETER(PoolType);

    RtlZeroMemory( DeviceDescription, sizeof(*DeviceDescription) );

    /* per-report-id scratch space, kept off the (small) kernel stack */
    scratch = AllocFunction( 4 * 256 * sizeof(ULONG) );
    if (!scratch) return HIDP_STATUS_INTERNAL_ERROR;
    input_len = scratch;
    output_len = scratch + 256;
    feature_len = scratch + 512;
    collection = scratch + 768;

    if (!(tlcs = parse_top_level_collections( ReportDesc, DescLength, &tlc_count )))
    {
        FreeFunction( scratch );
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (tlc_count == 0)
    {
        /* an empty or collection-less descriptor yields no data */
        FreeFunction( tlcs );
        FreeFunction( scratch );
        return STATUS_NO_DATA_DETECTED;
    }

    len = sizeof(*DeviceDescription->CollectionDesc) * tlc_count;
    if (!(DeviceDescription->CollectionDesc = AllocFunction( len ))) goto failed;

    for (i = 0; i < tlc_count; ++i)
    {
        if (!(preparsed = parse_descriptor( tlcs[i], tlcs[i + 1] - tlcs[i] ))) goto failed;

        len = preparsed->caps_size + FIELD_OFFSET(struct hid_preparsed_data, value_caps[0]) +
              preparsed->number_link_collection_nodes * sizeof(struct hid_collection_node);

        DeviceDescription->CollectionDescLength++;
        DeviceDescription->CollectionDesc[i].UsagePage = preparsed->usage_page;
        DeviceDescription->CollectionDesc[i].Usage = preparsed->usage;
        DeviceDescription->CollectionDesc[i].CollectionNumber = i + 1;
        DeviceDescription->CollectionDesc[i].InputLength = preparsed->input_report_byte_length;
        DeviceDescription->CollectionDesc[i].OutputLength = preparsed->output_report_byte_length;
        DeviceDescription->CollectionDesc[i].FeatureLength = preparsed->feature_report_byte_length;
        DeviceDescription->CollectionDesc[i].PreparsedDataLength = len;
        DeviceDescription->CollectionDesc[i].PreparsedData = (PHIDP_PREPARSED_DATA)preparsed;

        caps = HID_INPUT_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->input_caps_end - preparsed->input_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id]) report_count++;
            input_len[caps->report_id] = max(input_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }

        caps = HID_OUTPUT_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->output_caps_end - preparsed->output_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id] && !output_len[caps->report_id]) report_count++;
            output_len[caps->report_id] = max(output_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }

        caps = HID_FEATURE_VALUE_CAPS( preparsed );
        caps_end = caps + preparsed->feature_caps_end - preparsed->feature_caps_start;
        for (; caps != caps_end; ++caps)
        {
            len = caps->start_byte * 8 + caps->start_bit + caps->bit_size * caps->report_count;
            if (!input_len[caps->report_id] && !output_len[caps->report_id] && !feature_len[caps->report_id]) report_count++;
            feature_len[caps->report_id] = max(feature_len[caps->report_id], len);
            collection[caps->report_id] = i;
        }
    }

    len = sizeof(*DeviceDescription->ReportIDs) * report_count;
    if (!(DeviceDescription->ReportIDs = AllocFunction( len ))) goto failed;

    for (i = 0, report_count = 0; i < 256; ++i)
    {
        /*
         * The caps reserve a leading byte for the report id (Wine starts every
         * report at bit 8). Reports that belong to report id 0 carry no id byte
         * on the wire, so the per-report id length excludes it; reports with a
         * real id keep it. The collection-level length always includes it.
         */
        ULONG id_byte = (i == 0) ? 1 : 0;

        if (!input_len[i] && !output_len[i] && !feature_len[i]) continue;
        DeviceDescription->ReportIDs[report_count].ReportID = i;
        DeviceDescription->ReportIDs[report_count].CollectionNumber = collection[i] + 1;
        DeviceDescription->ReportIDs[report_count].InputLength = input_len[i] ? (input_len[i] + 7) / 8 - id_byte : 0;
        DeviceDescription->ReportIDs[report_count].OutputLength = output_len[i] ? (output_len[i] + 7) / 8 - id_byte : 0;
        DeviceDescription->ReportIDs[report_count].FeatureLength = feature_len[i] ? (feature_len[i] + 7) / 8 - id_byte : 0;
        report_count++;
    }
    DeviceDescription->ReportIDsLength = report_count;

    FreeFunction( tlcs );
    FreeFunction( scratch );
    return STATUS_SUCCESS;

failed:
    if (DeviceDescription->CollectionDesc)
    {
        for (i = 0; i < DeviceDescription->CollectionDescLength; ++i)
            FreeFunction( DeviceDescription->CollectionDesc[i].PreparsedData );
        FreeFunction( DeviceDescription->CollectionDesc );
    }
    FreeFunction( tlcs );
    FreeFunction( scratch );
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID
NTAPI
HidParser_FreeCollectionDescription(
    IN PHIDP_DEVICE_DESC DeviceDescription)
{
    ULONG i;

    for (i = 0; i < DeviceDescription->CollectionDescLength; ++i)
        FreeFunction( DeviceDescription->CollectionDesc[i].PreparsedData );
    FreeFunction( DeviceDescription->CollectionDesc );
    FreeFunction( DeviceDescription->ReportIDs );
}
