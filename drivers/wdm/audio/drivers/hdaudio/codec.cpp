/*
 * PROJECT:     ReactOS HD Audio codec function driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Generic HD Audio codec parser and control
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 *
 * The widget graph parsing loosely follows the model of the Linux kernel
 * generic HDA parser (sound/pci/hda/hda_generic.c), drastically simplified:
 * the first viable path from an analog output pin to an audio output
 * converter (DAC) is picked, powered up, unmuted and connected.
 */

#include "driver.h"

#define NDEBUG
#include <debug.h>

class CHDACodec : public IHDACodec,
                  public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CHDACodec);
    ~CHDACodec();

    /* IHDACodec */
    STDMETHODIMP_(NTSTATUS) Init(IN PDEVICE_OBJECT DeviceObject);
    STDMETHODIMP_(PHDAUDIO_BUS_INTERFACE) GetBusInterface(void);
    STDMETHODIMP_(NTSTATUS) SetRenderStream(IN UCHAR StreamId, IN USHORT ConverterFormat);
    STDMETHODIMP_(BOOLEAN) HasVolumeControl(void);
    STDMETHODIMP_(void) GetVolumeRange(OUT PLONG Minimum, OUT PLONG Maximum, OUT PLONG Step);
    STDMETHODIMP_(NTSTATUS) SetVolume(IN ULONG Channel, IN LONG Level);
    STDMETHODIMP_(NTSTATUS) GetVolume(IN ULONG Channel, OUT PLONG Level);
    STDMETHODIMP_(NTSTATUS) SetMute(IN BOOL Mute);
    STDMETHODIMP_(NTSTATUS) GetMute(OUT PBOOL Mute);

private:
    NTSTATUS QueryBusInterface(IN PDEVICE_OBJECT DeviceObject);
    NTSTATUS CodecRead(UCHAR Nid, USHORT Verb, USHORT Payload, PULONG Response);
    NTSTATUS CodecWrite(UCHAR Nid, USHORT Verb, USHORT Payload);
    NTSTATUS CodecWrite16(UCHAR Nid, UCHAR Verb, USHORT Payload);
    ULONG GetParameter(UCHAR Nid, UCHAR Param);
    PHDA_WIDGET GetWidget(UCHAR Nid);
    NTSTATUS ReadConnections(PHDA_WIDGET Widget);
    NTSTATUS ParseCodec(void);
    BOOLEAN FindOutputPathFrom(PHDA_WIDGET Widget, ULONG Depth);
    NTSTATUS SelectOutputPin(void);
    NTSTATUS EnableRenderPath(void);
    ULONG GetOutAmpCaps(PHDA_WIDGET Widget);

    HDAUDIO_BUS_INTERFACE m_BusInterface;
    BOOLEAN m_HaveBusInterface;

    UCHAR m_CodecAddress;
    UCHAR m_AfgNid;

    PHDA_WIDGET m_Widgets;
    ULONG m_WidgetCount;

    /* render path, [0] = pin ... [PathLength-1] = DAC */
    UCHAR m_Path[HDA_MAX_PATH_LENGTH];
    ULONG m_PathLength;
    UCHAR m_DacNid;
    UCHAR m_PinNid;

    /* output volume amplifier */
    UCHAR m_AmpNid;
    ULONG m_AmpCaps;

    /* cached volume state, 16.16 dB per channel */
    LONG m_VolumeLevel[2];
    BOOL m_Muted;
};

/*****************************************************************************
 * NewHDACodec
 */
NTSTATUS
NewHDACodec(
    OUT PUNKNOWN *Unknown,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType)
{
    PAGED_CODE();
    STD_CREATE_BODY_WITH_TAG_(CHDACodec, Unknown, UnknownOuter, PoolType,
                              HDA_POOL_TAG, PUNKNOWN);
}

