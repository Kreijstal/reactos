/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Kernel-Mode Test Suite user-mode test list
 */

#include <kmt_test.h>

KMT_TESTFUNC Test_CcCopyRead;
KMT_TESTFUNC Test_CcCopyWrite;
KMT_TESTFUNC Test_CcLazyWriteRace;
KMT_TESTFUNC Test_CcMapData;
KMT_TESTFUNC Test_CcPinMappedData;
KMT_TESTFUNC Test_CcPinRead;
KMT_TESTFUNC Test_CcSetFileSizes;
KMT_TESTFUNC Test_Cng;
KMT_TESTFUNC Test_Example;
KMT_TESTFUNC Test_FileAttributes;
KMT_TESTFUNC Test_FindFile;
KMT_TESTFUNC Test_FltMgrLoad;
KMT_TESTFUNC Test_FltMgrReg;
KMT_TESTFUNC Test_HidPDescription;
KMT_TESTFUNC Test_IoCreateFile;
KMT_TESTFUNC Test_IoDeviceObject;
KMT_TESTFUNC Test_IoReadWrite;
KMT_TESTFUNC Test_MmMapLockedPagesSpecifyCache;
KMT_TESTFUNC Test_MmPageoutSysView;
KMT_TESTFUNC Test_MmProcessChurnStress;
KMT_TESTFUNC Test_NtCreateSection;
KMT_TESTFUNC Test_NtfsDirIndex;
KMT_TESTFUNC Test_NtfsFormat;
KMT_TESTFUNC Test_NtfsChkdsk;
KMT_TESTFUNC Test_NtfsCompression;
KMT_TESTFUNC Test_NtfsCreateSemantics;
KMT_TESTFUNC Test_NtfsDataPath;
KMT_TESTFUNC Test_NtfsEa;
KMT_TESTFUNC Test_NtfsFsctl;
KMT_TESTFUNC Test_NtfsInfoClasses;
KMT_TESTFUNC Test_NtfsLinks;
KMT_TESTFUNC Test_NtfsObjectId;
KMT_TESTFUNC Test_NtfsQuota;
KMT_TESTFUNC Test_NtfsReparse;
KMT_TESTFUNC Test_NtfsSecurity;
KMT_TESTFUNC Test_NtfsSparse;
KMT_TESTFUNC Test_NtfsStreams;
KMT_TESTFUNC Test_NtfsUsn;
KMT_TESTFUNC Test_NtfsVolumeControl;
KMT_TESTFUNC Test_NtfsVolumeDirty;
KMT_TESTFUNC Test_NtSystemDebugControl;
KMT_TESTFUNC Test_PoIrp;
KMT_TESTFUNC Test_RtlAvlTree;
KMT_TESTFUNC Test_RtlCaptureContext;
KMT_TESTFUNC Test_RtlException;
KMT_TESTFUNC Test_RtlIntSafe;
KMT_TESTFUNC Test_RtlMemory;
KMT_TESTFUNC Test_RtlRegistry;
KMT_TESTFUNC Test_RtlSplayTree;
KMT_TESTFUNC Test_RtlStack;
KMT_TESTFUNC Test_RtlStrSafe;
KMT_TESTFUNC Test_RtlUnicodeString;
KMT_TESTFUNC Test_TcpIpIoctl;
KMT_TESTFUNC Test_TcpIpTdi;
KMT_TESTFUNC Test_TcpIpConnect;

/* tests with a leading '-' will not be listed */
const KMT_TEST TestList[] =
{
    { "-CcCopyRead",                   Test_CcCopyRead },   // TODO: Crashes on TestWHS
    { "-CcCopyWrite",                  Test_CcCopyWrite },  // TODO: Crashes on TestWHS
    { "-CcLazyWriteRace",              Test_CcLazyWriteRace }, // Reproduces ASSERT(Vacb->Dirty) lazy-writer/CcFlushCache race; bugchecks on a buggy kernel
    { "-CcMapData",                    Test_CcMapData },
    { "-CcPinMappedData",              Test_CcPinMappedData },
    { "-CcPinRead",                    Test_CcPinRead },
    { "-CcSetFileSizes",               Test_CcSetFileSizes },
    { "Cng",                          Test_Cng },
    { "-Example",                     Test_Example },
    { "FileAttributes",               Test_FileAttributes },
    { "FindFile",                     Test_FindFile },
    { "-FltMgrLoad",                  Test_FltMgrLoad }, // TODO: WIP/untested/crashes.
    { "-FltMgrReg",                   Test_FltMgrReg }, // TODO: WIP/untested/crashes.
    { "HidPDescription",              Test_HidPDescription },
    { "IoCreateFile",                 Test_IoCreateFile },
    { "IoDeviceObject",               Test_IoDeviceObject },
    { "IoReadWrite",                  Test_IoReadWrite },
    { "MmMapLockedPagesSpecifyCache", Test_MmMapLockedPagesSpecifyCache },
    { "MmPageoutSysView",             Test_MmPageoutSysView },
    { "MmProcessChurnStress",         Test_MmProcessChurnStress },
    { "NtCreateSection",              Test_NtCreateSection },
    { "NtfsDirIndex",                 Test_NtfsDirIndex },
    { "NtfsFormat",                   Test_NtfsFormat },
    { "NtfsChkdsk",                   Test_NtfsChkdsk },
    { "NtfsCompression",               Test_NtfsCompression },
    { "NtfsCreateSemantics",           Test_NtfsCreateSemantics },
    { "NtfsDataPath",                  Test_NtfsDataPath },
    { "NtfsEa",                        Test_NtfsEa },
    { "NtfsFsctl",                     Test_NtfsFsctl },
    { "NtfsInfoClasses",               Test_NtfsInfoClasses },
    { "NtfsLinks",                     Test_NtfsLinks },
    { "NtfsObjectId",                  Test_NtfsObjectId },
    { "NtfsQuota",                     Test_NtfsQuota },
    { "NtfsReparse",                   Test_NtfsReparse },
    { "NtfsSecurity",                  Test_NtfsSecurity },
    { "NtfsSparse",                    Test_NtfsSparse },
    { "NtfsStreams",                   Test_NtfsStreams },
    { "NtfsUsn",                       Test_NtfsUsn },
    { "NtfsVolumeControl",             Test_NtfsVolumeControl },
    { "NtfsVolumeDirty",               Test_NtfsVolumeDirty },
    { "NtSystemDebugControl",         Test_NtSystemDebugControl },
    { "PoIrp",                        Test_PoIrp },
    { "RtlAvlTree",                   Test_RtlAvlTree },
    { "RtlException",                 Test_RtlException },
    { "RtlIntSafe",                   Test_RtlIntSafe },
    { "RtlMemory",                    Test_RtlMemory },
    { "RtlRegistry",                  Test_RtlRegistry },
    { "RtlSplayTree",                 Test_RtlSplayTree },
    { "RtlStack",                     Test_RtlStack },
    { "RtlStrSafe",                   Test_RtlStrSafe },
    { "RtlUnicodeString",             Test_RtlUnicodeString },
    { "TcpIpTdi",                     Test_TcpIpTdi },
    { "TcpIpConnect",                 Test_TcpIpConnect },
#ifdef _M_AMD64
    { "RtlCaptureContextUM",          Test_RtlCaptureContext },
#endif
    { NULL,                           NULL },
};
