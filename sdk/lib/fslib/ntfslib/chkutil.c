/* NTFS checker record builders, derived from winmkntfs (GPL-2.0-or-later). */
#include "chkint.h"
#include "upcase_data.h"

static int runlen_bytes(LONGLONG val)
{
    int n;
    if (val == 0) return 0;
    if (val > 0) {
        for (n = 1; n < 8; n++) {
            if (val < (1LL << (n * 8 - 1))) return n;
        }
    } else {
        for (n = 1; n < 8; n++) {
            if (val >= -(1LL << (n * 8 - 1))) return n;
        }
    }
    return 8;
}

static int runlen_bytes_unsigned(ULONGLONG val)
{
    int n;
    if (val == 0) return 1; /* length field must be at least 1 byte */
    for (n = 1; n < 8; n++) {
        if (val < (1ULL << (n * 8))) return n;
    }
    return 8;
}

int NtfsChkEncodeRun(UCHAR *dst, ULONGLONG length, LONGLONG lcn_offset)
{
    int len_bytes = runlen_bytes_unsigned(length);
    /* A real (non-sparse) run must always emit at least one offset byte:
     * zero offset bytes means "sparse run" to every NTFS reader.  Hit by
     * $Boot, whose first run starts at LCN 0 (delta 0). */
    int off_bytes = runlen_bytes(lcn_offset);
    int i;

    if (off_bytes == 0)
        off_bytes = 1;

    dst[0] = (UCHAR)((off_bytes << 4) | len_bytes);
    for (i = 0; i < len_bytes; i++)
        dst[1 + i] = (UCHAR)(length >> (i * 8));
    for (i = 0; i < off_bytes; i++)
        dst[1 + len_bytes + i] = (UCHAR)(lcn_offset >> (i * 8));
    return 1 + len_bytes + off_bytes;
}

void NtfsChkInitializeRecord(FILE_RECORD_HEADER *rec, ULONG rec_size, ULONG sector_size,
                              USHORT seq_num, USHORT flags, ULONG mft_num)
{
    ULONG usa_offset, usa_size;

    memset(rec, 0, rec_size);
    rec->Magic = NRH_FILE_TYPE;

    /* Update sequence: right after the header */
    usa_offset = sizeof(FILE_RECORD_HEADER);
    usa_size = 1 + (rec_size / sector_size);  /* 1 USN + N array entries */
    rec->UpdateSeqOffset = (USHORT)usa_offset;
    rec->UpdateSeqSize = (USHORT)usa_size;

    /* Set USN to 1 */
    USHORT *usa = (USHORT *)((UCHAR *)rec + usa_offset);
    usa[0] = 1;  /* USN value */

    rec->SequenceNumber = seq_num;
    rec->LinkCount = 0;
    rec->AttributeOffset = (USHORT)NTFS_ALIGN_UP(usa_offset + usa_size * 2, 8);
    rec->Flags = flags;
    rec->BytesAllocated = rec_size;
    rec->MftRecordNumber = mft_num;
    rec->NextAttributeId = 0;
}

static UCHAR *get_attr_pos(FILE_RECORD_HEADER *rec)
{
    UCHAR *base = (UCHAR *)rec;
    UCHAR *pos = base + rec->AttributeOffset;

    /* Walk existing attributes to find the end */
    while (pos < base + rec->BytesAllocated - 8) {
        ATTR_RECORD *a = (ATTR_RECORD *)pos;
        if (a->Type == AT_END || a->Type == 0 || a->Length == 0)
            break;
        pos += a->Length;
    }
    return pos;
}

UCHAR *NtfsChkAddResidentAttribute(USHORT *next_instance, FILE_RECORD_HEADER *rec,
                              ULONG type, const WCHAR *name, ULONG name_len,
                              const void *value, ULONG value_len,
                              UCHAR resident_flags)
{
    UCHAR *pos = get_attr_pos(rec);
    ATTR_RECORD *a = (ATTR_RECORD *)pos;
    ULONG name_off, value_off, total_len;

    /* Size it before writing anything: header+name+value plus the 8-byte
     * AT_END tail must stay inside the record */
    name_off = 0x18;
    value_off = name_len > 0
        ? NTFS_ALIGN_UP(name_off + name_len * sizeof(WCHAR), 4)
        : name_off;
    total_len = NTFS_ALIGN_UP(value_off + value_len, 8);
    if (pos + total_len + 8 > (UCHAR *)rec + rec->BytesAllocated)
        return NULL;

    /* Zero only the header, not the AT_END marker that follows */
    memset(a, 0, total_len);
    a->Type = type;
    a->NonResident = 0;
    a->NameLength = (UCHAR)name_len;
    a->Flags = 0;
    a->Instance = (*next_instance)++;

    a->NameOffset = (USHORT)name_off;
    if (name_len > 0)
        memcpy(pos + name_off, name, name_len * sizeof(WCHAR));

    a->Resident.ValueOffset = (USHORT)value_off;
    a->Resident.ValueLength = value_len;
    a->Resident.ResidentFlags = resident_flags;

    if (value && value_len > 0)
        memcpy(pos + value_off, value, value_len);

    total_len = NTFS_ALIGN_UP(value_off + value_len, 8);
    a->Length = total_len;

    /* Write AT_END marker after this attribute */
    ULONG *end = (ULONG *)(pos + total_len);
    *end = AT_END;

    rec->BytesInUse = (ULONG)(pos + total_len + 8 - (UCHAR *)rec); /* +8 for AT_END + padding */
    return pos + total_len;
}

