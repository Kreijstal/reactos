/*
 * PROJECT:     ReactOS HD Audio codec function driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Minimal topology miniport (volume + mute on the render path)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "driver.h"

#define NDEBUG
#include <debug.h>

/* topology filter pins */
#define TOPO_PIN_WAVEOUT_SOURCE 0
#define TOPO_PIN_LINEOUT_DEST   1

/* topology filter nodes */
#define TOPO_NODE_VOLUME        0
#define TOPO_NODE_MUTE          1

/* fallback volume range when the codec path has no amplifier: -96..0 dB */
#define TOPO_FALLBACK_MIN_DB    (-96 << 16)
#define TOPO_FALLBACK_MAX_DB    0
#define TOPO_FALLBACK_STEP_DB   (3 << 16)

class CMiniportTopologyHDA : public IMiniportTopology,
                             public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopologyHDA);
    ~CMiniportTopologyHDA();

    /* IMiniport */
    STDMETHODIMP_(NTSTATUS) GetDescription(OUT PPCFILTER_DESCRIPTOR *Description);
    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(IN ULONG PinId,
                                                  IN PKSDATARANGE DataRange,
                                                  IN PKSDATARANGE MatchingDataRange,
                                                  IN ULONG OutputBufferLength,
                                                  OUT PVOID ResultantFormat OPTIONAL,
                                                  OUT PULONG ResultantFormatLength);

    /* IMiniportTopology */
    STDMETHODIMP_(NTSTATUS) Init(IN PUNKNOWN UnknownAdapter,
                                 IN PRESOURCELIST ResourceList,
                                 IN PPORTTOPOLOGY Port);

    static NTSTATUS NTAPI PropertyHandler_Volume(IN PPCPROPERTY_REQUEST PropertyRequest);
    static NTSTATUS NTAPI PropertyHandler_Mute(IN PPCPROPERTY_REQUEST PropertyRequest);
    static NTSTATUS NTAPI PropertyHandler_CpuResources(IN PPCPROPERTY_REQUEST PropertyRequest);

private:
    NTSTATUS BasicSupportVolume(IN PPCPROPERTY_REQUEST PropertyRequest);
    void GetRange(OUT PLONG Minimum, OUT PLONG Maximum, OUT PLONG Step);

    PHDACODEC m_Codec;

    /* cached state (also used when the path has no amplifier) */
    LONG m_VolumeLevel[2];
    BOOL m_Muted;
};

/*****************************************************************************
 * Automation tables
 */
static PCPROPERTY_ITEM PropertiesVolume[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMiniportTopologyHDA::PropertyHandler_Volume
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET,
        CMiniportTopologyHDA::PropertyHandler_CpuResources
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationVolume, PropertiesVolume);

static PCPROPERTY_ITEM PropertiesMute[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        CMiniportTopologyHDA::PropertyHandler_Mute
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CPU_RESOURCES,
        KSPROPERTY_TYPE_GET,
        CMiniportTopologyHDA::PropertyHandler_CpuResources
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMute, PropertiesMute);

/*****************************************************************************
 * Filter layout
 */
static KSDATARANGE TopoPinDataRangeBridge =
{
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    {STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO)},
    {STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG)},
    {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};

static PKSDATARANGE TopoPinDataRangePointersBridge[] =
{
    &TopoPinDataRangeBridge
};

static PCPIN_DESCRIPTOR TopoPins[] =
{
    /* TOPO_PIN_WAVEOUT_SOURCE */
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(TopoPinDataRangePointersBridge),
            TopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSCATEGORY_AUDIO,
            NULL,
            {0}
        }
    },
    /* TOPO_PIN_LINEOUT_DEST */
    {
        0, 0, 0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(TopoPinDataRangePointersBridge),
            TopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            (GUID *)&KSNODETYPE_SPEAKER,
            NULL,
            {0}
        }
    }
};

static PCNODE_DESCRIPTOR TopoNodes[] =
{
    /* TOPO_NODE_VOLUME */
    { 0, &AutomationVolume, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME },
    /* TOPO_NODE_MUTE */
    { 0, &AutomationMute, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE }
};

static PCCONNECTION_DESCRIPTOR TopoConnections[] =
{
    { PCFILTER_NODE, TOPO_PIN_WAVEOUT_SOURCE, TOPO_NODE_VOLUME, 1 },
    { TOPO_NODE_VOLUME, 0, TOPO_NODE_MUTE, 1 },
    { TOPO_NODE_MUTE, 0, PCFILTER_NODE, TOPO_PIN_LINEOUT_DEST }
};

