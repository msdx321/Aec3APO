//
// AecApoMFX.cpp -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Implementation of CAecApoMFX
//

#include <atlbase.h>
#include <atlcom.h>
#include <atlcoll.h>
#include <atlsync.h>
#include <mmreg.h>
#include <windows.h>
#include <strsafe.h>
#include <initguid.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <resource.h>

#include <float.h>
#include <algorithm>
#include <cmath>

#include "AecApo.h"
#include <devicetopology.h>

#pragma warning(push)
#pragma warning(disable : 4244)
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#pragma warning(pop)

CAecApoMFX::~CAecApoMFX()
{
    if (m_speexPreprocess)
    {
        speex_preprocess_state_destroy(m_speexPreprocess);
        m_speexPreprocess = nullptr;
    }
    if (m_speexState)
    {
        speex_echo_state_destroy(m_speexState);
        m_speexState = nullptr;
    }
}

namespace
{
constexpr int kDefaultSampleRateHz = 48000;
constexpr int kMaxInputChannels = 16;
constexpr float kSampleRateMatchToleranceHz = 1.0f;
constexpr int kSupportedSampleRatesHz[] = { 8000, 16000, 32000, 44100, 48000 };

static bool IsRateSupported(float rate_hz, const int* rates, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (std::fabs(rate_hz - static_cast<float>(rates[i])) < kSampleRateMatchToleranceHz)
        {
            return true;
        }
    }
    return false;
}

static int GetClosestRate(float rate_hz, const int* rates, size_t count, int fallback)
{
    float bestDiff = FLT_MAX;
    int best = fallback;
    for (size_t i = 0; i < count; ++i)
    {
        float diff = std::fabs(rate_hz - static_cast<float>(rates[i]));
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = rates[i];
        }
    }
    return best;
}

static bool IsSupportedAecSampleRate(float rate_hz)
{
    return IsRateSupported(rate_hz, kSupportedSampleRatesHz, ARRAYSIZE(kSupportedSampleRatesHz));
}

static int GetClosestSupportedSampleRate(float rate_hz)
{
    return GetClosestRate(rate_hz, kSupportedSampleRatesHz, ARRAYSIZE(kSupportedSampleRatesHz),
        kDefaultSampleRateHz);
}
} // namespace

static AecSampleFormat GetAecSampleFormat(const UNCOMPRESSEDAUDIOFORMAT& format)
{
    if (format.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
        format.dwBytesPerSampleContainer == 4 &&
        format.dwValidBitsPerSample == 32)
    {
        return AecSampleFormat::kFloat32;
    }

    if (format.guidFormatType == KSDATAFORMAT_SUBTYPE_PCM)
    {
        if (format.dwBytesPerSampleContainer == 2 && format.dwValidBitsPerSample == 16)
        {
            return AecSampleFormat::kPcm16;
        }
        if (format.dwBytesPerSampleContainer == 3 && format.dwValidBitsPerSample == 24)
        {
            return AecSampleFormat::kPcm24Packed;
        }
        if (format.dwBytesPerSampleContainer == 4 && format.dwValidBitsPerSample == 24)
        {
            return AecSampleFormat::kPcm24In32;
        }
        if (format.dwBytesPerSampleContainer == 4 && format.dwValidBitsPerSample == 32)
        {
            return AecSampleFormat::kPcm32;
        }
    }

    return AecSampleFormat::kUnknown;
}

static UINT32 GetBytesPerSample(AecSampleFormat format)
{
    switch (format)
    {
    case AecSampleFormat::kFloat32:
        return 4;
    case AecSampleFormat::kPcm16:
        return 2;
    case AecSampleFormat::kPcm24Packed:
        return 3;
    case AecSampleFormat::kPcm24In32:
    case AecSampleFormat::kPcm32:
        return 4;
    default:
        return 0;
    }
}

static UINT32 GetValidBitsPerSample(AecSampleFormat format)
{
    switch (format)
    {
    case AecSampleFormat::kFloat32:
        return 32;
    case AecSampleFormat::kPcm16:
        return 16;
    case AecSampleFormat::kPcm24Packed:
    case AecSampleFormat::kPcm24In32:
        return 24;
    case AecSampleFormat::kPcm32:
        return 32;
    default:
        return 0;
    }
}

static GUID GetFormatSubtype(AecSampleFormat format)
{
    return (format == AecSampleFormat::kFloat32)
        ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        : KSDATAFORMAT_SUBTYPE_PCM;
}

static int16_t FloatToS16Clamp(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float scaled = v * 32768.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32768.0f) scaled = -32768.0f;
    return static_cast<int16_t>(scaled);
}

static float S16ToFloat(int16_t v)
{
    return static_cast<float>(v) / 32768.0f;
}

static int32_t FloatToS24Clamp(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float scaled = v * 8388608.0f;
    if (scaled > 8388607.0f) scaled = 8388607.0f;
    if (scaled < -8388608.0f) scaled = -8388608.0f;
    return static_cast<int32_t>(scaled);
}

static float S24ToFloat(int32_t v)
{
    return static_cast<float>(v) / 8388608.0f;
}

static int32_t FloatToS32Clamp(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float scaled = v * 2147483648.0f;
    if (scaled > 2147483647.0f) scaled = 2147483647.0f;
    if (scaled < -2147483648.0f) scaled = -2147483648.0f;
    return static_cast<int32_t>(scaled);
}

static float S32ToFloat(int32_t v)
{
    return static_cast<float>(v) / 2147483648.0f;
}

static int32_t ReadPcm24PackedSample(const uint8_t* data, size_t sampleIndex)
{
    const uint8_t* src = data + (sampleIndex * 3);
    int32_t value = static_cast<int32_t>(src[0]) |
        (static_cast<int32_t>(src[1]) << 8) |
        (static_cast<int32_t>(src[2]) << 16);
    if (value & 0x800000)
    {
        value |= ~0xFFFFFF;
    }
    return value;
}

static void WritePcm24PackedSample(uint8_t* data, size_t sampleIndex, int32_t value)
{
    uint8_t* dst = data + (sampleIndex * 3);
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
}

