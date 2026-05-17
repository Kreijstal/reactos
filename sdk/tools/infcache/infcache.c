/*
 * PROJECT:     ReactOS INF cache generator
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <typedefs.h>
#include <infhost.h>

typedef struct _PLATFORM_INFO
{
    const WCHAR *Architecture;
    unsigned MajorVersion;
    unsigned MinorVersion;
} PLATFORM_INFO;

typedef struct _INF_CACHE_ENTRY
{
    struct _INF_CACHE_ENTRY *Next;
    const char *FileName;
    WCHAR *SectionName;
    WCHAR *DriverDescription;
    WCHAR *ProviderName;
    WCHAR *ManufacturerName;
    WCHAR *MatchingId;
    WCHAR ClassGuid[39];
    uint32_t DriverDateLow;
    uint32_t DriverDateHigh;
    uint64_t DriverVersion;
    unsigned FieldIndex;
} INF_CACHE_ENTRY;

static const WCHAR Version[] = {'V','e','r','s','i','o','n',0};
static const WCHAR ClassGUID[] = {'C','l','a','s','s','G','U','I','D',0};
static const WCHAR Provider[] = {'P','r','o','v','i','d','e','r',0};
static const WCHAR DriverVer[] = {'D','r','i','v','e','r','V','e','r',0};
static const WCHAR Manufacturer[] = {'M','a','n','u','f','a','c','t','u','r','e','r',0};
static const WCHAR Header[] = {'R','e','a','c','t','O','S',' ','I','N','F',' ','C','a','c','h','e',' ','1','\r','\n',0};

static size_t
wcs_len(const WCHAR *String)
{
    size_t Length = 0;

    while (String && String[Length])
        ++Length;
    return Length;
}

static int
wcs_equal_i(const WCHAR *Left, const WCHAR *Right)
{
    while (*Left && *Right)
    {
        WCHAR L = *Left++;
        WCHAR R = *Right++;

        if (L >= 'A' && L <= 'Z')
            L += 'a' - 'A';
        if (R >= 'A' && R <= 'Z')
            R += 'a' - 'A';
        if (L != R)
            return 0;
    }

    return *Left == 0 && *Right == 0;
}

static WCHAR *
wcs_dup(const WCHAR *String)
{
    size_t Length = wcs_len(String);
    WCHAR *Copy = malloc((Length + 1) * sizeof(WCHAR));

    if (!Copy)
        return NULL;

    memcpy(Copy, String, (Length + 1) * sizeof(WCHAR));
    return Copy;
}

static WCHAR *
wcs_dup_sanitized(const WCHAR *String)
{
    WCHAR *Copy = wcs_dup(String ? String : (const WCHAR[]){0});
    size_t i;

    if (!Copy)
        return NULL;

    for (i = 0; Copy[i]; ++i)
    {
        if (Copy[i] == '\t' || Copy[i] == '\r' || Copy[i] == '\n')
            Copy[i] = ' ';
    }

    return Copy;
}

static void
wcs_copy(WCHAR *Destination, const WCHAR *Source)
{
    while ((*Destination++ = *Source++) != 0)
        ;
}

static void
wcs_append(WCHAR *Destination, const WCHAR *Source)
{
    wcs_copy(Destination + wcs_len(Destination), Source);
}

static void
wcs_append_ascii(WCHAR *Destination, const char *Source)
{
    Destination += wcs_len(Destination);
    while (*Source)
        *Destination++ = (unsigned char)*Source++;
    *Destination = 0;
}

static int
get_string_field(PINFCONTEXT Context, unsigned FieldIndex, WCHAR **Value)
{
    ULONG RequiredSize;

    *Value = NULL;
    if (InfHostGetStringField(Context, FieldIndex, NULL, 0, &RequiredSize) != 0)
        return -1;

    *Value = malloc(RequiredSize * sizeof(WCHAR));
    if (!*Value)
        return -1;

    if (InfHostGetStringField(Context, FieldIndex, *Value, RequiredSize, &RequiredSize) != 0)
    {
        free(*Value);
        *Value = NULL;
        return -1;
    }

    return 0;
}

static int
get_line_text(HINF Inf, const WCHAR *Section, const WCHAR *Key, WCHAR **Value)
{
    PINFCONTEXT Context;
    int Ret;

    if (InfHostFindFirstLine(Inf, Section, Key, &Context) != 0)
        return -1;

    Ret = get_string_field(Context, 1, Value);
    InfHostFreeContext(Context);
    return Ret;
}

static const char *
path_basename(const char *Path)
{
    const char *Slash = strrchr(Path, '/');
    const char *BackSlash = strrchr(Path, '\\');
    const char *Base = Slash > BackSlash ? Slash : BackSlash;

    return Base ? Base + 1 : Path;
}

static unsigned
parse_uint(const WCHAR *String)
{
    unsigned Value = 0;

    while (*String >= '0' && *String <= '9')
        Value = Value * 10 + (*String++ - '0');

    return Value;
}

static uint64_t
days_from_civil(unsigned Year, unsigned Month, unsigned Day)
{
    uint64_t Era;
    unsigned Yoe, Doy, Doe;

    if (Month <= 2)
        --Year;
    Era = Year / 400;
    Yoe = Year - Era * 400;
    Doy = (153 * (Month + (Month > 2 ? (unsigned)-3 : 9)) + 2) / 5 + Day - 1;
    Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
    return Era * 146097 + Doe - 719468;
}

static void
parse_driver_ver(WCHAR *DriverVerValue, uint32_t *DateLow, uint32_t *DateHigh, uint64_t *Version)
{
    WCHAR *Comma;
    unsigned Month, Day, Year;

    *DateLow = 0;
    *DateHigh = 0;
    *Version = 0;

    if (!DriverVerValue)
        return;

    Comma = DriverVerValue;
    while (*Comma && *Comma != ',')
        ++Comma;
    if (*Comma == ',')
        *Comma++ = 0;
    else
        Comma = NULL;

    if (wcs_len(DriverVerValue) == 10 &&
        (DriverVerValue[2] == '-' || DriverVerValue[2] == '/') &&
        (DriverVerValue[5] == '-' || DriverVerValue[5] == '/'))
    {
        uint64_t FileTime;

        DriverVerValue[2] = 0;
        DriverVerValue[5] = 0;
        Month = parse_uint(DriverVerValue);
        Day = parse_uint(DriverVerValue + 3);
        Year = parse_uint(DriverVerValue + 6);
        FileTime = (days_from_civil(Year, Month, Day) + 11644473600ULL) * 10000000ULL;
        *DateLow = (uint32_t)FileTime;
        *DateHigh = (uint32_t)(FileTime >> 32);
    }

    if (Comma)
    {
        unsigned Parts[4] = {0, 0, 0, 0};
        unsigned Index;

        for (Index = 0; Index < 4 && *Comma; ++Index)
        {
            Parts[Index] = parse_uint(Comma);
            while (*Comma && *Comma != '.')
                ++Comma;
            if (*Comma == '.')
                ++Comma;
        }

        *Version = ((uint64_t)Parts[0] << 48) |
                   ((uint64_t)Parts[1] << 32) |
                   ((uint64_t)Parts[2] << 16) |
                   Parts[3];
    }
}

static int
section_exists(HINF Inf, const WCHAR *SectionName)
{
    PINFCONTEXT Context;

    if (InfHostFindFirstLine(Inf, SectionName, NULL, &Context) != 0)
        return 0;

    InfHostFreeContext(Context);
    return 1;
}

static int
find_actual_section(HINF Inf, const WCHAR *BaseSection, const PLATFORM_INFO *Platform, WCHAR **ActualSection)
{
    WCHAR Candidate[512];
    char VersionSuffix[32];
    static const WCHAR DotNT[] = {'.','N','T',0};
    unsigned Minor;

    snprintf(VersionSuffix, sizeof(VersionSuffix), ".%u.%u", Platform->MajorVersion, Platform->MinorVersion);
    wcs_copy(Candidate, BaseSection);
    wcs_append(Candidate, DotNT);
    wcs_append(Candidate, Platform->Architecture);
    wcs_append_ascii(Candidate, VersionSuffix);
    if (section_exists(Inf, Candidate))
        goto found;

    for (Minor = Platform->MinorVersion + 1; Minor-- > 0;)
    {
        snprintf(VersionSuffix, sizeof(VersionSuffix), ".%u.%u", Platform->MajorVersion, Minor);
        wcs_copy(Candidate, BaseSection);
        wcs_append(Candidate, DotNT);
        wcs_append(Candidate, Platform->Architecture);
        wcs_append_ascii(Candidate, VersionSuffix);
        if (section_exists(Inf, Candidate))
            goto found;
    }

    snprintf(VersionSuffix, sizeof(VersionSuffix), ".%u", Platform->MajorVersion);
    wcs_copy(Candidate, BaseSection);
    wcs_append(Candidate, DotNT);
    wcs_append(Candidate, Platform->Architecture);
    wcs_append_ascii(Candidate, VersionSuffix);
    if (section_exists(Inf, Candidate))
        goto found;

    wcs_copy(Candidate, BaseSection);
    wcs_append(Candidate, DotNT);
    wcs_append(Candidate, Platform->Architecture);
    if (section_exists(Inf, Candidate))
        goto found;

    for (Minor = Platform->MinorVersion + 1; Minor-- > 0;)
    {
        snprintf(VersionSuffix, sizeof(VersionSuffix), ".%u.%u", Platform->MajorVersion, Minor);
        wcs_copy(Candidate, BaseSection);
        wcs_append(Candidate, DotNT);
        wcs_append_ascii(Candidate, VersionSuffix);
        if (section_exists(Inf, Candidate))
            goto found;
    }

    wcs_copy(Candidate, BaseSection);
    wcs_append(Candidate, DotNT);
    if (section_exists(Inf, Candidate))
        goto found;

    wcs_copy(Candidate, BaseSection);
    if (!section_exists(Inf, Candidate))
        return -1;

found:
    *ActualSection = wcs_dup(Candidate);
    return *ActualSection ? 0 : -1;
}

static int
add_entry(INF_CACHE_ENTRY **Head,
          INF_CACHE_ENTRY **Tail,
          const char *FileName,
          const WCHAR *SectionName,
          const WCHAR *DriverDescription,
          const WCHAR *ProviderName,
          const WCHAR *ManufacturerName,
          const WCHAR *MatchingId,
          const WCHAR *ClassGuid,
          uint32_t DriverDateLow,
          uint32_t DriverDateHigh,
          uint64_t DriverVersion,
          unsigned FieldIndex)
{
    INF_CACHE_ENTRY *Entry = calloc(1, sizeof(*Entry));

    if (!Entry)
        return -1;

    Entry->FileName = FileName;
    Entry->SectionName = wcs_dup_sanitized(SectionName);
    Entry->DriverDescription = wcs_dup_sanitized(DriverDescription);
    Entry->ProviderName = wcs_dup_sanitized(ProviderName);
    Entry->ManufacturerName = wcs_dup_sanitized(ManufacturerName);
    Entry->MatchingId = wcs_dup_sanitized(MatchingId);
    wcs_copy(Entry->ClassGuid, ClassGuid);
    Entry->DriverDateLow = DriverDateLow;
    Entry->DriverDateHigh = DriverDateHigh;
    Entry->DriverVersion = DriverVersion;
    Entry->FieldIndex = FieldIndex;

    if (!Entry->SectionName || !Entry->DriverDescription || !Entry->ProviderName ||
        !Entry->ManufacturerName || !Entry->MatchingId)
    {
        free(Entry->SectionName);
        free(Entry->DriverDescription);
        free(Entry->ProviderName);
        free(Entry->ManufacturerName);
        free(Entry->MatchingId);
        free(Entry);
        return -1;
    }

    if (*Tail)
        (*Tail)->Next = Entry;
    else
        *Head = Entry;
    *Tail = Entry;
    return 0;
}

static int
scan_inf(const char *Path, const PLATFORM_INFO *Platform, INF_CACHE_ENTRY **Head, INF_CACHE_ENTRY **Tail)
{
    HINF Inf;
    ULONG ErrorLine;
    PINFCONTEXT ManufacturerContext;
    WCHAR *ClassGuid = NULL;
    WCHAR *ProviderName = NULL;
    WCHAR *DriverVerValue = NULL;
    uint32_t DriverDateLow, DriverDateHigh;
    uint64_t DriverVersion;
    const char *FileName = path_basename(Path);

    if (InfHostOpenFile(&Inf, Path, 0, &ErrorLine) != 0)
        return 0;

    if (get_line_text(Inf, Version, ClassGUID, &ClassGuid) != 0 ||
        get_line_text(Inf, Version, Provider, &ProviderName) != 0)
    {
        InfHostCloseFile(Inf);
        free(ClassGuid);
        free(ProviderName);
        return 0;
    }

    if (get_line_text(Inf, Version, DriverVer, &DriverVerValue) != 0)
        DriverVerValue = NULL;
    parse_driver_ver(DriverVerValue, &DriverDateLow, &DriverDateHigh, &DriverVersion);

    if (InfHostFindFirstLine(Inf, Manufacturer, NULL, &ManufacturerContext) == 0)
    {
        do
        {
            WCHAR *ManufacturerName = NULL;
            WCHAR *ManufacturerSection = NULL;
            WCHAR *ActualSection = NULL;
            PINFCONTEXT DeviceContext;

            if (get_string_field(ManufacturerContext, 0, &ManufacturerName) != 0 ||
                get_string_field(ManufacturerContext, 1, &ManufacturerSection) != 0 ||
                find_actual_section(Inf, ManufacturerSection, Platform, &ActualSection) != 0)
            {
                free(ManufacturerName);
                free(ManufacturerSection);
                free(ActualSection);
                continue;
            }

            if (InfHostFindFirstLine(Inf, ActualSection, NULL, &DeviceContext) == 0)
            {
                do
                {
                    LONG FieldCount = InfHostGetFieldCount(DeviceContext);
                    WCHAR *SectionName = NULL;
                    WCHAR *DriverDescription = NULL;
                    LONG FieldIndex;

                    if (get_string_field(DeviceContext, 1, &SectionName) != 0 ||
                        get_string_field(DeviceContext, 0, &DriverDescription) != 0)
                    {
                        free(SectionName);
                        free(DriverDescription);
                        continue;
                    }

                    for (FieldIndex = 2; FieldIndex <= FieldCount; ++FieldIndex)
                    {
                        WCHAR *MatchingId = NULL;

                        if (get_string_field(DeviceContext, FieldIndex, &MatchingId) == 0)
                        {
                            add_entry(Head,
                                      Tail,
                                      FileName,
                                      SectionName,
                                      DriverDescription,
                                      ProviderName,
                                      ManufacturerName,
                                      MatchingId,
                                      ClassGuid,
                                      DriverDateLow,
                                      DriverDateHigh,
                                      DriverVersion,
                                      (unsigned)FieldIndex);
                            free(MatchingId);
                        }
                    }

                    free(SectionName);
                    free(DriverDescription);
                } while (InfHostFindNextLine(DeviceContext, DeviceContext) == 0);

                InfHostFreeContext(DeviceContext);
            }

            free(ManufacturerName);
            free(ManufacturerSection);
            free(ActualSection);
        } while (InfHostFindNextLine(ManufacturerContext, ManufacturerContext) == 0);

        InfHostFreeContext(ManufacturerContext);
    }

    free(ClassGuid);
    free(ProviderName);
    free(DriverVerValue);
    InfHostCloseFile(Inf);
    return 0;
}

static void
write_wchar(FILE *File, WCHAR Character)
{
    fputc((unsigned char)(Character & 0xff), File);
    fputc((unsigned char)((Character >> 8) & 0xff), File);
}

static void
write_wcs(FILE *File, const WCHAR *String)
{
    while (*String)
        write_wchar(File, *String++);
}

static void
write_ascii(FILE *File, const char *String)
{
    while (*String)
        write_wchar(File, (unsigned char)*String++);
}

static void
write_hex32(FILE *File, uint32_t Value)
{
    char Buffer[9];
    snprintf(Buffer, sizeof(Buffer), "%08x", Value);
    write_ascii(File, Buffer);
}

static void
write_hex64(FILE *File, uint64_t Value)
{
    char Buffer[17];
    snprintf(Buffer, sizeof(Buffer), "%016llx", (unsigned long long)Value);
    write_ascii(File, Buffer);
}

static void
write_entry(FILE *File, const INF_CACHE_ENTRY *Entry)
{
    write_ascii(File, Entry->FileName);
    write_wchar(File, '\t');
    write_wcs(File, Entry->SectionName);
    write_wchar(File, '\t');
    write_wcs(File, Entry->DriverDescription);
    write_wchar(File, '\t');
    write_wcs(File, Entry->ProviderName);
    write_wchar(File, '\t');
    write_wcs(File, Entry->ManufacturerName);
    write_wchar(File, '\t');
    write_wcs(File, Entry->ClassGuid);
    write_wchar(File, '\t');
    write_hex32(File, Entry->DriverDateLow);
    write_wchar(File, '\t');
    write_hex32(File, Entry->DriverDateHigh);
    write_wchar(File, '\t');
    write_hex64(File, Entry->DriverVersion);
    write_wchar(File, '\t');
    write_hex32(File, Entry->FieldIndex);
    write_wchar(File, '\t');
    write_wcs(File, Entry->MatchingId);
    write_wcs(File, (const WCHAR[]){'\r','\n',0});
}

static int
write_cache(const char *Path, const INF_CACHE_ENTRY *Head)
{
    FILE *File = fopen(Path, "wb");
    const INF_CACHE_ENTRY *Entry;

    if (!File)
        return -1;

    write_wchar(File, 0xfeff);
    write_wcs(File, Header);

    for (Entry = Head; Entry; Entry = Entry->Next)
        write_entry(File, Entry);

    fclose(File);
    return 0;
}

static const WCHAR *
architecture_name(const char *Architecture)
{
    static const WCHAR X86[] = {'x','8','6',0};
    static const WCHAR Amd64[] = {'A','M','D','6','4',0};
    static const WCHAR Arm[] = {'A','R','M',0};
    static const WCHAR Arm64[] = {'A','R','M','6','4',0};

    if (strcmp(Architecture, "i386") == 0 || strcmp(Architecture, "x86") == 0)
        return X86;
    if (strcmp(Architecture, "amd64") == 0)
        return Amd64;
    if (strcmp(Architecture, "arm") == 0)
        return Arm;
    if (strcmp(Architecture, "arm64") == 0)
        return Arm64;
    return (const WCHAR[]){0};
}

int
main(int argc, char **argv)
{
    PLATFORM_INFO Platform;
    INF_CACHE_ENTRY *Head = NULL;
    INF_CACHE_ENTRY *Tail = NULL;
    int i;

    if (argc < 6)
    {
        fprintf(stderr, "usage: infcache <output> <arch> <major> <minor> <inf>...\n");
        return 1;
    }

    Platform.Architecture = architecture_name(argv[2]);
    Platform.MajorVersion = (unsigned)strtoul(argv[3], NULL, 0);
    Platform.MinorVersion = (unsigned)strtoul(argv[4], NULL, 0);

    for (i = 5; i < argc; ++i)
        scan_inf(argv[i], &Platform, &Head, &Tail);

    if (write_cache(argv[1], Head) != 0)
    {
        fprintf(stderr, "infcache: failed to write %s\n", argv[1]);
        return 1;
    }

    return 0;
}