UCHAR *NtfsChkAddNonresidentAttribute(USHORT *next_instance, ULONG cluster_size,
                                 FILE_RECORD_HEADER *rec,
                                 ULONG type, const WCHAR *name, ULONG name_len,
                                 ULONGLONG start_lcn, ULONGLONG num_clusters,
                                 ULONGLONG data_size, ULONGLONG initialized_size)
{
    UCHAR *pos = get_attr_pos(rec);
    ATTR_RECORD *a = (ATTR_RECORD *)pos;
    ULONG name_off, runs_off, total_len;
    UCHAR run_buf[18];
    int run_len;

    /* Size it before writing anything (run encoded into run_buf first) */
    name_off = 0x40;
    runs_off = name_len > 0
        ? NTFS_ALIGN_UP(name_off + name_len * sizeof(WCHAR), 4)
        : name_off;
    run_len = NtfsChkEncodeRun(run_buf, num_clusters, (LONGLONG)start_lcn);
    total_len = NTFS_ALIGN_UP(runs_off + run_len + 1, 8);
    if (pos + total_len + 8 > (UCHAR *)rec + rec->BytesAllocated)
        return NULL;

    memset(a, 0, total_len);
    a->Type = type;
    a->NonResident = 1;
    a->NameLength = (UCHAR)name_len;
    a->Flags = 0;
    a->Instance = (*next_instance)++;

    a->NameOffset = (USHORT)name_off;
    if (name_len > 0)
        memcpy(pos + name_off, name, name_len * sizeof(WCHAR));

    a->NR.LowestVcn = 0;
    a->NR.HighestVcn = num_clusters - 1;
    a->NR.MappingPairsOffset = (USHORT)runs_off;
    a->NR.AllocatedSize = (LONGLONG)(num_clusters * cluster_size);
    a->NR.DataSize = (LONGLONG)data_size;
    a->NR.InitializedSize = (LONGLONG)initialized_size;

    /* Single data run, encoded above */
    memcpy(pos + runs_off, run_buf, run_len);
    pos[runs_off + run_len] = 0; /* Terminator */

    a->Length = total_len;

    /* AT_END marker */
    ULONG *end = (ULONG *)(pos + total_len);
    *end = AT_END;

    rec->BytesInUse = (ULONG)(pos + total_len + 8 - (UCHAR *)rec);
    return pos + total_len;
}

void NtfsChkBuildFileName(FILE_NAME_ATTR *fn, ULONGLONG parent_ref,
                         const WCHAR *name, UCHAR name_len, UCHAR name_type,
                         ULONG file_attrs, ULONGLONG alloc_size,
                         ULONGLONG data_size, ULONGLONG time_val)
{
    /* parent_ref: complete reference (MFT number low 48 bits, seq high 16) */
    fn->ParentDirectory = parent_ref;
    fn->CreationTime = time_val;
    fn->ModificationTime = time_val;
    fn->MftModificationTime = time_val;
    fn->AccessTime = time_val;
    fn->AllocatedSize = alloc_size;
    fn->DataSize = data_size;
    fn->FileAttributes = file_attrs;
    fn->Extended.EaInfo.PackedEaSize = 0;
    fn->Extended.EaInfo.Reserved = 0;
    fn->FileNameLength = name_len;
    fn->FileNameType = name_type;
    memcpy(fn->FileName, name, name_len * sizeof(WCHAR));
}

ULONGLONG NtfsChkMftReference(ULONG record, USHORT seq)
{
    return (ULONGLONG)record | ((ULONGLONG)seq << 48);
}

void NtfsChkBuildUpcase(WCHAR *Table, ULONG Entries)
{
    ULONG i;

    for (i = 0; i < Entries; ++i)
        Table[i] = (WCHAR)(NtfsUpCaseTable[2 * i] |
                           (NtfsUpCaseTable[2 * i + 1] << 8));
}