static int32_t SignExtend24(int32_t value)
{
    return (value << 8) >> 8;
}

static void ExtractMonoSamples(const void* input,
    AecSampleFormat format,
    UINT32 frames,
    UINT32 channels,
    bool averageChannels,
    bool inputSilent,
    float* out)
{
    if (!out || frames == 0)
    {
        return;
    }

    if (inputSilent || input == nullptr || channels == 0)
    {
        std::fill(out, out + frames, 0.0f);
        return;
    }

    switch (format)
    {
    case AecSampleFormat::kFloat32:
    {
        const float* in = static_cast<const float*>(input);
        if (averageChannels && channels > 1)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const float* framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (UINT32 ch = 0; ch < channels; ++ch)
                {
                    sum += framePtr[ch];
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                out[frame] = in[frame * channels];
            }
        }
        break;
    }
    case AecSampleFormat::kPcm16:
    {
        const int16_t* in = static_cast<const int16_t*>(input);
        if (averageChannels && channels > 1)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const int16_t* framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (UINT32 ch = 0; ch < channels; ++ch)
                {
                    sum += S16ToFloat(framePtr[ch]);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                out[frame] = S16ToFloat(in[frame * channels]);
            }
        }
        break;
    }
    case AecSampleFormat::kPcm24Packed:
    {
        const uint8_t* in = static_cast<const uint8_t*>(input);
        if (averageChannels && channels > 1)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                float sum = 0.0f;
                size_t baseIndex = static_cast<size_t>(frame) * channels;
                for (UINT32 ch = 0; ch < channels; ++ch)
                {
                    int32_t sample = ReadPcm24PackedSample(in, baseIndex + ch);
                    sum += S24ToFloat(sample);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                size_t sampleIndex = static_cast<size_t>(frame) * channels;
                out[frame] = S24ToFloat(ReadPcm24PackedSample(in, sampleIndex));
            }
        }
        break;
    }
    case AecSampleFormat::kPcm24In32:
    {
        const int32_t* in = static_cast<const int32_t*>(input);
        if (averageChannels && channels > 1)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const int32_t* framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (UINT32 ch = 0; ch < channels; ++ch)
                {
                    sum += S24ToFloat(SignExtend24(framePtr[ch]));
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                out[frame] = S24ToFloat(SignExtend24(in[frame * channels]));
            }
        }
        break;
    }
    case AecSampleFormat::kPcm32:
    {
        const int32_t* in = static_cast<const int32_t*>(input);
        if (averageChannels && channels > 1)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const int32_t* framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (UINT32 ch = 0; ch < channels; ++ch)
                {
                    sum += S32ToFloat(framePtr[ch]);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                out[frame] = S32ToFloat(in[frame * channels]);
            }
        }
        break;
    }
    default:
        std::fill(out, out + frames, 0.0f);
        break;
    }
}

static void WriteMonoSamples(void* output,
    AecSampleFormat format,
    UINT32 frames,
    UINT32 channels,
    const float* mono)
{
    if (!output || !mono || frames == 0 || channels == 0)
    {
        return;
    }

    switch (format)
    {
    case AecSampleFormat::kFloat32:
    {
        float* out = static_cast<float*>(output);
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            float sample = mono[frame];
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                out[frame * channels + ch] = sample;
            }
        }
        break;
    }
    case AecSampleFormat::kPcm16:
    {
        int16_t* out = static_cast<int16_t*>(output);
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            int16_t sample = FloatToS16Clamp(mono[frame]);
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                out[frame * channels + ch] = sample;
            }
        }
        break;
    }
    case AecSampleFormat::kPcm24Packed:
    {
        uint8_t* out = static_cast<uint8_t*>(output);
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            int32_t sample = FloatToS24Clamp(mono[frame]);
            size_t baseIndex = static_cast<size_t>(frame) * channels;
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                WritePcm24PackedSample(out, baseIndex + ch, sample);
            }
        }
        break;
    }
    case AecSampleFormat::kPcm24In32:
    {
        int32_t* out = static_cast<int32_t*>(output);
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            int32_t sample = FloatToS24Clamp(mono[frame]);
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                out[frame * channels + ch] = sample;
            }
        }
        break;
    }
    case AecSampleFormat::kPcm32:
    {
        int32_t* out = static_cast<int32_t*>(output);
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            int32_t sample = FloatToS32Clamp(mono[frame]);
            for (UINT32 ch = 0; ch < channels; ++ch)
            {
                out[frame * channels + ch] = sample;
            }
        }
        break;
    }
    default:
        break;
    }
}

// Static declaration of the APO_REG_PROPERTIES structure
// associated with this APO.  The number in <> brackets is the
// number of IIDs supported by this APO.  If more than one, then additional
// IIDs are added at the end
#pragma warning (disable : 4815)
const AVRT_DATA CRegAPOProperties<1> CAecApoMFX::sm_RegProperties(
    __uuidof(AecApoMFX),                           // clsid of this APO
    L"Aec3Apo",                 // friendly name of this APO
    L"Copyright (c) msdx321",                          // copyright info
    1,                                              // major version #
    0,                                              // minor version #
    __uuidof(IAudioProcessingObject),               // iid of primary interface
    (APO_FLAG) (APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH), // kak check this
    DEFAULT_APOREG_MININPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINPUTCONNECTIONS,
    DEFAULT_APOREG_MINOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINSTANCES
    );


