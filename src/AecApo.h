//
// AecApo.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//   Declaration of the CAecApoMFX class.
//

#pragma once

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>
#include "AecApoDll_h.h"

#include <commonmacros.h>
#include <devicetopology.h>

#include <audioengineextensionapo.h>
#include <vector>
#include <cstdint>

#include "SampleFifo.h"
#include <memory>

_Analysis_mode_(_Analysis_code_type_user_driver_)

    enum class AecSampleFormat {
        kUnknown = 0,
        kFloat32,
        kPcm16,
        kPcm24Packed,
        kPcm24In32,
        kPcm32
    };

typedef struct SpeexEchoState_ SpeexEchoState;
typedef struct SpeexPreprocessState_ SpeexPreprocessState;

// Forward declare Speex destroy functions
extern "C" void speex_echo_state_destroy(SpeexEchoState *st);
extern "C" void speex_preprocess_state_destroy(SpeexPreprocessState *st);

// RAII wrappers for Speex resources using unique_ptr with custom deleters
namespace SpeexRAII
{
    struct EchoStateDeleter
    {
        void operator()(SpeexEchoState *state) const
        {
            if (state)
                speex_echo_state_destroy(state);
        }
    };

    struct PreprocessStateDeleter
    {
        void operator()(SpeexPreprocessState *state) const
        {
            if (state)
                speex_preprocess_state_destroy(state);
        }
    };

    using EchoStatePtr = std::unique_ptr<SpeexEchoState, EchoStateDeleter>;
    using PreprocessStatePtr = std::unique_ptr<SpeexPreprocessState, PreprocessStateDeleter>;
}

#pragma AVRT_VTABLES_BEGIN
// Aec APO class - MFX
class CAecApoMFX : public CComObjectRootEx<CComMultiThreadModel>,
                   public CComCoClass<CAecApoMFX, &CLSID_AecApoMFX>,
                   public CBaseAudioProcessingObject,
                   public IAudioSystemEffects3,
                   public IAudioProcessingObjectNotifications,
                   public IApoAcousticEchoCancellation,
                   public IApoAuxiliaryInputConfiguration,
                   public IApoAuxiliaryInputRT
{
public:
    // constructor
    CAecApoMFX()
        : CBaseAudioProcessingObject(sm_RegProperties)
    {
    }
    ~CAecApoMFX();

    DECLARE_REGISTRY_RESOURCEID(IDR_AECAPOMFX)

    BEGIN_COM_MAP(CAecApoMFX)
    COM_INTERFACE_ENTRY(IAudioSystemEffects)
    COM_INTERFACE_ENTRY(IAudioSystemEffects2)
    COM_INTERFACE_ENTRY(IAudioSystemEffects3)
    COM_INTERFACE_ENTRY(IAudioProcessingObjectNotifications)
    COM_INTERFACE_ENTRY(IAudioProcessingObjectRT)
    COM_INTERFACE_ENTRY(IAudioProcessingObject)
    COM_INTERFACE_ENTRY(IAudioProcessingObjectConfiguration)
    COM_INTERFACE_ENTRY(IApoAcousticEchoCancellation)
    COM_INTERFACE_ENTRY(IApoAuxiliaryInputConfiguration)
    COM_INTERFACE_ENTRY(IApoAuxiliaryInputRT)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

public:
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_PROPERTY **ppInputConnections, UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_PROPERTY **ppOutputConnections);

    STDMETHOD(GetLatency)(HNSTIME *pTime);

    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections,
                              APO_CONNECTION_DESCRIPTOR **ppInputConnections,
                              UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR **ppOutputConnections);

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE *pbyData);

    // IAudioSystemEffects2
    STDMETHOD(GetEffectsList)(_Outptr_result_buffer_maybenull_(*pcEffects) LPGUID *ppEffectsIds, _Out_ UINT *pcEffects, _In_ HANDLE Event);

    // IAudioProcessingObject
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *pOutputFormat, IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat);
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *pInputFormat, IAudioMediaType *pRequestedOutputFormat, IAudioMediaType **ppSupportedOutputFormat);

    // IAPOAuxiliaryInputConfiguration
    STDMETHOD(AddAuxiliaryInput)(
        DWORD dwInputId,
        UINT32 cbDataSize,
        BYTE *pbyData,
        APO_CONNECTION_DESCRIPTOR *pInputConnection) override;
    STDMETHOD(RemoveAuxiliaryInput)(
        DWORD dwInputId) override;
    STDMETHOD(IsInputFormatSupported)(
        IAudioMediaType *pRequestedInputFormat,
        IAudioMediaType **ppSupportedInputFormat) override;

    // IAPOAuxiliaryInputRT
    STDMETHOD_(void, AcceptInput)(
        DWORD dwInputId,
        const APO_CONNECTION_PROPERTY *pInputConnection) override;

    // IAudioSystemEffects3
    STDMETHOD(GetControllableSystemEffectsList)(
        _Outptr_result_buffer_maybenull_(*numEffects) AUDIO_SYSTEMEFFECT **effects, _Out_ UINT *numEffects, _In_opt_ HANDLE event) override;

    STDMETHODIMP SetAudioSystemEffectState(GUID, AUDIO_SYSTEMEFFECT_STATE) override { return S_OK; }

    // IAudioProcessingObjectNotifications
    STDMETHOD(GetApoNotificationRegistrationInfo)(_Out_writes_(*count) APO_NOTIFICATION_DESCRIPTOR **apoNotifications, _Out_ DWORD *count) override;
    STDMETHOD_(void, HandleNotification)(_In_ APO_NOTIFICATION *apoNotification) override;