CHDACodec::~CHDACodec()
{
    PAGED_CODE();

    if (m_Widgets)
    {
        ExFreePoolWithTag(m_Widgets, HDA_POOL_TAG);
        m_Widgets = NULL;
    }

    if (m_HaveBusInterface && m_BusInterface.InterfaceDereference)
    {
        m_BusInterface.InterfaceDereference(m_BusInterface.Context);
        m_HaveBusInterface = FALSE;
    }
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::NonDelegatingQueryInterface(
    IN REFIID Interface,
    OUT PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PHDACODEC(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IHDACodec))
    {
        *Object = PVOID(PHDACODEC(this));
    }
    else
    {
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

/*****************************************************************************
 * CHDACodec::QueryBusInterface
 *****************************************************************************
 * Sends IRP_MN_QUERY_INTERFACE for GUID_HDAUDIO_BUS_INTERFACE to the lower
 * device stack (the HDAUDIO bus PDO). Must be called at PASSIVE_LEVEL after
 * the lower stack has been started.
 */
NTSTATUS
CHDACodec::QueryBusInterface(
    IN PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT LowerDevice;
    PIO_STACK_LOCATION IoStack;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    PAGED_CODE();

    LowerDevice = IoGetLowerDeviceObject(DeviceObject);
    if (!LowerDevice)
        return STATUS_NO_SUCH_DEVICE;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       LowerDevice,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatus);
    if (!Irp)
    {
        ObDereferenceObject(LowerDevice);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* PnP IRPs must be initialized to STATUS_NOT_SUPPORTED */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_PNP;
    IoStack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    IoStack->Parameters.QueryInterface.InterfaceType = &GUID_HDAUDIO_BUS_INTERFACE;
    IoStack->Parameters.QueryInterface.Size = sizeof(HDAUDIO_BUS_INTERFACE);
    IoStack->Parameters.QueryInterface.Version = 0x0100;
    IoStack->Parameters.QueryInterface.Interface = (PINTERFACE)&m_BusInterface;
    IoStack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    ObDereferenceObject(LowerDevice);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: failed to query HDAUDIO_BUS_INTERFACE: 0x%lx\n", Status);
        return Status;
    }

    if (!m_BusInterface.TransferCodecVerbs ||
        !m_BusInterface.AllocateRenderDmaEngine ||
        !m_BusInterface.AllocateDmaBuffer ||
        !m_BusInterface.FreeDmaBuffer ||
        !m_BusInterface.FreeDmaEngine ||
        !m_BusInterface.SetDmaEngineState ||
        !m_BusInterface.GetLinkPositionRegister ||
        !m_BusInterface.GetResourceInformation)
    {
        DPRINT1("HDAUDIO: bus interface is incomplete\n");
        return STATUS_NOT_SUPPORTED;
    }

    if (m_BusInterface.InterfaceReference)
        m_BusInterface.InterfaceReference(m_BusInterface.Context);
    m_HaveBusInterface = TRUE;

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * Verb helpers
 */
NTSTATUS
CHDACodec::CodecRead(
    UCHAR Nid,
    USHORT Verb,
    USHORT Payload,
    PULONG Response)
{
    HDAUDIO_CODEC_TRANSFER Transfer;
    NTSTATUS Status;

    RtlZeroMemory(&Transfer, sizeof(Transfer));
    Transfer.Output.Verb8.CodecAddress = m_CodecAddress;
    Transfer.Output.Verb8.Node = Nid;
    Transfer.Output.Verb8.VerbId = Verb;
    Transfer.Output.Verb8.Data = (UCHAR)Payload;

    Status = m_BusInterface.TransferCodecVerbs(m_BusInterface.Context,
                                               1, &Transfer, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Transfer.Input.IsValid)
        return STATUS_IO_DEVICE_ERROR;

    if (Response)
        *Response = Transfer.Input.Response;

    return STATUS_SUCCESS;
}

NTSTATUS
CHDACodec::CodecWrite(
    UCHAR Nid,
    USHORT Verb,
    USHORT Payload)
{
    return CodecRead(Nid, Verb, Payload, NULL);
}

NTSTATUS
CHDACodec::CodecWrite16(
    UCHAR Nid,
    UCHAR Verb,
    USHORT Payload)
{
    HDAUDIO_CODEC_TRANSFER Transfer;
    NTSTATUS Status;

    RtlZeroMemory(&Transfer, sizeof(Transfer));
    Transfer.Output.Verb16.CodecAddress = m_CodecAddress;
    Transfer.Output.Verb16.Node = Nid;
    Transfer.Output.Verb16.VerbId = Verb;
    Transfer.Output.Verb16.Data = Payload;

    Status = m_BusInterface.TransferCodecVerbs(m_BusInterface.Context,
                                               1, &Transfer, NULL, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Transfer.Input.IsValid)
        return STATUS_IO_DEVICE_ERROR;

    return STATUS_SUCCESS;
}

ULONG
CHDACodec::GetParameter(
    UCHAR Nid,
    UCHAR Param)
{
    ULONG Response = 0;

    if (!NT_SUCCESS(CodecRead(Nid, HDA_VERB_GET_PARAMETERS, Param, &Response)))
        return 0;

    return Response;
}

PHDA_WIDGET
CHDACodec::GetWidget(
    UCHAR Nid)
{
    ULONG i;

    for (i = 0; i < m_WidgetCount; i++)
    {
        if (m_Widgets[i].Nid == Nid)
            return &m_Widgets[i];
    }

    return NULL;
}

/*****************************************************************************
 * CHDACodec::ReadConnections
 *****************************************************************************
 * Reads the connection list of a widget, expanding range entries.
 * See Intel HDA spec 7.3.3.3 / 7.1.2 (short and long form entries).
 */
NTSTATUS
CHDACodec::ReadConnections(
    PHDA_WIDGET Widget)
{
    ULONG ListLength;
    ULONG EntriesPerResponse;
    ULONG EntryBits, EntryMask, RangeBit;
    ULONG Count, i;
    ULONG PrevNid = 0;

    Widget->ConnCount = 0;

    if (!(Widget->Caps & HDA_WCAP_CONN_LIST))
        return STATUS_SUCCESS;

    ListLength = GetParameter(Widget->Nid, HDA_PAR_CONNLIST_LEN);
    Count = ListLength & HDA_CLIST_LEN_MASK;
    if (Count == 0)
        return STATUS_SUCCESS;

    if (ListLength & HDA_CLIST_LONG)
    {
        EntriesPerResponse = 2;
        EntryBits = 16;
        EntryMask = 0x7fff;
        RangeBit = 0x8000;
    }
    else
    {
        EntriesPerResponse = 4;
        EntryBits = 8;
        EntryMask = 0x7f;
        RangeBit = 0x80;
    }

    for (i = 0; i < Count; i += EntriesPerResponse)
    {
        ULONG Response, j;

        if (!NT_SUCCESS(CodecRead(Widget->Nid, HDA_VERB_GET_CONNECT_LIST,
                                  (USHORT)i, &Response)))
            return STATUS_IO_DEVICE_ERROR;

        for (j = 0; j < EntriesPerResponse && (i + j) < Count; j++)
        {
            ULONG Entry = (Response >> (j * EntryBits)) & (EntryMask | RangeBit);
            ULONG Nid = Entry & EntryMask;

            if (Entry & RangeBit)
            {
                /* range entry: previous entry .. this entry */
                ULONG n;

                if (PrevNid == 0 || Nid < PrevNid)
                    continue;

                for (n = PrevNid + 1; n <= Nid; n++)
                {
                    if (Widget->ConnCount >= HDA_MAX_CONNECTIONS)
                        break;
                    Widget->Connections[Widget->ConnCount++] = (UCHAR)n;
                }
            }
            else if (Nid != 0)
            {
                if (Widget->ConnCount < HDA_MAX_CONNECTIONS)
                    Widget->Connections[Widget->ConnCount++] = (UCHAR)Nid;
            }

            PrevNid = Nid;
        }
    }

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * CHDACodec::ParseCodec
 *****************************************************************************
 * Walks the audio function group and caches all widgets.
 */
NTSTATUS
CHDACodec::ParseCodec(void)
{
    ULONG NodeCount;
    ULONG StartNid, NumNodes;
    ULONG VendorId;
    ULONG i;

    PAGED_CODE();

    m_BusInterface.GetResourceInformation(m_BusInterface.Context,
                                          &m_CodecAddress,
                                          &m_AfgNid);

    VendorId = GetParameter(0, HDA_PAR_VENDOR_ID);

    if (m_AfgNid == 0)
    {
        /* fall back: search the root node's children for the AFG */
        ULONG RootNodes = GetParameter(0, HDA_PAR_NODE_COUNT);
        ULONG FgStart = (RootNodes >> 16) & 0xff;
        ULONG FgCount = RootNodes & 0xff;

        for (i = 0; i < FgCount; i++)
        {
            ULONG FuncType = GetParameter((UCHAR)(FgStart + i), HDA_PAR_FUNCTION_TYPE);
            if ((FuncType & 0xff) == HDA_GRP_AUDIO_FUNCTION)
            {
                m_AfgNid = (UCHAR)(FgStart + i);
                break;
            }
        }
    }

    if (m_AfgNid == 0)
    {
        DPRINT1("HDAUDIO: no audio function group found (codec %u, vendor 0x%08lx)\n",
                m_CodecAddress, VendorId);
        return STATUS_NOT_SUPPORTED;
    }

    /* power up the function group */
    CodecWrite(m_AfgNid, HDA_VERB_SET_POWER_STATE, HDA_PWRST_D0);

    NodeCount = GetParameter(m_AfgNid, HDA_PAR_NODE_COUNT);
    StartNid = (NodeCount >> 16) & 0xff;
    NumNodes = NodeCount & 0xff;

    DPRINT1("HDAUDIO: codec %u vendor 0x%08lx AFG %u, widgets %lu..%lu\n",
            m_CodecAddress, VendorId, m_AfgNid, StartNid, StartNid + NumNodes - 1);

    if (NumNodes == 0 || NumNodes > HDA_MAX_WIDGETS)
        return STATUS_NOT_SUPPORTED;

    m_Widgets = (PHDA_WIDGET)ExAllocatePoolZero(NonPagedPool,
                                                NumNodes * sizeof(HDA_WIDGET),
                                                HDA_POOL_TAG);
    if (!m_Widgets)
        return STATUS_INSUFFICIENT_RESOURCES;

    m_WidgetCount = NumNodes;

    for (i = 0; i < NumNodes; i++)
    {
        PHDA_WIDGET Widget = &m_Widgets[i];

        Widget->Nid = (UCHAR)(StartNid + i);
        Widget->Caps = GetParameter(Widget->Nid, HDA_PAR_AUDIO_WIDGET_CAP);

        if (HDA_WCAP_TYPE(Widget->Caps) == HDA_WID_PIN)
        {
            Widget->PinCaps = GetParameter(Widget->Nid, HDA_PAR_PIN_CAP);

            if (!NT_SUCCESS(CodecRead(Widget->Nid, HDA_VERB_GET_CONFIG_DEFAULT,
                                      0, &Widget->DefConfig)))
                Widget->DefConfig = 0;
        }

        ReadConnections(Widget);

        DPRINT("HDAUDIO: widget %u type %lu caps 0x%08lx pincaps 0x%08lx defcfg 0x%08lx conns %u\n",
               Widget->Nid, HDA_WCAP_TYPE(Widget->Caps), Widget->Caps,
               Widget->PinCaps, Widget->DefConfig, Widget->ConnCount);
    }

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * CHDACodec::FindOutputPathFrom
 *****************************************************************************
 * Depth-first search from an output pin (already pushed on m_Path) towards
 * an audio output converter. Digital widgets are skipped.
 */
BOOLEAN
CHDACodec::FindOutputPathFrom(
    PHDA_WIDGET Widget,
    ULONG Depth)
{
    USHORT i;

    if (Depth >= HDA_MAX_PATH_LENGTH - 1)
        return FALSE;

    /* first pass: directly connected DACs */
    for (i = 0; i < Widget->ConnCount; i++)
    {
        PHDA_WIDGET Conn = GetWidget(Widget->Connections[i]);

        if (!Conn || (Conn->Caps & HDA_WCAP_DIGITAL))
            continue;

        if (HDA_WCAP_TYPE(Conn->Caps) == HDA_WID_AUD_OUT)
        {
            m_Path[m_PathLength++] = Conn->Nid;
            return TRUE;
        }
    }

    /* second pass: recurse through mixers and selectors */
    for (i = 0; i < Widget->ConnCount; i++)
    {
        PHDA_WIDGET Conn = GetWidget(Widget->Connections[i]);
        ULONG Type;

        if (!Conn || (Conn->Caps & HDA_WCAP_DIGITAL))
            continue;

        Type = HDA_WCAP_TYPE(Conn->Caps);
        if (Type != HDA_WID_AUD_MIX && Type != HDA_WID_AUD_SEL)
            continue;

        m_Path[m_PathLength++] = Conn->Nid;
        if (FindOutputPathFrom(Conn, Depth + 1))
            return TRUE;
        m_PathLength--;
    }

    return FALSE;
}

/*****************************************************************************
 * CHDACodec::SelectOutputPin
 *****************************************************************************
 * Picks the best analog output pin and binds a path pin -> ... -> DAC.
 * Preference order: line out, speaker, headphone (like the default
 * association ordering used by hda_auto_parser in practice for the
 * primary output).
 */
NTSTATUS
CHDACodec::SelectOutputPin(void)
{
    static const UCHAR DevicePreference[] =
        { HDA_JACK_LINE_OUT, HDA_JACK_SPEAKER, HDA_JACK_HP_OUT };
    ULONG p, i;

    PAGED_CODE();

    for (p = 0; p < RTL_NUMBER_OF(DevicePreference); p++)
    {
        for (i = 0; i < m_WidgetCount; i++)
        {
            PHDA_WIDGET Widget = &m_Widgets[i];

            if (HDA_WCAP_TYPE(Widget->Caps) != HDA_WID_PIN)
                continue;
            if (Widget->Caps & HDA_WCAP_DIGITAL)
                continue;
            if (!(Widget->PinCaps & HDA_PINCAP_OUT))
                continue;
            if (HDA_DEFCFG_PORT_CONN(Widget->DefConfig) == HDA_PORT_CONN_NONE)
                continue;
            if (HDA_DEFCFG_DEVICE(Widget->DefConfig) != DevicePreference[p])
                continue;

            m_PathLength = 0;
            m_Path[m_PathLength++] = Widget->Nid;

            if (FindOutputPathFrom(Widget, 0))
            {
                m_PinNid = m_Path[0];
                m_DacNid = m_Path[m_PathLength - 1];

                DPRINT1("HDAUDIO: render path: pin %u -> DAC %u (%lu widgets)\n",
                        m_PinNid, m_DacNid, m_PathLength);
                return STATUS_SUCCESS;
            }
        }
    }

    DPRINT1("HDAUDIO: no analog output path found\n");
    return STATUS_NOT_SUPPORTED;
}

ULONG
CHDACodec::GetOutAmpCaps(
    PHDA_WIDGET Widget)
{
    if (!(Widget->Caps & HDA_WCAP_OUT_AMP))
        return 0;

    if (Widget->Caps & HDA_WCAP_AMP_OVRD)
        return GetParameter(Widget->Nid, HDA_PAR_AMP_OUT_CAP);

    return GetParameter(m_AfgNid, HDA_PAR_AMP_OUT_CAP);
}

/*****************************************************************************
 * CHDACodec::EnableRenderPath
 *****************************************************************************
 * Powers up, connects and unmutes every widget on the render path
 * (walking from the DAC towards the pin).
 */
NTSTATUS
CHDACodec::EnableRenderPath(void)
{
    LONG i;

    PAGED_CODE();

    for (i = (LONG)m_PathLength - 1; i >= 0; i--)
    {
        PHDA_WIDGET Widget = GetWidget(m_Path[i]);
        ULONG Type;

        if (!Widget)
            return STATUS_INTERNAL_ERROR;

        Type = HDA_WCAP_TYPE(Widget->Caps);

        if (Widget->Caps & HDA_WCAP_POWER)
            CodecWrite(Widget->Nid, HDA_VERB_SET_POWER_STATE, HDA_PWRST_D0);

        /* unmute + 0dB on the output amplifier */
        if (Widget->Caps & HDA_WCAP_OUT_AMP)
        {
            ULONG AmpCaps = GetOutAmpCaps(Widget);
            USHORT Gain = (USHORT)HDA_AMPCAP_OFFSET(AmpCaps); /* 0dB */

            CodecWrite16(Widget->Nid, HDA_VERB16_SET_AMP_GAIN_MUTE,
                         HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT |
                         HDA_AMP_SET_RIGHT | Gain);

            /* remember the amp closest to the pin for volume control */
            m_AmpNid = Widget->Nid;
            m_AmpCaps = AmpCaps;
        }

        /* route the widget to its upstream path member */
        if (i < (LONG)m_PathLength - 1)
        {
            UCHAR Upstream = m_Path[i + 1];
            USHORT c;

            for (c = 0; c < Widget->ConnCount; c++)
            {
                if (Widget->Connections[c] == Upstream)
                    break;
            }

            if (c < Widget->ConnCount)
            {
                if ((Type == HDA_WID_PIN || Type == HDA_WID_AUD_SEL) &&
                    Widget->ConnCount > 1)
                {
                    CodecWrite(Widget->Nid, HDA_VERB_SET_CONNECT_SEL, c);
                }

                /* unmute the mixer input feeding from the upstream widget */
                if (Widget->Caps & HDA_WCAP_IN_AMP)
                {
                    CodecWrite16(Widget->Nid, HDA_VERB16_SET_AMP_GAIN_MUTE,
                                 (USHORT)(HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT |
                                          HDA_AMP_SET_RIGHT |
                                          (c << HDA_AMP_SET_INDEX_SHIFT)));
                }
            }
        }

        if (Type == HDA_WID_PIN)
        {
            USHORT PinCtl = HDA_PINCTL_OUT_EN;

            if (HDA_DEFCFG_DEVICE(Widget->DefConfig) == HDA_JACK_HP_OUT &&
                (Widget->PinCaps & HDA_PINCAP_HP_DRV))
            {
                PinCtl |= HDA_PINCTL_HP_EN;
            }

            CodecWrite(Widget->Nid, HDA_VERB_SET_PIN_WIDGET_CONTROL, PinCtl);

            /* enable the external amplifier if the pin supports EAPD */
            if (Widget->PinCaps & HDA_PINCAP_EAPD)
                CodecWrite(Widget->Nid, HDA_VERB_SET_EAPD_BTLENABLE, HDA_EAPDBTL_EAPD);
        }
    }

    m_VolumeLevel[0] = 0;
    m_VolumeLevel[1] = 0;
    m_Muted = FALSE;

    return STATUS_SUCCESS;
}

/*****************************************************************************
 * IHDACodec methods
 */
STDMETHODIMP_(NTSTATUS)
CHDACodec::Init(
    IN PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = QueryBusInterface(DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ParseCodec();
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SelectOutputPin();
    if (!NT_SUCCESS(Status))
        return Status;

    return EnableRenderPath();
}

STDMETHODIMP_(PHDAUDIO_BUS_INTERFACE)
CHDACodec::GetBusInterface(void)
{
    return m_HaveBusInterface ? &m_BusInterface : NULL;
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::SetRenderStream(
    IN UCHAR StreamId,
    IN USHORT ConverterFormat)
{
    NTSTATUS Status;

    PAGED_CODE();

    /* stream tag in bits 7:4, channel (lowest of the stream) in bits 3:0 */
    Status = CodecWrite(m_DacNid, HDA_VERB_SET_CHANNEL_STREAMID,
                        (USHORT)((StreamId & 0xf) << 4));
    if (!NT_SUCCESS(Status))
        return Status;

    return CodecWrite16(m_DacNid, HDA_VERB16_SET_STREAM_FORMAT, ConverterFormat);
}

STDMETHODIMP_(BOOLEAN)
CHDACodec::HasVolumeControl(void)
{
    return m_AmpNid != 0 && HDA_AMPCAP_NUM_STEPS(m_AmpCaps) != 0;
}

/*****************************************************************************
 * Amplifier step <-> 16.16 dB conversion.
 * Per HDA spec 7.3.4.10: each step is (StepSize + 1) * 0.25 dB,
 * step 'Offset' equals 0dB.
 */
STDMETHODIMP_(void)
CHDACodec::GetVolumeRange(
    OUT PLONG Minimum,
    OUT PLONG Maximum,
    OUT PLONG Step)
{
    LONG StepSizeDb;    /* 16.16 dB per step */
    LONG Offset = (LONG)HDA_AMPCAP_OFFSET(m_AmpCaps);
    LONG NumSteps = (LONG)HDA_AMPCAP_NUM_STEPS(m_AmpCaps);

    StepSizeDb = ((LONG)HDA_AMPCAP_STEP_SIZE(m_AmpCaps) + 1) * 0x4000; /* 0.25dB = 0x4000 */

    *Step = StepSizeDb;
    *Minimum = -Offset * StepSizeDb;
    *Maximum = (NumSteps - Offset) * StepSizeDb;
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::SetVolume(
    IN ULONG Channel,
    IN LONG Level)
{
    LONG Minimum, Maximum, Step;
    LONG StepValue;
    USHORT Flags;

    PAGED_CODE();

    if (!HasVolumeControl())
        return STATUS_NOT_SUPPORTED;

    GetVolumeRange(&Minimum, &Maximum, &Step);

    if (Level == HDA_PROP_MOST_NEGATIVE || Level < Minimum)
        Level = Minimum;
    if (Level > Maximum)
        Level = Maximum;

    /* convert dB to amplifier step */
    StepValue = (Level - Minimum + Step / 2) / Step;
    if (StepValue < 0)
        StepValue = 0;
    if (StepValue > (LONG)HDA_AMPCAP_NUM_STEPS(m_AmpCaps))
        StepValue = (LONG)HDA_AMPCAP_NUM_STEPS(m_AmpCaps);

    if (Channel == (ULONG)-1)
        Flags = HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT;
    else if (Channel == 0)
        Flags = HDA_AMP_SET_LEFT;
    else if (Channel == 1)
        Flags = HDA_AMP_SET_RIGHT;
    else
        return STATUS_INVALID_PARAMETER;

    if (Channel == (ULONG)-1 || Channel == 0)
        m_VolumeLevel[0] = Level;
    if (Channel == (ULONG)-1 || Channel == 1)
        m_VolumeLevel[1] = Level;

    return CodecWrite16(m_AmpNid, HDA_VERB16_SET_AMP_GAIN_MUTE,
                        (USHORT)(HDA_AMP_SET_OUTPUT | Flags |
                                 (m_Muted ? HDA_AMP_MUTE : 0) |
                                 (StepValue & HDA_AMP_GAIN_MASK)));
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::GetVolume(
    IN ULONG Channel,
    OUT PLONG Level)
{
    PAGED_CODE();

    if (!HasVolumeControl())
        return STATUS_NOT_SUPPORTED;

    if (Channel == 0 || Channel == (ULONG)-1)
        *Level = m_VolumeLevel[0];
    else if (Channel == 1)
        *Level = m_VolumeLevel[1];
    else
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::SetMute(
    IN BOOL Mute)
{
    NTSTATUS Status;

    PAGED_CODE();

    if (!HasVolumeControl() || !(m_AmpCaps & HDA_AMPCAP_MUTE))
        return STATUS_NOT_SUPPORTED;

    m_Muted = Mute;

    /* reprogram both channels with the cached gain and the new mute state */
    Status = SetVolume(0, m_VolumeLevel[0]);
    if (NT_SUCCESS(Status))
        Status = SetVolume(1, m_VolumeLevel[1]);

    return Status;
}

STDMETHODIMP_(NTSTATUS)
CHDACodec::GetMute(
    OUT PBOOL Mute)
{
    PAGED_CODE();

    if (!HasVolumeControl() || !(m_AmpCaps & HDA_AMPCAP_MUTE))
        return STATUS_NOT_SUPPORTED;

    *Mute = m_Muted;
    return STATUS_SUCCESS;
}