static GUID TopoCategories[] =
{
    {STATICGUIDOF(KSCATEGORY_AUDIO)},
    {STATICGUIDOF(KSCATEGORY_TOPOLOGY)}
};

static PCFILTER_DESCRIPTOR TopoFilterDescriptor =
{
    0,                                  /* Version */
    NULL,                               /* AutomationTable */
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(TopoPins),
    TopoPins,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(TopoNodes),
    TopoNodes,
    SIZEOF_ARRAY(TopoConnections),
    TopoConnections,
    SIZEOF_ARRAY(TopoCategories),
    TopoCategories
};

/*****************************************************************************
 * CreateMiniportTopologyHDA
 */
NTSTATUS
CreateMiniportTopologyHDA(
    OUT PUNKNOWN *Unknown,
    IN REFCLSID ClassId,
    IN PUNKNOWN UnknownOuter OPTIONAL,
    IN POOL_TYPE PoolType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ClassId);

    STD_CREATE_BODY_WITH_TAG_(CMiniportTopologyHDA, Unknown, UnknownOuter,
                              PoolType, HDA_POOL_TAG, PUNKNOWN);
}

CMiniportTopologyHDA::~CMiniportTopologyHDA()
{
    PAGED_CODE();

    if (m_Codec)
    {
        m_Codec->Release();
        m_Codec = NULL;
    }
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyHDA::NonDelegatingQueryInterface(
    IN REFIID Interface,
    OUT PVOID *Object)
{
    PAGED_CODE();

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
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
CMiniportTopologyHDA::Init(
    IN PUNKNOWN UnknownAdapter,
    IN PRESOURCELIST ResourceList,
    IN PPORTTOPOLOGY Port)
{
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);

    if (!UnknownAdapter)
        return STATUS_INVALID_PARAMETER;

    Status = UnknownAdapter->QueryInterface(IID_IHDACodec, (PVOID *)&m_Codec);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: topology miniport: no IHDACodec interface\n");
        return Status;
    }

    m_VolumeLevel[0] = 0;
    m_VolumeLevel[1] = 0;
    m_Muted = FALSE;

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyHDA::GetDescription(
    OUT PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();

    *Description = &TopoFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CMiniportTopologyHDA::DataRangeIntersection(
    IN ULONG PinId,
    IN PKSDATARANGE DataRange,
    IN PKSDATARANGE MatchingDataRange,
    IN ULONG OutputBufferLength,
    OUT PVOID ResultantFormat OPTIONAL,
    OUT PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);

    /* topology pins are bridge pins, there is nothing to intersect */
    return STATUS_NOT_IMPLEMENTED;
}

void
CMiniportTopologyHDA::GetRange(
    OUT PLONG Minimum,
    OUT PLONG Maximum,
    OUT PLONG Step)
{
    if (m_Codec->HasVolumeControl())
    {
        m_Codec->GetVolumeRange(Minimum, Maximum, Step);
    }
    else
    {
        *Minimum = TOPO_FALLBACK_MIN_DB;
        *Maximum = TOPO_FALLBACK_MAX_DB;
        *Step = TOPO_FALLBACK_STEP_DB;
    }
}

/*****************************************************************************
 * CMiniportTopologyHDA::BasicSupportVolume
 *****************************************************************************
 * Returns KSPROPERTY_DESCRIPTION with a stepped dB range for the volume
 * node (same layout as the ac97 sample driver).
 */
NTSTATUS
CMiniportTopologyHDA::BasicSupportVolume(
    IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION PropDesc = (PKSPROPERTY_DESCRIPTION)PropertyRequest->Value;

        PropDesc->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT |
                                KSPROPERTY_TYPE_GET |
                                KSPROPERTY_TYPE_SET;
        PropDesc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION) +
                                    sizeof(KSPROPERTY_MEMBERSHEADER) +
                                    sizeof(KSPROPERTY_STEPPING_LONG);
        PropDesc->PropTypeSet.Set = KSPROPTYPESETID_General;
        PropDesc->PropTypeSet.Id = VT_I4;
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount = 1;
        PropDesc->Reserved = 0;

        if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION) +
            sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG))
        {
            PKSPROPERTY_MEMBERSHEADER Members = (PKSPROPERTY_MEMBERSHEADER)(PropDesc + 1);
            PKSPROPERTY_STEPPING_LONG Range;
            LONG Minimum, Maximum, Step;

            Members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
            Members->MembersCount = 1;
            Members->Flags = 0;

            Range = (PKSPROPERTY_STEPPING_LONG)(Members + 1);

            GetRange(&Minimum, &Maximum, &Step);

            Range->Bounds.SignedMinimum = Minimum;
            Range->Bounds.SignedMaximum = Maximum;
            Range->SteppingDelta = Step;
            Range->Reserved = 0;

            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION) +
                                         sizeof(KSPROPERTY_MEMBERSHEADER) +
                                         sizeof(KSPROPERTY_STEPPING_LONG);
        }
        else
        {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }

        return STATUS_SUCCESS;
    }

    if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        PULONG AccessFlags = (PULONG)PropertyRequest->Value;

        *AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT |
                       KSPROPERTY_TYPE_GET |
                       KSPROPERTY_TYPE_SET;

        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    return STATUS_BUFFER_TOO_SMALL;
}