public:
    UINT64 m_auxiliaryInputId = 0;
    static const CRegAPOProperties<1> sm_RegProperties; // registration properties
    BOOL m_initializedForEffectsDiscovery = FALSE;
    GUID m_audioSignalProcessingMode = GUID_NULL;
    UINT32 m_inputSamplesPerFrame = 0;
    UINT32 m_renderSamplesPerFrame = 0;
    int m_renderSampleRateHz = 0;
    int m_sampleRateHz = 0;
    size_t m_frameSize = 0;
    AecSampleFormat m_inputSampleFormat = AecSampleFormat::kUnknown;
    AecSampleFormat m_outputSampleFormat = AecSampleFormat::kUnknown;
    AecSampleFormat m_renderSampleFormat = AecSampleFormat::kUnknown;

    CComPtr<IMMDevice> m_spCaptureDevice;
    CComPtr<IMMDevice> m_spLoopbackDevice;

    float m_captureEndpointMasterVolume = 0;
    float m_loopbackEndpointMasterVolume = 0;

private:
    // Helper methods for LockForProcess
    HRESULT ValidateAndSetupFormats(
        APO_CONNECTION_DESCRIPTOR **ppInputConnections,
        APO_CONNECTION_DESCRIPTOR **ppOutputConnections);
    void InitializeProcessingBuffers();
    void InitializeSpeexProcessors();

    // Helper method for APOProcess
    void ProcessSpeexFrame();

    CComPtr<IAudioProcessingObjectLoggingService> m_apoLoggingService;
    CComAutoCriticalSection m_speexLock;
    SampleFifo m_speexRenderFifo;
    SampleFifo m_captureFifo;
    SampleFifo m_outputFifo;
    std::vector<float> m_renderScratch;
    std::vector<float> m_speexRenderFrameScratch;
    std::vector<float> m_captureScratch;
    std::vector<float> m_captureFrameScratch;
    std::vector<float> m_outputScratch;
    SpeexRAII::EchoStatePtr m_speexState;
    SpeexRAII::PreprocessStatePtr m_speexPreprocess;
    int m_speexFrameSize = 0;
    std::vector<int16_t> m_speexMic16;
    std::vector<int16_t> m_speexRef16;
    std::vector<int16_t> m_speexOut16;
};
#pragma AVRT_VTABLES_END

OBJECT_ENTRY_AUTO(__uuidof(AecApoMFX), CAecApoMFX)
