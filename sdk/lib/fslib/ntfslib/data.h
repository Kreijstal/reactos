/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.h
 * PURPOSE:     NTFS lib definitions
 * PROGRAMMERS: Klachkov Valery
 */

#ifndef NTFSLIB_DATA_H
#define NTFSLIB_DATA_H


/* INCLUDES ******************************************************************/

#include "ntfslib.h"


/* STRUCTURES & ENUMS ********************************************************/

typedef enum
{
    Indexed     = 0x02,
    Resident    = 0x40,  // Always
    NonResident = 0x80   // Allowed to be
} ATTR_FLAG, *PATTR_FLAG;

typedef struct
{
    WCHAR     Label[64];
    ULONG     Type;
    ULONG     DisplayRule;
    ULONG     CollationRule;
    ULONG     Flags;          // See ATTR_FLAG
    ULONGLONG MinimumSize;
    ULONGLONG MaximumSize;
} ATTR_DEF, *PATTR_DEF;


/* TABLES ********************************************************************/

const ATTR_DEF ATTRIBUTES_TABLE[] =
{
    { L"$STANDARD_INFORMATION" , AttributeStandardInformation, 0, 0, Resident          , 0x30, 0x48       },
    { L"$ATTRIBUTE_LIST"       , AttributeAttributeList      , 0, 0, NonResident       , 0   , ULLONG_MAX },
    { L"$FILE_NAME"            , AttributeFileName           , 0, 0, Indexed | Resident, 0x44, 0x242      },
    { L"$OBJECT_ID"            , AttributeObjectId           , 0, 0, Resident          , 0   , 0x100      },
    { L"$SECURITY_DESCRIPTOR"  , AttributeSecurityDescriptor , 0, 0, NonResident       , 0   , ULLONG_MAX },
    { L"$VOLUME_NAME"          , AttributeVolumeName         , 0, 0, Resident          , 0x02, 0x100      },
    { L"$VOLUME_INFORMATION"   , AttributeVolumeInformation  , 0, 0, Resident          , 0x0C, 0x0C       },
    { L"$DATA"                 , AttributeData               , 0, 0, 0                 , 0   , ULLONG_MAX },
    { L"$INDEX_ROOT"           , AttributeIndexRoot          , 0, 0, Resident          , 0   , ULLONG_MAX },
    { L"$INDEX_ALLOCATION"     , AttributeIndexAllocation    , 0, 0, NonResident       , 0   , ULLONG_MAX },
    { L"$BITMAP"               , AttributeBitmap             , 0, 0, NonResident       , 0   , ULLONG_MAX },
    { L"$REPARSE_POINT"        , AttributeReparsePoint       , 0, 0, NonResident       , 0   , 0x4000     },
    { L"$EA_INFORMATION"       , AttributeEAInformation      , 0, 0, Resident          , 0x08, 0x08       },
    { L"$EA"                   , AttributeEA                 , 0, 0, 0                 , 0   , 0x10000    },
    { L"$LOGGED_UTILITY_STREAM", AttributeLoggedUtilityStream, 0, 0, NonResident       , 0   , 0x10000    },
    { L""                      , 0                           , 0, 0, 0                 , 0   , 0          }
};

/* The uppercase template is shared with the checker in upcase_data.c. */

#endif
