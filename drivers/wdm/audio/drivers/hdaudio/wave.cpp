/*
 * PROJECT:     ReactOS HD Audio codec function driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WaveCyclic miniport rendering through the HDAUDIO bus DMA engine
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "driver.h"

#define NDEBUG
#include <debug.h>

/* wave filter pins */
#define WAVE_PIN_RENDER_SINK    0
#define WAVE_PIN_RENDER_BRIDGE  1

/* buffer layout: 4 chunks of 10ms each */
#define WAVE_NOTIFICATION_MS    10
#define WAVE_NUM_CHUNKS         4

#define WAVE_MIN_SAMPLE_RATE    44100
#define WAVE_MAX_SAMPLE_RATE    48000
#define WAVE_DEFAULT_RATE       48000

class CMiniportWaveCyclicHDA;

/*****************************************************************************
 * CMiniportWaveCyclicStreamHDA
 *****************************************************************************
 * Render stream. The object also implements IDmaChannel: the cyclic buffer
 * is not a system DMA adapter buffer but the HDA controller BDL buffer
 * allocated through the HDAUDIO bus interface, so the stream itself hands
 * out its mapping. Note that portcls' WaveCyclic pin treats the DMA channel
 * as an aggregate of the stream (it never releases it separately), therefore
 * NewStream returns this object as IDmaChannel without an additional
 * reference; the buffer is torn down in the stream destructor.
 */
class CMiniportWaveCyclicStreamHDA : public IMiniportWaveCyclicStream,
                                     public IDmaChannel,
                                     public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveCyclicStreamHDA);
    ~CMiniportWaveCyclicStreamHDA();

    /* IMiniportWaveCyclicStream */
    STDMETHODIMP_(NTSTATUS) SetFormat(IN PKSDATAFORMAT DataFormat);
    STDMETHODIMP_(ULONG) SetNotificationFreq(IN ULONG Interval, OUT PULONG FrameSize);
    STDMETHODIMP_(NTSTATUS) SetState(IN KSSTATE State);
    STDMETHODIMP_(NTSTATUS) GetPosition(OUT PULONG Position);
    STDMETHODIMP_(NTSTATUS) NormalizePhysicalPosition(IN OUT PLONGLONG PhysicalPosition);
    STDMETHODIMP_(void) Silence(IN PVOID Buffer, IN ULONG ByteCount);

    /* IDmaChannel */
    STDMETHODIMP_(NTSTATUS) AllocateBuffer(IN ULONG BufferSize,
                                           IN PPHYSICAL_ADDRESS PhysicalAddressConstraint OPTIONAL);
    STDMETHODIMP_(void) FreeBuffer(void);
    STDMETHODIMP_(ULONG) TransferCount(void);
    STDMETHODIMP_(ULONG) MaximumBufferSize(void);
    STDMETHODIMP_(ULONG) AllocatedBufferSize(void);
    STDMETHODIMP_(ULONG) BufferSize(void);
    STDMETHODIMP_(void) SetBufferSize(IN ULONG BufferSize);
    STDMETHODIMP_(PVOID) SystemAddress(void);
#if defined(__cplusplus) && !defined(_MSC_VER)
    STDMETHODIMP_(PHYSICAL_ADDRESS*) PhysicalAddress(PHYSICAL_ADDRESS *pRet);
#else
    STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress(void);
#endif
    STDMETHODIMP_(PADAPTER_OBJECT) GetAdapterObject(void);
    STDMETHODIMP_(void) CopyTo(IN PVOID Destination, IN PVOID Source, IN ULONG ByteCount);
    STDMETHODIMP_(void) CopyFrom(IN PVOID Destination, IN PVOID Source, IN ULONG ByteCount);

    NTSTATUS Init(IN CMiniportWaveCyclicHDA *Miniport, IN PKSDATAFORMAT DataFormat);

    PSERVICEGROUP m_ServiceGroup;