/*****************************************************************************
 * Property handlers
 */
NTSTATUS
NTAPI
CMiniportTopologyHDA::PropertyHandler_Volume(
    IN PPCPROPERTY_REQUEST PropertyRequest)
{
    CMiniportTopologyHDA *That;
    ULONG Channel = MAXULONG;

    PAGED_CODE();

    That = (CMiniportTopologyHDA *)(PMINIPORTTOPOLOGY)PropertyRequest->MajorTarget;

    if (PropertyRequest->Node != TOPO_NODE_VOLUME)
        return STATUS_INVALID_PARAMETER;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        return That->BasicSupportVolume(PropertyRequest);

    /* the channel number follows the KSNODEPROPERTY structure */
    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
        Channel = *(PULONG)PropertyRequest->Instance;

    if (Channel != 0 && Channel != 1 && Channel != MAXULONG)
        return STATUS_INVALID_PARAMETER;

    if (PropertyRequest->ValueSize < sizeof(LONG))
        return STATUS_BUFFER_TOO_SMALL;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PLONG)PropertyRequest->Value =
            That->m_VolumeLevel[(Channel == 1) ? 1 : 0];
        PropertyRequest->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        LONG Level = *(PLONG)PropertyRequest->Value;
        LONG Minimum, Maximum, Step;

        That->GetRange(&Minimum, &Maximum, &Step);

        if (Level == HDA_PROP_MOST_NEGATIVE || Level < Minimum)
            Level = Minimum;
        if (Level > Maximum)
            Level = Maximum;

        if (Channel == 0 || Channel == MAXULONG)
            That->m_VolumeLevel[0] = Level;
        if (Channel == 1 || Channel == MAXULONG)
            That->m_VolumeLevel[1] = Level;

        /* push to the codec amplifier when the path has one */
        if (That->m_Codec->HasVolumeControl())
            That->m_Codec->SetVolume(Channel, Level);

        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
CMiniportTopologyHDA::PropertyHandler_Mute(
    IN PPCPROPERTY_REQUEST PropertyRequest)
{
    CMiniportTopologyHDA *That;

    PAGED_CODE();

    That = (CMiniportTopologyHDA *)(PMINIPORTTOPOLOGY)PropertyRequest->MajorTarget;

    if (PropertyRequest->Node != TOPO_NODE_MUTE)
        return STATUS_INVALID_PARAMETER;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        if (PropertyRequest->ValueSize < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *(PULONG)PropertyRequest->Value = KSPROPERTY_TYPE_BASICSUPPORT |
                                          KSPROPERTY_TYPE_GET |
                                          KSPROPERTY_TYPE_SET;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->ValueSize < sizeof(BOOL))
        return STATUS_BUFFER_TOO_SMALL;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PBOOL)PropertyRequest->Value = That->m_Muted;
        PropertyRequest->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        That->m_Muted = *(PBOOL)PropertyRequest->Value;

        if (That->m_Codec->HasVolumeControl())
            That->m_Codec->SetMute(That->m_Muted);

        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
CMiniportTopologyHDA::PropertyHandler_CpuResources(
    IN PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        if (PropertyRequest->ValueSize < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *(PULONG)PropertyRequest->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}