#pragma AVRT_CODE_BEGIN
//-------------------------------------------------------------------------
// Description:
//
//  Do the actual processing of data.
//
// Parameters:
//
//      u32NumInputConnections      - [in] number of input connections
//      ppInputConnections          - [in] pointer to list of input APO_CONNECTION_PROPERTY pointers
//      u32NumOutputConnections      - [in] number of output connections
//      ppOutputConnections         - [in] pointer to list of output APO_CONNECTION_PROPERTY pointers
//
// Return values:
//
//      void
//
// Remarks:
//
//  This function processes data in a manner dependent on the implementing
//  object.  This routine can not fail and can not block, or call any other
//  routine that blocks, or touch pagable memory.
//
STDMETHODIMP_(void) CAecApoMFX::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    UNREFERENCED_PARAMETER(u32NumInputConnections);
    UNREFERENCED_PARAMETER(u32NumOutputConnections);

    const void* inputBuffer = nullptr;
    void* outputBuffer = nullptr;

    ATLASSERT(m_bIsLocked);

    // assert that the number of input and output connectins fits our registration properties
    ATLASSERT(m_pRegProperties->u32MinInputConnections <= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MaxInputConnections >= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MinOutputConnections <= u32NumOutputConnections);
    ATLASSERT(m_pRegProperties->u32MaxOutputConnections >= u32NumOutputConnections);

    ATLASSERT(ppInputConnections[0]->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);
    ATLASSERT(ppOutputConnections[0]->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);

    APO_CONNECTION_PROPERTY_V2* inConnection = reinterpret_cast<APO_CONNECTION_PROPERTY_V2*>(ppInputConnections[0]);
    APO_CONNECTION_PROPERTY_V2* outConnection = reinterpret_cast<APO_CONNECTION_PROPERTY_V2*>(ppOutputConnections[0]);
    UNREFERENCED_PARAMETER(inConnection);
    UNREFERENCED_PARAMETER(outConnection);

    // check APO_BUFFER_FLAGS.
    switch( ppInputConnections[0]->u32BufferFlags )
    {
        case BUFFER_INVALID:
        {
            ATLASSERT(false);  // invalid flag - should never occur.  don't do anything.
            break;
        }
        case BUFFER_VALID:
        case BUFFER_SILENT:
        {
            inputBuffer = reinterpret_cast<const void*>(ppInputConnections[0]->pBuffer);
            outputBuffer = reinterpret_cast<void*>(ppOutputConnections[0]->pBuffer);

            UINT32 frames = ppInputConnections[0]->u32ValidFrameCount;
            bool inputSilent = (ppInputConnections[0]->u32BufferFlags == BUFFER_SILENT);

            if (m_captureScratch.size() < frames)
            {
                m_captureScratch.resize(frames);
            }
            ExtractMonoSamples(inputBuffer,
                m_inputSampleFormat,
                frames,
                m_inputSamplesPerFrame,
                false,
                inputSilent,
                m_captureScratch.data());

            if (!m_speexState || m_frameSize == 0)
            {
                if (m_outputScratch.size() < frames)
                {
                    m_outputScratch.resize(frames);
                }
                std::copy(m_captureScratch.begin(), m_captureScratch.begin() + frames,
                    m_outputScratch.begin());
                WriteMonoSamples(outputBuffer,
                    m_outputSampleFormat,
                    frames,
                    m_u32SamplesPerFrame,
                    m_outputScratch.data());
            }
            else
            {
                m_captureFifo.Push(m_captureScratch.data(), frames);

                if (m_captureFrameScratch.size() < m_frameSize)
                {
                    m_captureFrameScratch.resize(m_frameSize);
                }

                const size_t frameSize = m_frameSize;
                std::vector<float>& captureFrameScratch = m_captureFrameScratch;
                std::vector<float>& renderFrameScratch = m_speexRenderFrameScratch;
                std::vector<int16_t>& speexMic16 = m_speexMic16;
                std::vector<int16_t>& speexRef16 = m_speexRef16;
                std::vector<int16_t>& speexOut16 = m_speexOut16;

                // Process full 10 ms blocks through Speex AEC.
                while (m_captureFifo.Count() >= frameSize)
                {
                    m_captureFifo.Pop(captureFrameScratch.data(), frameSize);

                    size_t got = 0;
                    {
                        CComCritSecLock<CComAutoCriticalSection> lock(m_speexLock);
                        got = m_speexRenderFifo.Pop(renderFrameScratch.data(), frameSize);
                    }
                    if (got < frameSize)
                    {
                        std::fill(renderFrameScratch.begin() + got,
                            renderFrameScratch.end(),
                            0.0f);
                    }

                    for (size_t i = 0; i < frameSize; ++i)
                    {
                        speexMic16[i] = FloatToS16Clamp(captureFrameScratch[i]);
                        speexRef16[i] = FloatToS16Clamp(renderFrameScratch[i]);
                    }
                    speex_echo_cancellation(m_speexState,
                        speexMic16.data(),
                        speexRef16.data(),
                        speexOut16.data());
                    if (m_speexPreprocess)
                    {
                        speex_preprocess_run(m_speexPreprocess, speexOut16.data());
                    }
                    for (size_t i = 0; i < frameSize; ++i)
                    {
                        captureFrameScratch[i] = S16ToFloat(speexOut16[i]);
                    }

                    m_outputFifo.Push(captureFrameScratch.data(), frameSize);
                }

                // Emit processed samples; if not enough yet, fall back to input.
                if (m_outputScratch.size() < frames)
                {
                    m_outputScratch.resize(frames);
                }
                size_t produced = m_outputFifo.Pop(m_outputScratch.data(), frames);
                if (produced < frames)
                {
                    for (UINT32 frame = static_cast<UINT32>(produced); frame < frames; ++frame)
                    {
                        m_outputScratch[frame] = m_captureScratch[frame];
                    }
                }

                WriteMonoSamples(outputBuffer,
                    m_outputSampleFormat,
                    frames,
                    m_u32SamplesPerFrame,
                    m_outputScratch.data());
            }

            // Set the valid frame count.
            ppOutputConnections[0]->u32ValidFrameCount = ppInputConnections[0]->u32ValidFrameCount;
            ppOutputConnections[0]->u32BufferFlags = ppInputConnections[0]->u32BufferFlags;

            break;
        }
        default:
        {
            ATLASSERT(false);  // invalid flag - should never occur
            break;
        }
    } // switch

} // APOProcess
#pragma AVRT_CODE_END

//-------------------------------------------------------------------------
// Description:
//
// Parameters:
//
//      pTime                       - [out] hundreds-of-nanoseconds
//
// Return values:
//
//      S_OK on success, a failure code on failure
STDMETHODIMP CAecApoMFX::GetLatency(HNSTIME* pTime)  
{  
    ASSERT_NONREALTIME();
    HRESULT hr = S_OK;
  
    IF_TRUE_ACTION_JUMP(NULL == pTime, hr = E_POINTER, Exit);  
  
    *pTime = 0;

Exit:  
    return hr;  
}