private:
    static VOID NTAPI TimerDpcRoutine(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2);
    NTSTATUS ParseFormat(IN PKSDATAFORMAT DataFormat, OUT PHDAUDIO_STREAM_FORMAT StreamFormat);
    void StopTimer(void);

    CMiniportWaveCyclicHDA *m_Miniport;
    PHDACODEC m_Codec;
    PHDAUDIO_BUS_INTERFACE m_BusInterface;
    PPORTWAVECYCLIC m_Port;

    HANDLE m_DmaHandle;
    PMDL m_Mdl;
    PVOID m_SystemAddress;
    ULONG m_BufferSize;
    SIZE_T m_AllocatedSize;
    PULONG m_PositionRegister;
    UCHAR m_StreamId;
    ULONG m_FifoSize;

    ULONG m_SampleRate;
    ULONG m_BlockAlign;

    KSSTATE m_State;
    ULONG m_Interval;
    KTIMER m_Timer;
    KDPC m_Dpc;
    BOOLEAN m_TimerActive;
};

/*****************************************************************************
 * CMiniportWaveCyclicHDA
 */
class CMiniportWaveCyclicHDA : public IMiniportWaveCyclic,
                               public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveCyclicHDA);
    ~CMiniportWaveCyclicHDA();

    /* IMiniport */
    STDMETHODIMP_(NTSTATUS) GetDescription(OUT PPCFILTER_DESCRIPTOR *Description);
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(IN ULONG PinId,
                                                  IN PKSDATARANGE DataRange,
                                                  IN PKSDATARANGE MatchingDataRange,
                                                  IN ULONG OutputBufferLength,
                                                  OUT PVOID ResultantFormat OPTIONAL,
                                                  OUT PULONG ResultantFormatLength);

    /* IMiniportWaveCyclic */
    STDMETHODIMP_(NTSTATUS) Init(IN PUNKNOWN UnknownAdapter,
                                 IN PRESOURCELIST ResourceList,
                                 IN PPORTWAVECYCLIC Port);
    STDMETHODIMP_(NTSTATUS) NewStream(OUT PMINIPORTWAVECYCLICSTREAM *Stream,
                                      IN PUNKNOWN OuterUnknown OPTIONAL,
                                      IN POOL_TYPE PoolType,
                                      IN ULONG Pin,
                                      IN BOOLEAN Capture,
                                      IN PKSDATAFORMAT DataFormat,
                                      OUT PDMACHANNEL *DmaChannel,
                                      OUT PSERVICEGROUP *ServiceGroup);

    friend class CMiniportWaveCyclicStreamHDA;

private:
    PPORTWAVECYCLIC m_Port;
    PHDACODEC m_Codec;
    BOOLEAN m_StreamActive;
};

/*****************************************************************************
 * Filter data ranges
 */
static KSDATARANGE_AUDIO WavePinDataRangePcm =
{
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
        {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)},
        {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}
    },
    2,                      /* MaximumChannels */
    16,                     /* MinimumBitsPerSample */
    16,                     /* MaximumBitsPerSample */
    WAVE_MIN_SAMPLE_RATE,   /* MinimumSampleFrequency */
    WAVE_MAX_SAMPLE_RATE    /* MaximumSampleFrequency */
};

static PKSDATARANGE WavePinDataRangePointersPcm[] =
{
    (PKSDATARANGE)&WavePinDataRangePcm
};

static KSDATARANGE WavePinDataRangeBridge =
{
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
    {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
    {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};

static PKSDATARANGE WavePinDataRangePointersBridge[] =
{
    &WavePinDataRangeBridge
};

static PCPIN_DESCRIPTOR WavePins[] =
{
    /* WAVE_PIN_RENDER_SINK */
    {
        1, 1, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(WavePinDataRangePointersPcm),
            WavePinDataRangePointersPcm,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            (GUID *)&KSCATEGORY_AUDIO,
            NULL,
            {0}
        }
    },
    /* WAVE_PIN_RENDER_BRIDGE */
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(WavePinDataRangePointersBridge),
            WavePinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSCATEGORY_AUDIO,
            NULL,
            {0}
        }
    }
};

static PCNODE_DESCRIPTOR WaveNodes[] =
{
    { 0, NULL, &KSNODETYPE_DAC, NULL }
};

static PCCONNECTION_DESCRIPTOR WaveConnections[] =
{
    { PCFILTER_NODE, WAVE_PIN_RENDER_SINK, 0, 1 },
    { 0, 0, PCFILTER_NODE, WAVE_PIN_RENDER_BRIDGE }
};

