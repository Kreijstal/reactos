/*
 * NTFS Attribute Definition Table
 * Standard attribute definitions for NTFS 3.1
 */
#ifdef NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#define memset(d, v, n) RtlFillMemory(d, n, v)
#else
#include <string.h>
#endif
#include "ntfs_types.h"

/* Helper to initialize ATTR_DEF with ASCII name (all NTFS attr names are ASCII) */
static void set_attrdef(ATTR_DEF *ad, const char *name, ULONG type,
                        ULONG flags, ULONGLONG min_size, ULONGLONG max_size)
{
    int i;
    memset(ad, 0, sizeof(*ad));
    for (i = 0; name[i] && i < 63; i++)
        ad->Name[i] = (WCHAR)(unsigned char)name[i];
    ad->Type = type;
    ad->Flags = flags;
    ad->MinSize = min_size;
    ad->MaxSize = max_size;
}

/* Build the table at runtime since we can't use L"" portably with uint16_t WCHAR */
static ATTR_DEF s_attrdef_table[16];
static int s_attrdef_built = 0;

static void build_attrdef_table(void)
{
    if (s_attrdef_built) return;
    set_attrdef(&s_attrdef_table[0],  "$STANDARD_INFORMATION",  0x10, 0x40, 48, 72);
    set_attrdef(&s_attrdef_table[1],  "$ATTRIBUTE_LIST",        0x20, 0x80, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[2],  "$FILE_NAME",             0x30, 0x42, 68, 578);
    set_attrdef(&s_attrdef_table[3],  "$OBJECT_ID",             0x40, 0x40, 0, 256);
    set_attrdef(&s_attrdef_table[4],  "$SECURITY_DESCRIPTOR",   0x50, 0x80, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[5],  "$VOLUME_NAME",           0x60, 0x40, 2, 256);
    set_attrdef(&s_attrdef_table[6],  "$VOLUME_INFORMATION",    0x70, 0x40, 12, 12);
    set_attrdef(&s_attrdef_table[7],  "$DATA",                  0x80, 0x00, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[8],  "$INDEX_ROOT",            0x90, 0x40, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[9],  "$INDEX_ALLOCATION",      0xA0, 0x80, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[10], "$BITMAP",                0xB0, 0x80, 0, (ULONGLONG)-1);
    set_attrdef(&s_attrdef_table[11], "$REPARSE_POINT",         0xC0, 0x80, 0, 16384);
    set_attrdef(&s_attrdef_table[12], "$EA_INFORMATION",        0xD0, 0x40, 8, 8);
    set_attrdef(&s_attrdef_table[13], "$EA",                    0xE0, 0x00, 0, 65536);
    set_attrdef(&s_attrdef_table[14], "$LOGGED_UTILITY_STREAM", 0x100, 0x80, 0, 65536);
    s_attrdef_built = 1;
}

const ATTR_DEF *get_attrdef_table(ULONG *count)
{
    build_attrdef_table();
    *count = 15;
    return s_attrdef_table;
}