//-------------------------------------------------------------------------
// Description:
//
//  Verifies that the APO is ready to process and locks its state if so.
//
// Parameters:
//
//      u32NumInputConnections - [in] number of input connections attached to this APO
//      ppInputConnections - [in] connection descriptor of each input connection attached to this APO
//      u32NumOutputConnections - [in] number of output connections attached to this APO
//      ppOutputConnections - [in] connection descriptor of each output connection attached to this APO
//
// Return values:
//
//      S_OK                                Object is locked and ready to process.
//      E_POINTER                           Invalid pointer passed to function.
//      APOERR_INVALID_CONNECTION_FORMAT    Invalid connection format.
//      APOERR_NUM_CONNECTIONS_INVALID      Number of input or output connections is not valid on
//                                          this APO.
STDMETHODIMP CAecApoMFX::LockForProcess(UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR** ppInputConnections,  
    UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    ASSERT_NONREALTIME();
    HRESULT hr = S_OK;

    UNCOMPRESSEDAUDIOFORMAT  uncompAudioFormat;
    UNCOMPRESSEDAUDIOFORMAT  uncompInputFormat;

    // fill in the samples per frame for the output (since APO_FLAG_SAMPLESPERFRAME_MUST_MATCH is not selected)
    // There are two potentially different samples per frame values here. The input, which will be interleaved + primary. 
    // And the output, which is just the primary. Because this is used for clearing the zeroing the output buffer, we're going
    // to fill it in with the output samples per frame. ProcessBuffer has both.
    hr = ppOutputConnections[0]->pFormat->GetUncompressedAudioFormat(&uncompAudioFormat);
    IF_FAILED_JUMP(hr, Exit);

    m_u32SamplesPerFrame = uncompAudioFormat.dwSamplesPerFrame;
    m_outputSampleFormat = GetAecSampleFormat(uncompAudioFormat);
    hr = ppInputConnections[0]->pFormat->GetUncompressedAudioFormat(&uncompInputFormat);
    IF_FAILED_JUMP(hr, Exit);
    m_inputSamplesPerFrame = uncompInputFormat.dwSamplesPerFrame;
    m_inputSampleFormat = GetAecSampleFormat(uncompInputFormat);
    if (m_inputSampleFormat == AecSampleFormat::kUnknown ||
        m_outputSampleFormat == AecSampleFormat::kUnknown)
    {
        hr = E_INVALIDARG;
        goto Exit;
    }

    if (!IsSupportedAecSampleRate(uncompInputFormat.fFramesPerSecond))
    {
        hr = E_INVALIDARG;
        goto Exit;
    }

    m_sampleRateHz = GetClosestSupportedSampleRate(uncompInputFormat.fFramesPerSecond);
    m_frameSize = static_cast<size_t>(m_sampleRateHz / 100);
    m_captureFifo.Init(static_cast<size_t>(m_sampleRateHz));
    m_outputFifo.Init(static_cast<size_t>(m_sampleRateHz));
    m_speexRenderFifo.Init(static_cast<size_t>(m_sampleRateHz));

    if (m_speexPreprocess)
    {
        speex_preprocess_state_destroy(m_speexPreprocess);
        m_speexPreprocess = nullptr;
    }
    if (m_speexState)
    {
        speex_echo_state_destroy(m_speexState);
        m_speexState = nullptr;
    }
    m_speexFrameSize = static_cast<int>(m_frameSize);
    if (m_speexFrameSize > 0)
    {
        int filterLen = m_speexFrameSize * 10; // 100 ms tail
        m_speexState = speex_echo_state_init(m_speexFrameSize, filterLen);
        if (m_speexState)
        {
            speex_echo_ctl(m_speexState, SPEEX_ECHO_SET_SAMPLING_RATE, &m_sampleRateHz);
            m_speexPreprocess = speex_preprocess_state_init(m_speexFrameSize, m_sampleRateHz);
            if (m_speexPreprocess)
            {
                speex_preprocess_ctl(m_speexPreprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, m_speexState);
                int denoise = 1;
                speex_preprocess_ctl(m_speexPreprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
            }
            m_speexMic16.assign(m_frameSize, 0);
            m_speexRef16.assign(m_frameSize, 0);
            m_speexOut16.assign(m_frameSize, 0);
            m_speexRenderFrameScratch.assign(m_frameSize, 0.0f);
        }
    }

    hr = CBaseAudioProcessingObject::LockForProcess(u32NumInputConnections,
        ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    IF_FAILED_JUMP(hr, Exit);
    

Exit:
    return hr;
}

// The method that this long comment refers to is "Initialize()"
//-------------------------------------------------------------------------
// Description:
//
//  Generic initialization routine for APOs.
//
// Parameters:
//
//     cbDataSize - [in] the size in bytes of the initialization data.
//     pbyData - [in] initialization data specific to this APO
//
// Return values:
//
//     S_OK                         Successful completion.
//     E_POINTER                    Invalid pointer passed to this function.
//     E_INVALIDARG                 Invalid argument
//     AEERR_ALREADY_INITIALIZED    APO is already initialized
//
// Remarks:
//
//  This method initializes the APO.  The data is variable length and
//  should have the form of:
//
//    struct MyAPOInitializationData
//    {
//        APOInitBaseStruct APOInit;
//        ... // add additional fields here
//    };
//
//  If the APO needs no initialization or needs no data to initialize
//  itself, it is valid to pass NULL as the pbyData parameter and 0 as
//  the cbDataSize parameter.
//
//  As part of designing an APO, decide which parameters should be
//  immutable (set once during initialization) and which mutable (changeable
//  during the lifetime of the APO instance).  Immutable parameters must
//  only be specifiable in the Initialize call; mutable parameters must be
//  settable via methods on whichever parameter control interface(s) your
//  APO provides. Mutable values should either be set in the initialize
//  method (if they are required for proper operation of the APO prior to
//  LockForProcess) or default to reasonable values upon initialize and not
//  be required to be set before LockForProcess.
//
//  Within the mutable parameters, you must also decide which can be changed
//  while the APO is locked for processing and which cannot.
//
//  All parameters should be considered immutable as a first choice, unless
//  there is a specific scenario which requires them to be mutable; similarly,
//  no mutable parameters should be changeable while the APO is locked, unless
//  a specific scenario requires them to be.  Following this guideline will
//  simplify the APO's state diagram and implementation and prevent certain
//  types of bug.
//
//  If a parameter changes the APOs latency or MaxXXXFrames values, it must be
//  immutable.
//
//  The default version of this function uses no initialization data, but does verify
//  the passed parameters and set the m_bIsInitialized member to true.
//
//  Note: This method may not be called from a real-time processing thread.
//

HRESULT CAecApoMFX::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    HRESULT                     hr = S_OK;


    IF_TRUE_ACTION_JUMP( ((NULL == pbyData) && (0 != cbDataSize)), hr = E_INVALIDARG, Exit);
    IF_TRUE_ACTION_JUMP( ((NULL != pbyData) && (0 == cbDataSize)), hr = E_INVALIDARG, Exit);

    if (cbDataSize == sizeof(APOInitSystemEffects3))
    {
        //
        // pbyData contains APOInitSystemEffects3 structure describing the microphone endpoint
        //
        APOInitSystemEffects3* papoSysFxInit3 = (APOInitSystemEffects3*)pbyData;
        m_initializedForEffectsDiscovery = papoSysFxInit3->InitializeForDiscoveryOnly;

        // Support for all processing modes; log when not COMMUNICATIONS.
        m_audioSignalProcessingMode = papoSysFxInit3->AudioProcessingMode;

        // Register for notification of endpoint volume change in GetApoNotificationRegistrationInfo      
        // Keep a reference to the device that will be registering for endpoint volume notifcations. 

        IF_TRUE_ACTION_JUMP(papoSysFxInit3->pDeviceCollection == nullptr, hr = E_INVALIDARG, Exit);
        // Get the endpoint on which this APO has been created. It is the last device in the device collection.
        UINT32 numDevices;
        hr = papoSysFxInit3->pDeviceCollection->GetCount(&numDevices);
        IF_FAILED_JUMP(hr, Exit);
        IF_TRUE_ACTION_JUMP(numDevices <= 0, hr = E_INVALIDARG, Exit);

        hr = papoSysFxInit3->pDeviceCollection->Item(numDevices - 1, &m_spCaptureDevice);
        IF_FAILED_JUMP(hr, Exit);

        m_bIsInitialized = true;

        // Try to get the logging service, but ignore errors as failure to do logging it is not fatal.
        if(SUCCEEDED(papoSysFxInit3->pServiceProvider->QueryService(SID_AudioProcessingObjectLoggingService, IID_PPV_ARGS(&m_apoLoggingService))))
        {
            m_apoLoggingService->ApoLog(APO_LOG_LEVEL_INFO, L"CAecApoMFX::Initialize called with APOInitSystemEffects3.");
        }        
    }
    else if (cbDataSize == sizeof(APOInitSystemEffects2))
    {
        //
        // pbyData contains APOInitSystemEffects2 structure describing the microphone endpoint
        //
        APOInitSystemEffects2* papoSysFxInit2 = (APOInitSystemEffects2*)pbyData;
        m_initializedForEffectsDiscovery = papoSysFxInit2->InitializeForDiscoveryOnly;

        // Support for all processing modes; log when not COMMUNICATIONS.
        m_audioSignalProcessingMode = papoSysFxInit2->AudioProcessingMode;

        m_bIsInitialized = true;
    }
    else
    {
        hr = E_INVALIDARG;
    }

Exit:
    return hr;
}

//-------------------------------------------------------------------------
// Description:
//
//
//
// Parameters:
//
//
//
// Return values:
//
//
//
// Remarks:
//
//
STDMETHODIMP CAecApoMFX::GetEffectsList(_Outptr_result_buffer_maybenull_(*pcEffects) LPGUID *ppEffectsIds, _Out_ UINT *pcEffects, _In_ HANDLE Event)
{
    UNREFERENCED_PARAMETER(Event);

    *ppEffectsIds = NULL;
    *pcEffects = 0;

    if (m_audioSignalProcessingMode == AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS)
    {
        // Return the list of effects implemented by this APO for COMMUNICATIONS processing mode  
        static const GUID effectsList[] = { AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION };

        *ppEffectsIds = (LPGUID)CoTaskMemAlloc(sizeof(effectsList));
        if (!*ppEffectsIds)
        {
            return E_OUTOFMEMORY;
        }
        *pcEffects = ARRAYSIZE(effectsList);
        CopyMemory(*ppEffectsIds, effectsList, sizeof(effectsList));
    }

    return S_OK;
}

STDMETHODIMP CAecApoMFX::GetControllableSystemEffectsList(_Outptr_result_buffer_maybenull_(*numEffects) AUDIO_SYSTEMEFFECT** effects, _Out_ UINT* numEffects, _In_opt_ HANDLE event)
{
    UNREFERENCED_PARAMETER(event);

    if (effects == nullptr || numEffects == nullptr)
    {
        return E_POINTER;
    }

    *effects = nullptr;
    *numEffects = 0;

    if (m_audioSignalProcessingMode == AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS)
    {
        // Return the list of effects implemented by this APO for COMMUNICATIONS processing mode
        static const GUID effectsList[] = {AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION};

        AUDIO_SYSTEMEFFECT* audioEffects = static_cast<AUDIO_SYSTEMEFFECT*>(
            CoTaskMemAlloc(ARRAYSIZE(effectsList) * sizeof(AUDIO_SYSTEMEFFECT)));
        if (audioEffects == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        for (UINT i = 0; i < ARRAYSIZE(effectsList); i++)
        {
            audioEffects[i].id = effectsList[i];
            audioEffects[i].state = AUDIO_SYSTEMEFFECT_STATE_ON;
            audioEffects[i].canSetState = FALSE;
        }

        *numEffects = ARRAYSIZE(effectsList);
        *effects = audioEffects;
    }

    return S_OK;
}

HRESULT IsInputFormatSupportedForAec(IAudioMediaType* pMediaType, BOOL * pSupported)
{
    ASSERT_NONREALTIME();

    HRESULT hr = S_OK;
    UNCOMPRESSEDAUDIOFORMAT format;

    IF_TRUE_ACTION_JUMP((pMediaType == nullptr || pSupported == nullptr), hr = E_INVALIDARG, exit);
    hr = pMediaType->GetUncompressedAudioFormat(&format);
    IF_FAILED_JUMP(hr, exit);

    AecSampleFormat sampleFormat = GetAecSampleFormat(format);
    *pSupported = sampleFormat != AecSampleFormat::kUnknown &&
                  IsSupportedAecSampleRate(format.fFramesPerSecond) &&
                  format.dwSamplesPerFrame > 0 &&
                  format.dwSamplesPerFrame <= kMaxInputChannels;

exit:
    return hr;
}

HRESULT IsOutputFormatSupportedForAec(IAudioMediaType* pMediaType, BOOL * pSupported)
{
    ASSERT_NONREALTIME();

    HRESULT hr = S_OK;
    UNCOMPRESSEDAUDIOFORMAT format;

    IF_TRUE_ACTION_JUMP((pMediaType == nullptr || pSupported == nullptr), hr = E_INVALIDARG, exit);
    hr = pMediaType->GetUncompressedAudioFormat(&format);
    IF_FAILED_JUMP(hr, exit);

    AecSampleFormat sampleFormat = GetAecSampleFormat(format);
    *pSupported = sampleFormat != AecSampleFormat::kUnknown &&
                  IsSupportedAecSampleRate(format.fFramesPerSecond) &&
                  format.dwSamplesPerFrame == 1; // mono output

exit:
    return hr;
}

HRESULT
CreatePreferredInputMediaType(IAudioMediaType** ppMediaType,
                              UINT32 requestedInputChannelCount,
                              float requestedSampleRate,
                              AecSampleFormat requestedFormat)
{
    ASSERT_NONREALTIME();

    AecSampleFormat formatType = (requestedFormat != AecSampleFormat::kUnknown)
        ? requestedFormat
        : AecSampleFormat::kFloat32;
    float sampleRate = static_cast<float>(GetClosestSupportedSampleRate(requestedSampleRate));
    UNCOMPRESSEDAUDIOFORMAT format = {};
    format.guidFormatType = GetFormatSubtype(formatType);
    format.dwSamplesPerFrame = 1;
    format.dwBytesPerSampleContainer = GetBytesPerSample(formatType);
    format.dwValidBitsPerSample = GetValidBitsPerSample(formatType);
    format.fFramesPerSecond = sampleRate;
    format.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;

    // Match the channel count of the input if it is less than 16
    if (requestedInputChannelCount <= kMaxInputChannels)
    {
        format.dwSamplesPerFrame = requestedInputChannelCount;
        format.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;
    }
    
    return CreateAudioMediaTypeFromUncompressedAudioFormat(&format, ppMediaType);
}

HRESULT
CreatePreferredOutputMediaType(IAudioMediaType** ppMediaType,
                               float requestedSampleRate,
                               AecSampleFormat requestedFormat)
{
    ASSERT_NONREALTIME();

    AecSampleFormat formatType = (requestedFormat != AecSampleFormat::kUnknown)
        ? requestedFormat
        : AecSampleFormat::kFloat32;
    float sampleRate = static_cast<float>(GetClosestSupportedSampleRate(requestedSampleRate));
    UNCOMPRESSEDAUDIOFORMAT format = {};
    format.guidFormatType = GetFormatSubtype(formatType);
    format.dwSamplesPerFrame = 1;
    format.dwBytesPerSampleContainer = GetBytesPerSample(formatType);
    format.dwValidBitsPerSample = GetValidBitsPerSample(formatType);
    format.fFramesPerSecond = sampleRate;
    format.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;

    return CreateAudioMediaTypeFromUncompressedAudioFormat(&format, ppMediaType);
}

//-------------------------------------------------------------------------
// Description:
//
//
//
// Parameters:
//
//
//
// Return values:
//
//
//
// Remarks:
//
//
STDMETHODIMP CAecApoMFX::IsInputFormatSupported(IAudioMediaType *pOutputFormat, IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat)
{
    ASSERT_NONREALTIME();
    HRESULT hResult;

    IF_TRUE_ACTION_JUMP((NULL == pRequestedInputFormat) || (NULL == ppSupportedInputFormat), hResult = E_POINTER, Exit);
    *ppSupportedInputFormat = NULL;

    // This method here is called in the context of the MIC endpoint
    // There are 2 supported scenarios
    // - The AEC APO can handle any mic format
    // - The AEC APO can support exactly 1 input format
    //
    // For the purposes of this sample AEC APO, we support common voice sample rates and PCM/float formats.
    // The APO can accept up to 16 channels at the input and will output mono audio.
    //

    if (pOutputFormat)
    {
        // Is this a valid format that we support at the output?
        BOOL bSupportedOut = FALSE;
        hResult = IsOutputFormatSupportedForAec(pOutputFormat, &bSupportedOut);
        IF_FAILED_JUMP(hResult, Exit);
        if (!bSupportedOut)
        {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    } 
    
    BOOL bSupported = FALSE;
    hResult = IsInputFormatSupportedForAec(pRequestedInputFormat, &bSupported);
    IF_FAILED_JUMP(hResult, Exit);

    if (!bSupported)
    {
        UNCOMPRESSEDAUDIOFORMAT requestedFormat = {};
        float requestedRate = static_cast<float>(kDefaultSampleRateHz);
        AecSampleFormat requestedSampleFormat = AecSampleFormat::kUnknown;
        if (SUCCEEDED(pRequestedInputFormat->GetUncompressedAudioFormat(&requestedFormat)))
        {
            requestedRate = requestedFormat.fFramesPerSecond;
            requestedSampleFormat = GetAecSampleFormat(requestedFormat);
        }
        hResult = CreatePreferredInputMediaType(ppSupportedInputFormat,
            pRequestedInputFormat->GetAudioFormat()->nChannels,
            requestedRate,
            requestedSampleFormat);
        IF_FAILED_JUMP(hResult, Exit);
        return S_FALSE;
    }
   
    pRequestedInputFormat->AddRef();
    *ppSupportedInputFormat = pRequestedInputFormat;

Exit:

    return hResult;
}

//-------------------------------------------------------------------------
// Description:
//
//
//
// Parameters:
//
//
//
// Return values:
//
//
//
// Remarks:
//
//
STDMETHODIMP CAecApoMFX::IsOutputFormatSupported(IAudioMediaType *pInputFormat, IAudioMediaType *pRequestedOutputFormat, IAudioMediaType **ppSupportedOutputFormat)
{
    ASSERT_NONREALTIME();
    HRESULT hResult;

    IF_TRUE_ACTION_JUMP((NULL == pRequestedOutputFormat) || (NULL == ppSupportedOutputFormat), hResult = E_POINTER, Exit);
    *ppSupportedOutputFormat = NULL;

    if (pInputFormat != nullptr)
    {
        BOOL bSupportedIn = FALSE;
        hResult = IsInputFormatSupportedForAec(pInputFormat, &bSupportedIn);
        IF_FAILED_JUMP(hResult, Exit);
        if (!bSupportedIn)
        {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    }

    BOOL bSupported = FALSE;
    hResult = IsOutputFormatSupportedForAec(pRequestedOutputFormat, &bSupported);
    IF_FAILED_JUMP(hResult, Exit);

    if (!bSupported)
    {
        UNCOMPRESSEDAUDIOFORMAT requestedFormat = {};
        float requestedRate = static_cast<float>(kDefaultSampleRateHz);
        AecSampleFormat requestedSampleFormat = AecSampleFormat::kUnknown;
        if (SUCCEEDED(pRequestedOutputFormat->GetUncompressedAudioFormat(&requestedFormat)))
        {
            requestedRate = requestedFormat.fFramesPerSecond;
            requestedSampleFormat = GetAecSampleFormat(requestedFormat);
        }
        hResult = CreatePreferredOutputMediaType(ppSupportedOutputFormat,
            requestedRate,
            requestedSampleFormat);
        IF_FAILED_JUMP(hResult, Exit);
        return S_FALSE;
    }

    pRequestedOutputFormat->AddRef();
    *ppSupportedOutputFormat = pRequestedOutputFormat;

Exit:

    return hResult;
}

STDMETHODIMP
CAecApoMFX::AddAuxiliaryInput(
    DWORD dwInputId,
    UINT32 cbDataSize,
    BYTE *pbyData,
    APO_CONNECTION_DESCRIPTOR * pInputConnection
)
{
    HRESULT hResult = S_OK;
    UNCOMPRESSEDAUDIOFORMAT renderFormat = {};

    ASSERT_NONREALTIME();

    IF_TRUE_ACTION_JUMP(m_bIsLocked, hResult = APOERR_APO_LOCKED, Exit);
    IF_TRUE_ACTION_JUMP(!m_bIsInitialized, hResult = APOERR_NOT_INITIALIZED, Exit);

    BOOL bSupported = FALSE;
    hResult = IsInputFormatSupportedForAec(pInputConnection->pFormat, &bSupported);
    IF_FAILED_JUMP(hResult, Exit);
    IF_TRUE_ACTION_JUMP(!bSupported, hResult = APOERR_FORMAT_NOT_SUPPORTED, Exit);

    hResult = pInputConnection->pFormat->GetUncompressedAudioFormat(&renderFormat);
    IF_FAILED_JUMP(hResult, Exit);
    m_renderSamplesPerFrame = renderFormat.dwSamplesPerFrame;
    m_renderSampleRateHz = GetClosestSupportedSampleRate(renderFormat.fFramesPerSecond);
    m_renderSampleFormat = GetAecSampleFormat(renderFormat);

    // This APO can only handle 1 auxiliary input
    IF_TRUE_ACTION_JUMP(m_auxiliaryInputId != 0, hResult = APOERR_NUM_CONNECTIONS_INVALID, Exit);

    m_auxiliaryInputId = dwInputId;

    IF_TRUE_ACTION_JUMP( ((NULL == pbyData) && (0 != cbDataSize)), hResult = E_INVALIDARG, Exit);
    IF_TRUE_ACTION_JUMP( ((NULL != pbyData) && (0 == cbDataSize)), hResult = E_INVALIDARG, Exit);
    if (cbDataSize == sizeof(APOInitSystemEffects3))
    {
        //
        // pbyData contains APOInitSystemEffects3 structure describing the loopback endpoint
        //
        APOInitSystemEffects3* papoSysFxInit3 = (APOInitSystemEffects3*)pbyData;

        // Register for notification in GetApoNotificationRegistrationInfo

        // Keep a reference to the loopback device that will be registering for endpoint volume notifcations.

        IF_TRUE_ACTION_JUMP(papoSysFxInit3->pDeviceCollection == nullptr, hResult = E_INVALIDARG, Exit);
        UINT32 numDevices;
        hResult = papoSysFxInit3->pDeviceCollection->GetCount(&numDevices);
        IF_FAILED_JUMP(hResult, Exit);
        IF_TRUE_ACTION_JUMP(numDevices <= 0, hResult = E_INVALIDARG, Exit);

        hResult = papoSysFxInit3->pDeviceCollection->Item(numDevices - 1, &m_spLoopbackDevice);
        IF_FAILED_JUMP(hResult, Exit);

    }
    else
    {
        //
        // pbyData contains APOInitSystemEffects2 structure describing the render endpoint
        //
    }

    // Signal to AEC algorithm that there is a reference audio stream

Exit:
    return hResult;
}

STDMETHODIMP
CAecApoMFX::RemoveAuxiliaryInput(DWORD dwInputId)
{
    HRESULT hResult = S_OK;
    ASSERT_NONREALTIME();

    IF_TRUE_ACTION_JUMP(m_bIsLocked, hResult = APOERR_APO_LOCKED, Exit);
    IF_TRUE_ACTION_JUMP(!m_bIsInitialized, hResult = APOERR_NOT_INITIALIZED, Exit);

    // This APO can only handle 1 auxiliary input
    IF_TRUE_ACTION_JUMP(m_auxiliaryInputId != dwInputId, hResult = APOERR_INVALID_INPUTID, Exit);

    m_auxiliaryInputId = 0;
    m_renderSamplesPerFrame = 0;
    m_renderSampleRateHz = 0;
    m_renderSampleFormat = AecSampleFormat::kUnknown;

    // Signal to AEC algorithm that there is no longer any reference audio stream

Exit:
    return hResult;
}

STDMETHODIMP
CAecApoMFX::IsInputFormatSupported(IAudioMediaType* pRequestedInputFormat,
                                   IAudioMediaType** ppSupportedInputFormat)
{
    ASSERT_NONREALTIME();
    HRESULT hResult = S_OK;

    IF_TRUE_ACTION_JUMP((NULL == pRequestedInputFormat) || (NULL == ppSupportedInputFormat), hResult = E_POINTER, Exit);

    BOOL bSupported = FALSE;
    hResult = IsInputFormatSupportedForAec(pRequestedInputFormat, &bSupported);
    IF_FAILED_JUMP(hResult, Exit);

    if (!bSupported)
    {
        UNCOMPRESSEDAUDIOFORMAT requestedFormat = {};
        float requestedRate = static_cast<float>(kDefaultSampleRateHz);
        AecSampleFormat requestedSampleFormat = AecSampleFormat::kUnknown;
        if (SUCCEEDED(pRequestedInputFormat->GetUncompressedAudioFormat(&requestedFormat)))
        {
            requestedRate = requestedFormat.fFramesPerSecond;
            requestedSampleFormat = GetAecSampleFormat(requestedFormat);
        }
        hResult = CreatePreferredInputMediaType(ppSupportedInputFormat,
            pRequestedInputFormat->GetAudioFormat()->nChannels,
            requestedRate,
            requestedSampleFormat);
        IF_FAILED_JUMP(hResult, Exit);
        return S_FALSE;
    }

    pRequestedInputFormat->AddRef();
    *ppSupportedInputFormat = pRequestedInputFormat;

Exit:
    return hResult;
}

// IAPOAuxiliaryInputRT
STDMETHODIMP_(void)
CAecApoMFX::AcceptInput(DWORD dwInputId,
                        const APO_CONNECTION_PROPERTY * pInputConnection)
{
    ASSERT_REALTIME();
    ATLASSERT(m_bIsInitialized);
    ATLASSERT(m_bIsLocked);

    UNREFERENCED_PARAMETER(dwInputId);
    ATLASSERT(pInputConnection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);
    ATLASSERT(dwInputId == m_auxiliaryInputId);

    // Check connectionV2->property.u32BufferFlags to see whether loopback buffer is silent
    // Provide loopback buffer and timestamp to AEC algorithm
    UINT32 frames = pInputConnection->u32ValidFrameCount;
    if (m_renderScratch.size() < frames)
    {
        m_renderScratch.resize(frames);
    }

    bool inputSilent = (pInputConnection->u32BufferFlags == BUFFER_SILENT ||
        pInputConnection->pBuffer == 0);
    UINT32 renderChannels = (m_renderSamplesPerFrame != 0) ? m_renderSamplesPerFrame : 1;
    ExtractMonoSamples(reinterpret_cast<const void*>(pInputConnection->pBuffer),
        m_renderSampleFormat,
        frames,
        renderChannels,
        true,
        inputSilent,
        m_renderScratch.data());

    if (m_speexState)
    {
        CComCritSecLock<CComAutoCriticalSection> lock(m_speexLock);
        m_speexRenderFifo.Push(m_renderScratch.data(), frames);
    }

}

STDMETHODIMP CAecApoMFX::GetApoNotificationRegistrationInfo(_Out_writes_(*count) APO_NOTIFICATION_DESCRIPTOR** apoNotifications, _Out_ DWORD* count)
{
    *apoNotifications = nullptr;
    *count = 0;
    // Placeholder: no endpoint notifications yet.

    return S_OK;
}

static bool IsSameEndpointId(IMMDevice* device1, IMMDevice* device2)
{
    bool isSameEndpointId = false;

    CComHeapPtr<WCHAR> deviceId1;
    if (SUCCEEDED(device1->GetId(&deviceId1)))
    {
        CComHeapPtr<WCHAR> deviceId2;
        if (SUCCEEDED(device2->GetId(&deviceId2)))
        {
            isSameEndpointId = (CompareStringOrdinal(deviceId1, -1, deviceId2, -1, TRUE) == CSTR_EQUAL);
        }
    }
    return isSameEndpointId;
}

// HandleNotification is called whenever there is a change that matches any of the
// APO_NOTIFICATION_DESCRIPTOR elements in the array that was returned by GetApoNotificationRegistrationInfo.
// Note that the APO will have to query each property once to get its initial value because this method is
// only invoked when any of the properties have changed.
STDMETHODIMP_(void) CAecApoMFX::HandleNotification(_In_ APO_NOTIFICATION* /* apoNotification */)
{
    // Handle endpoint volume change
    /* 
    if (apoNotification->type == APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE)
    {
        if (IsSameEndpointId(apoNotification->audioEndpointVolumeChange.endpoint, m_spCaptureDevice))
        {            
            m_captureEndpointMasterVolume = apoNotification->audioEndpointVolumeChange.volume->fMasterVolume;
        }
        else if (IsSameEndpointId(apoNotification->audioEndpointVolumeChange.endpoint, m_spLoopbackDevice))
        {
            m_loopbackEndpointMasterVolume = apoNotification->audioEndpointVolumeChange.volume->fMasterVolume;
        }
    }
    else if (apoNotification->type == APO_NOTIFICATION_TYPE_ENDPOINT_PROPERTY_CHANGE)
    {

    }
    else if(apoNotification->type == APO_NOTIFICATION_TYPE_SYSTEM_EFFECTS_PROPERTY_CHANGE)
    {

    } 
    */
}