static GUID WaveCategories[] =
{
    {STATICGUIDOF(KSCATEGORY_AUDIO)},
    {STATICGUIDOF(KSCATEGORY_RENDER)}
};

static PCFILTER_DESCRIPTOR WaveFilterDescriptor =
{
    0,                                  /* Version */
    NULL,                               /* AutomationTable */
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(WavePins),
    WavePins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(WaveNodes),
    WaveNodes,
    SIZEOF_ARRAY(WaveConnections),
    WaveConnections,
    SIZEOF_ARRAY(WaveCategories),
    WaveCategories
};

/*****************************************************************************
 * CreateMiniportWaveCyclicHDA
 */
NTSTATUS
CreateMiniportWaveCyclicHDA(
    OUT PUNKNOWN *Unknown,
    IN REFCLSID ClassId,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ClassId);

    STD_CREATE_BODY_WITH_TAG_(CMiniportWaveCyclicHDA, Unknown, UnknownOuter,
                              PoolType, HDA_POOL_TAG, PUNKNOWN);
}

CMiniportWaveCyclicHDA::~CMiniportWaveCyclicHDA()
{
    PAGED_CODE();

    if (m_Codec)
    {
        m_Codec->Release();
        m_Codec = NULL;
    }

    if (m_Port)
    {
        m_Port->Release();
        m_Port = NULL;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicHDA::NonDelegatingQueryInterface(
    IN REFIID Interface,
    OUT PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVECYCLIC(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveCyclic))
    {
        *Object = PVOID(PMINIPORTWAVECYCLIC(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicHDA::Init(
    IN PUNKNOWN UnknownAdapter,
    IN PRESOURCELIST ResourceList,
    IN PPORTWAVECYCLIC Port)
{
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourceList);

    if (!UnknownAdapter || !Port)
        return STATUS_INVALID_PARAMETER;

    Status = UnknownAdapter->QueryInterface(IID_IHDACodec, (PVOID *)&m_Codec);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: wave miniport: no IHDACodec interface\n");
        return Status;
    }

    m_Port = Port;
    m_Port->AddRef();

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicHDA::GetDescription(
    OUT PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();

    *Description = &WaveFilterDescriptor;
    return STATUS_SUCCESS;
}

/*****************************************************************************
 * CMiniportWaveCyclicHDA::DataRangeIntersection
 *****************************************************************************
 * Intersects a requested data range with our PCM range and produces a
 * KSDATAFORMAT_WAVEFORMATEX. ReactOS portcls has no default intersection
 * handler, so this must be fully implemented here.
 */
STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicHDA::DataRangeIntersection(
    IN ULONG PinId,
    IN PKSDATARANGE DataRange,
    IN PKSDATARANGE MatchingDataRange,
    IN ULONG OutputBufferLength,
    OUT PVOID ResultantFormat OPTIONAL,
    OUT PULONG ResultantFormatLength)
{
    PKSDATARANGE_AUDIO AudioRange;
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;
    ULONG SampleRate;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(MatchingDataRange);

    if (PinId != WAVE_PIN_RENDER_SINK)
        return STATUS_NO_MATCH;

    if (!IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
        !IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }

    if (!IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
        !IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }

    if (!IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        !IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD))
    {
        return STATUS_NO_MATCH;
    }

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);

    if (OutputBufferLength == 0)
        return STATUS_BUFFER_OVERFLOW;

    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX))
        return STATUS_BUFFER_TOO_SMALL;

    SampleRate = WAVE_DEFAULT_RATE;

    if (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
    {
        AudioRange = (PKSDATARANGE_AUDIO)DataRange;

        if (AudioRange->MaximumChannels < 2 ||
            AudioRange->MinimumBitsPerSample > 16 ||
            AudioRange->MaximumBitsPerSample < 16 ||
            AudioRange->MinimumSampleFrequency > WAVE_MAX_SAMPLE_RATE ||
            AudioRange->MaximumSampleFrequency < WAVE_MIN_SAMPLE_RATE)
        {
            return STATUS_NO_MATCH;
        }

        if (AudioRange->MaximumSampleFrequency < SampleRate)
            SampleRate = AudioRange->MaximumSampleFrequency;
        if (AudioRange->MinimumSampleFrequency > SampleRate)
            SampleRate = AudioRange->MinimumSampleFrequency;
    }

    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    RtlZeroMemory(WaveFormat, sizeof(KSDATAFORMAT_WAVEFORMATEX));

    WaveFormat->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    WaveFormat->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    WaveFormat->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    WaveFormat->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    WaveFormat->DataFormat.SampleSize = 4;

    WaveFormat->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    WaveFormat->WaveFormatEx.nChannels = 2;
    WaveFormat->WaveFormatEx.nSamplesPerSec = SampleRate;
    WaveFormat->WaveFormatEx.wBitsPerSample = 16;
    WaveFormat->WaveFormatEx.nBlockAlign = 4;
    WaveFormat->WaveFormatEx.nAvgBytesPerSec = SampleRate * 4;
    WaveFormat->WaveFormatEx.cbSize = 0;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicHDA::NewStream(
    OUT PMINIPORTWAVECYCLICSTREAM *Stream,
    IN PUNKNOWN OuterUnknown OPTIONAL,
    IN POOL_TYPE PoolType,
    IN ULONG Pin,
    IN BOOLEAN Capture,
    IN PKSDATAFORMAT DataFormat,
    OUT PDMACHANNEL *DmaChannel,
    OUT PSERVICEGROUP *ServiceGroup)
{
    CMiniportWaveCyclicStreamHDA *NewStream;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(OuterUnknown);

    if (Pin != WAVE_PIN_RENDER_SINK || Capture)
        return STATUS_INVALID_PARAMETER;

    NewStream = new(PoolType, HDA_POOL_TAG) CMiniportWaveCyclicStreamHDA(NULL);
    if (!NewStream)
        return STATUS_INSUFFICIENT_RESOURCES;

    NewStream->AddRef();

    Status = NewStream->Init(this, DataFormat);
    if (!NT_SUCCESS(Status))
    {
        NewStream->Release();
        return Status;
    }

    *Stream = (PMINIPORTWAVECYCLICSTREAM)NewStream;

    /*
     * The DMA channel is the same object; portcls' pin never releases the
     * IDmaChannel pointer separately, so no extra reference is taken here.
     */
    *DmaChannel = (PDMACHANNEL)NewStream;

    /* the pin does not release the service group either, the stream owns it */
    *ServiceGroup = NewStream->m_ServiceGroup;

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * CMiniportWaveCyclicStreamHDA
 */
CMiniportWaveCyclicStreamHDA::~CMiniportWaveCyclicStreamHDA()
{
    PAGED_CODE();

    StopTimer();

    /* make sure no timer DPC is still running against this object */
    KeFlushQueuedDpcs();

    if (m_BusInterface && m_DmaHandle)
    {
        HANDLE Handle = m_DmaHandle;

        /* ensure the DMA engine is stopped before freeing */
        m_BusInterface->SetDmaEngineState(m_BusInterface->Context,
                                          StopState, 1, &Handle);

        if (m_Mdl)
        {
            m_BusInterface->FreeDmaBuffer(m_BusInterface->Context, Handle);
            m_Mdl = NULL;
        }

        m_BusInterface->FreeDmaEngine(m_BusInterface->Context, Handle);
        m_DmaHandle = NULL;
    }

    if (m_ServiceGroup)
    {
        m_ServiceGroup->Release();
        m_ServiceGroup = NULL;
    }

    if (m_Port)
    {
        m_Port->Release();
        m_Port = NULL;
    }

    if (m_Codec)
    {
        m_Codec->Release();
        m_Codec = NULL;
    }

    if (m_Miniport)
    {
        m_Miniport->m_StreamActive = FALSE;
        m_Miniport->Release();
        m_Miniport = NULL;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::NonDelegatingQueryInterface(
    IN REFIID Interface,
    OUT PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVECYCLICSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveCyclicStream))
    {
        *Object = PVOID(PMINIPORTWAVECYCLICSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IDmaChannel))
    {
        *Object = PVOID(PDMACHANNEL(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

NTSTATUS
CMiniportWaveCyclicStreamHDA::ParseFormat(
    IN PKSDATAFORMAT DataFormat,
    OUT PHDAUDIO_STREAM_FORMAT StreamFormat)
{
    PKSDATAFORMAT_WAVEFORMATEX WaveFormat;

    PAGED_CODE();

    if (DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
        return STATUS_INVALID_PARAMETER;

    if (!IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return STATUS_INVALID_PARAMETER;
    }

    WaveFormat = (PKSDATAFORMAT_WAVEFORMATEX)DataFormat;

    if (WaveFormat->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM ||
        WaveFormat->WaveFormatEx.wBitsPerSample != 16 ||
        WaveFormat->WaveFormatEx.nChannels != 2 ||
        WaveFormat->WaveFormatEx.nSamplesPerSec < WAVE_MIN_SAMPLE_RATE ||
        WaveFormat->WaveFormatEx.nSamplesPerSec > WAVE_MAX_SAMPLE_RATE)
    {
        DPRINT1("HDAUDIO: unsupported format: tag %u bits %u ch %u rate %lu\n",
                WaveFormat->WaveFormatEx.wFormatTag,
                WaveFormat->WaveFormatEx.wBitsPerSample,
                WaveFormat->WaveFormatEx.nChannels,
                WaveFormat->WaveFormatEx.nSamplesPerSec);
        return STATUS_INVALID_PARAMETER;
    }

    StreamFormat->SampleRate = WaveFormat->WaveFormatEx.nSamplesPerSec;
    StreamFormat->ValidBitsPerSample = 16;
    StreamFormat->ContainerSize = 16;
    StreamFormat->NumberOfChannels = 2;

    m_SampleRate = WaveFormat->WaveFormatEx.nSamplesPerSec;
    m_BlockAlign = WaveFormat->WaveFormatEx.nBlockAlign;

    return STATUS_SUCCESS;
}

NTSTATUS
CMiniportWaveCyclicStreamHDA::Init(
    IN CMiniportWaveCyclicHDA *Miniport,
    IN PKSDATAFORMAT DataFormat)
{
    HDAUDIO_STREAM_FORMAT StreamFormat;
    HDAUDIO_CONVERTER_FORMAT ConverterFormat;
    ULONG ChunkSize;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ParseFormat(DataFormat, &StreamFormat);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Miniport->m_StreamActive)
    {
        /* the underlying converter supports one stream at a time */
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_Miniport = Miniport;
    m_Miniport->AddRef();

    m_Codec = Miniport->m_Codec;
    m_Codec->AddRef();

    m_Port = Miniport->m_Port;
    m_Port->AddRef();

    m_BusInterface = m_Codec->GetBusInterface();
    if (!m_BusInterface)
        return STATUS_DEVICE_NOT_READY;

    m_State = KSSTATE_STOP;
    m_Interval = WAVE_NOTIFICATION_MS;

    KeInitializeTimerEx(&m_Timer, NotificationTimer);
    KeInitializeDpc(&m_Dpc, TimerDpcRoutine, this);

    Status = PcNewServiceGroup(&m_ServiceGroup, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = m_BusInterface->AllocateRenderDmaEngine(m_BusInterface->Context,
                                                     &StreamFormat,
                                                     FALSE,
                                                     &m_DmaHandle,
                                                     &ConverterFormat);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: AllocateRenderDmaEngine failed: 0x%lx\n", Status);
        m_DmaHandle = NULL;
        return Status;
    }

    /* cyclic buffer: WAVE_NUM_CHUNKS chunks of WAVE_NOTIFICATION_MS each */
    ChunkSize = (m_SampleRate * m_BlockAlign * WAVE_NOTIFICATION_MS) / 1000;
    ChunkSize -= (ChunkSize % m_BlockAlign);
    m_BufferSize = ChunkSize * WAVE_NUM_CHUNKS;

    Status = m_BusInterface->AllocateDmaBuffer(m_BusInterface->Context,
                                               m_DmaHandle,
                                               m_BufferSize,
                                               &m_Mdl,
                                               &m_AllocatedSize,
                                               &m_StreamId,
                                               &m_FifoSize);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: AllocateDmaBuffer failed: 0x%lx\n", Status);
        m_Mdl = NULL;
        return Status;
    }

    m_SystemAddress = MmGetSystemAddressForMdlSafe(m_Mdl, NormalPagePriority);
    if (!m_SystemAddress)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(m_SystemAddress, m_BufferSize);

    Status = m_BusInterface->GetLinkPositionRegister(m_BusInterface->Context,
                                                     m_DmaHandle,
                                                     &m_PositionRegister);
    if (!NT_SUCCESS(Status))
        return Status;

    /* bind the codec converter to the stream */
    Status = m_Codec->SetRenderStream(m_StreamId, ConverterFormat.ConverterFormat);
    if (!NT_SUCCESS(Status))
        return Status;

    Miniport->m_StreamActive = TRUE;

    DPRINT1("HDAUDIO: stream ready: tag %u rate %lu buffer %lu fifo %lu\n",
            m_StreamId, m_SampleRate, m_BufferSize, m_FifoSize);

    return STATUS_SUCCESS;
}

VOID
NTAPI
CMiniportWaveCyclicStreamHDA::TimerDpcRoutine(
    PKDPC Dpc,
    PVOID Context,
    PVOID Arg1,
    PVOID Arg2)
{
    CMiniportWaveCyclicStreamHDA *This = (CMiniportWaveCyclicStreamHDA *)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (This->m_Port && This->m_ServiceGroup)
        This->m_Port->Notify(This->m_ServiceGroup);
}

void
CMiniportWaveCyclicStreamHDA::StopTimer(void)
{
    if (m_TimerActive)
    {
        KeCancelTimer(&m_Timer);
        m_TimerActive = FALSE;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::SetFormat(
    IN PKSDATAFORMAT DataFormat)
{
    HDAUDIO_STREAM_FORMAT StreamFormat;
    HDAUDIO_CONVERTER_FORMAT ConverterFormat;
    NTSTATUS Status;

    PAGED_CODE();

    if (m_State == KSSTATE_RUN)
        return STATUS_INVALID_DEVICE_REQUEST;

    Status = ParseFormat(DataFormat, &StreamFormat);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = m_BusInterface->ChangeBandwidthAllocation(m_BusInterface->Context,
                                                       m_DmaHandle,
                                                       &StreamFormat,
                                                       &ConverterFormat);
    if (!NT_SUCCESS(Status))
        return Status;

    return m_Codec->SetRenderStream(m_StreamId, ConverterFormat.ConverterFormat);
}

STDMETHODIMP_(ULONG)
CMiniportWaveCyclicStreamHDA::SetNotificationFreq(
    IN ULONG Interval,
    OUT PULONG FrameSize)
{
    if (Interval != 0)
        m_Interval = Interval;

    *FrameSize = m_BufferSize / WAVE_NUM_CHUNKS;

    return m_Interval;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::SetState(
    IN KSSTATE State)
{
    HANDLE Handle = m_DmaHandle;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("HDAUDIO: stream SetState %u -> %u\n", m_State, State);

    if (State == m_State)
        return STATUS_SUCCESS;

    switch (State)
    {
        case KSSTATE_RUN:
        {
            LARGE_INTEGER DueTime;

            Status = m_BusInterface->SetDmaEngineState(m_BusInterface->Context,
                                                       RunState, 1, &Handle);
            if (!NT_SUCCESS(Status))
                break;

            DueTime.QuadPart = -10000LL * m_Interval;
            KeSetTimerEx(&m_Timer, DueTime, m_Interval, &m_Dpc);
            m_TimerActive = TRUE;
            break;
        }

        case KSSTATE_ACQUIRE:
        case KSSTATE_PAUSE:
            StopTimer();
            Status = m_BusInterface->SetDmaEngineState(m_BusInterface->Context,
                                                       PauseState, 1, &Handle);
            break;

        case KSSTATE_STOP:
            StopTimer();
            Status = m_BusInterface->SetDmaEngineState(m_BusInterface->Context,
                                                       StopState, 1, &Handle);
            if (NT_SUCCESS(Status))
            {
                /* reset the DMA engine so the link position restarts at 0 */
                m_BusInterface->SetDmaEngineState(m_BusInterface->Context,
                                                  ResetState, 1, &Handle);
            }
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(Status))
        m_State = State;

    return Status;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::GetPosition(
    OUT PULONG Position)
{
    if (m_State == KSSTATE_STOP || !m_PositionRegister || m_BufferSize == 0)
    {
        *Position = 0;
        return STATUS_SUCCESS;
    }

    *Position = (*(volatile ULONG *)m_PositionRegister) % m_BufferSize;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::NormalizePhysicalPosition(
    IN OUT PLONGLONG PhysicalPosition)
{
    ULONG BytesPerSecond = m_SampleRate * m_BlockAlign;

    if (BytesPerSecond == 0)
        return STATUS_DEVICE_NOT_READY;

    *PhysicalPosition = (*PhysicalPosition * 10000000LL) / BytesPerSecond;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CMiniportWaveCyclicStreamHDA::Silence(
    IN PVOID Buffer,
    IN ULONG ByteCount)
{
    RtlZeroMemory(Buffer, ByteCount);
}

/*****************************************************************************
 * IDmaChannel: exposes the bus-allocated cyclic buffer.
 * The buffer is allocated once at stream creation; AllocateBuffer only
 * validates the requested size against it.
 */
STDMETHODIMP_(NTSTATUS)
CMiniportWaveCyclicStreamHDA::AllocateBuffer(
    IN ULONG BufferSize,
    IN PPHYSICAL_ADDRESS PhysicalAddressConstraint OPTIONAL)
{
    UNREFERENCED_PARAMETER(PhysicalAddressConstraint);

    if (BufferSize > m_AllocatedSize)
        return STATUS_INSUFFICIENT_RESOURCES;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(void)
CMiniportWaveCyclicStreamHDA::FreeBuffer(void)
{
}

STDMETHODIMP_(ULONG)
CMiniportWaveCyclicStreamHDA::TransferCount(void)
{
    return m_BufferSize;
}

STDMETHODIMP_(ULONG)
CMiniportWaveCyclicStreamHDA::MaximumBufferSize(void)
{
    return (ULONG)m_AllocatedSize;
}

STDMETHODIMP_(ULONG)
CMiniportWaveCyclicStreamHDA::AllocatedBufferSize(void)
{
    return (ULONG)m_AllocatedSize;
}

STDMETHODIMP_(ULONG)
CMiniportWaveCyclicStreamHDA::BufferSize(void)
{
    return m_BufferSize;
}

STDMETHODIMP_(void)
CMiniportWaveCyclicStreamHDA::SetBufferSize(
    IN ULONG BufferSize)
{
    if (BufferSize <= m_AllocatedSize)
        m_BufferSize = BufferSize;
}

STDMETHODIMP_(PVOID)
CMiniportWaveCyclicStreamHDA::SystemAddress(void)
{
    return m_SystemAddress;
}

#if defined(__cplusplus) && !defined(_MSC_VER)
STDMETHODIMP_(PHYSICAL_ADDRESS*)
CMiniportWaveCyclicStreamHDA::PhysicalAddress(
    PHYSICAL_ADDRESS *pRet)
{
    pRet->QuadPart = 0;
    if (m_SystemAddress)
        *pRet = MmGetPhysicalAddress(m_SystemAddress);

    return pRet;
}
#else
STDMETHODIMP_(PHYSICAL_ADDRESS)
CMiniportWaveCyclicStreamHDA::PhysicalAddress(void)
{
    PHYSICAL_ADDRESS Address;

    Address.QuadPart = 0;
    if (m_SystemAddress)
        Address = MmGetPhysicalAddress(m_SystemAddress);

    return Address;
}
#endif

STDMETHODIMP_(PADAPTER_OBJECT)
CMiniportWaveCyclicStreamHDA::GetAdapterObject(void)
{
    /* the DMA engine lives in the HDA controller, no system adapter exists */
    return NULL;
}

STDMETHODIMP_(void)
CMiniportWaveCyclicStreamHDA::CopyTo(
    IN PVOID Destination,
    IN PVOID Source,
    IN ULONG ByteCount)
{
    RtlCopyMemory(Destination, Source, ByteCount);
}

STDMETHODIMP_(void)
CMiniportWaveCyclicStreamHDA::CopyFrom(
    IN PVOID Destination,
    IN PVOID Source,
    IN ULONG ByteCount)
{
    RtlCopyMemory(Destination, Source, ByteCount);
}
