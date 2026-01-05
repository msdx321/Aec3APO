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
#include <array>
#include <cmath>

#include "AecApo.h"
#include "SampleConverter.h"
#include <devicetopology.h>

#include "speex/speex_echo.h"
#include "speex/speex_resampler.h"
#include "rnnoise.h"

CAecApoMFX::~CAecApoMFX()
{
    // RAII unique_ptr handles cleanup automatically
}

namespace
{
    constexpr int kDefaultSampleRateHz = 48000;
    constexpr int kMaxInputChannels = 16;
    constexpr float kSampleRateMatchToleranceHz = 1.0f;
    constexpr std::array<int, 5> kSupportedSampleRatesHz = {8000, 16000, 32000, 44100, 48000};

    // Speex configuration constants
    constexpr int kFilterTailMultiplier = 10;  // 100ms tail (10 * 10ms frames)
    constexpr int kFrameDurationDivisor = 100; // 10ms frames (sampleRate / 100)
    constexpr int kRnnoiseSampleRateHz = 48000;
    constexpr float kRnnoiseVadThreshold = 0.6f;
    constexpr int kRnnoiseVadGraceMs = 200;
    constexpr float kRnnoisePcmScale = 32768.0f;
    constexpr float kRnnoisePcmInvScale = 1.0f / 32768.0f;

    static bool IsSupportedAecSampleRate(float rate_hz)
    {
        return std::any_of(kSupportedSampleRatesHz.begin(), kSupportedSampleRatesHz.end(),
                           [rate_hz](int rate)
                           {
                               return std::fabs(rate_hz - static_cast<float>(rate)) < kSampleRateMatchToleranceHz;
                           });
    }

    static int GetClosestSupportedSampleRate(float rate_hz)
    {
        auto closest = std::min_element(kSupportedSampleRatesHz.begin(), kSupportedSampleRatesHz.end(),
                                        [rate_hz](int a, int b)
                                        {
                                            float diffA = std::fabs(rate_hz - static_cast<float>(a));
                                            float diffB = std::fabs(rate_hz - static_cast<float>(b));
                                            return diffA < diffB;
                                        });

        return closest != kSupportedSampleRatesHz.end() ? *closest : kDefaultSampleRateHz;
    }
} // namespace

static AecSampleFormat GetAecSampleFormat(const UNCOMPRESSEDAUDIOFORMAT &format)
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

// Use AudioSampleConverter namespace for all conversion operations
using namespace AudioSampleConverter;

static void ExtractMonoSamples(const void *input,
                               AecSampleFormat format,
                               UINT32 frames,
                               UINT32 channels,
                               bool averageChannels,
                               bool inputSilent,
                               float *out)
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
        ExtractMonoSamplesTyped<float>(input, frames, channels, averageChannels,
                                       ConverterTraits<float>::ToFloat, out);
        break;

    case AecSampleFormat::kPcm16:
        ExtractMonoSamplesTyped<int16_t>(input, frames, channels, averageChannels,
                                         ConverterTraits<int16_t>::ToFloat, out);
        break;

    case AecSampleFormat::kPcm24Packed:
        ExtractMonoSamplesPcm24Packed(input, frames, channels, averageChannels, out);
        break;

    case AecSampleFormat::kPcm24In32:
        ExtractMonoSamplesTyped<int32_t>(input, frames, channels, averageChannels, [](int32_t v)
                                         { return ConverterTraits<int32_t>::ToFloat24(SignExtend24(v)); }, out);
        break;

    case AecSampleFormat::kPcm32:
        ExtractMonoSamplesTyped<int32_t>(input, frames, channels, averageChannels,
                                         ConverterTraits<int32_t>::ToFloat32, out);
        break;

    default:
        std::fill(out, out + frames, 0.0f);
        break;
    }
}

static void WriteMonoSamples(void *output,
                             AecSampleFormat format,
                             UINT32 frames,
                             UINT32 channels,
                             const float *mono)
{
    if (!output || !mono || frames == 0 || channels == 0)
    {
        return;
    }

    switch (format)
    {
    case AecSampleFormat::kFloat32:
        WriteMonoSamplesTyped<float>(output, frames, channels,
                                     ConverterTraits<float>::FromFloat, mono);
        break;

    case AecSampleFormat::kPcm16:
        WriteMonoSamplesTyped<int16_t>(output, frames, channels,
                                       ConverterTraits<int16_t>::FromFloat, mono);
        break;

    case AecSampleFormat::kPcm24Packed:
        WriteMonoSamplesPcm24Packed(output, frames, channels, mono);
        break;

    case AecSampleFormat::kPcm24In32:
        WriteMonoSamplesTyped<int32_t>(output, frames, channels,
                                       ConverterTraits<int32_t>::FromFloat24, mono);
        break;

    case AecSampleFormat::kPcm32:
        WriteMonoSamplesTyped<int32_t>(output, frames, channels,
                                       ConverterTraits<int32_t>::FromFloat32, mono);
        break;

    default:
        break;
    }
}

// Static declaration of the APO_REG_PROPERTIES structure
// associated with this APO.  The number in <> brackets is the
// number of IIDs supported by this APO.  If more than one, then additional
// IIDs are added at the end
#pragma warning(disable : 4815)
const AVRT_DATA CRegAPOProperties<1> CAecApoMFX::sm_RegProperties(
    __uuidof(AecApoMFX),                                                                 // clsid of this APO
    L"Aec3Apo",                                                                          // friendly name of this APO
    L"Copyright (c) msdx321",                                                            // copyright info
    1,                                                                                   // major version #
    1,                                                                                   // minor version #
    __uuidof(IAudioProcessingObject),                                                    // iid of primary interface
    (APO_FLAG)(APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH), // kak check this
    DEFAULT_APOREG_MININPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINPUTCONNECTIONS,
    DEFAULT_APOREG_MINOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXOUTPUTCONNECTIONS,
    DEFAULT_APOREG_MAXINSTANCES);

//-------------------------------------------------------------------------
// Helper Methods
//-------------------------------------------------------------------------

//
// ProcessSpeexFrame - Process one frame through Speex AEC (in-place)
//
void CAecApoMFX::ProcessSpeexFrame(std::vector<float> &captureFrameScratch, size_t frameSize)
{
    if (!m_speexState)
    {
        return;
    }

    std::vector<float> &renderFrameScratch = m_speexRenderFrameScratch;
    std::vector<int16_t> &speexMic16 = m_speexMic16;
    std::vector<int16_t> &speexRef16 = m_speexRef16;
    std::vector<int16_t> &speexOut16 = m_speexOut16;

    // Lock-free FIFO - no critical section needed
    size_t got = m_speexRenderFifo.Pop(renderFrameScratch.data(), frameSize);
    if (got < frameSize)
    {
        std::fill(renderFrameScratch.begin() + got,
                  renderFrameScratch.end(),
                  0.0f);
    }

    // Vectorized float->int16 conversion
    std::transform(captureFrameScratch.begin(), captureFrameScratch.begin() + frameSize,
                   speexMic16.begin(),
                   ConverterTraits<int16_t>::FromFloat);

    std::transform(renderFrameScratch.begin(), renderFrameScratch.begin() + frameSize,
                   speexRef16.begin(),
                   ConverterTraits<int16_t>::FromFloat);

    speex_echo_cancellation(m_speexState.get(),
                            speexMic16.data(),
                            speexRef16.data(),
                            speexOut16.data());

    // Vectorized int16->float conversion
    std::transform(speexOut16.begin(), speexOut16.begin() + frameSize,
                   captureFrameScratch.begin(),
                   ConverterTraits<int16_t>::ToFloat);
}

void CAecApoMFX::ProcessRnnoiseFrame(std::vector<float> &captureFrameScratch, size_t frameSize)
{
    if (!m_rnnoiseState || m_rnnoiseFrameSize <= 0)
    {
        return;
    }

    if (m_rnnoiseInputScratch.size() < static_cast<size_t>(m_rnnoiseFrameSize))
    {
        m_rnnoiseInputScratch.resize(m_rnnoiseFrameSize);
    }
    if (m_rnnoiseOutputScratch.size() < static_cast<size_t>(m_rnnoiseFrameSize))
    {
        m_rnnoiseOutputScratch.resize(m_rnnoiseFrameSize);
    }

    bool rnnoiseReady = false;
    if (m_rnnoiseResamplerIn && m_rnnoiseResamplerOut)
    {
        spx_uint32_t inLen = static_cast<spx_uint32_t>(frameSize);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(m_rnnoiseFrameSize);
        speex_resampler_process_float(m_rnnoiseResamplerIn.get(),
                                      0,
                                      captureFrameScratch.data(),
                                      &inLen,
                                      m_rnnoiseInputScratch.data(),
                                      &outLen);
        if (outLen < static_cast<spx_uint32_t>(m_rnnoiseFrameSize))
        {
            std::fill(m_rnnoiseInputScratch.begin() + outLen, m_rnnoiseInputScratch.end(), 0.0f);
        }
        rnnoiseReady = true;
    }
    else if (frameSize == static_cast<size_t>(m_rnnoiseFrameSize))
    {
        std::copy(captureFrameScratch.begin(),
                  captureFrameScratch.begin() + frameSize,
                  m_rnnoiseInputScratch.begin());
        rnnoiseReady = true;
    }

    if (rnnoiseReady)
    {
        // Vectorized PCM scale up
        std::transform(m_rnnoiseInputScratch.begin(),
                       m_rnnoiseInputScratch.begin() + m_rnnoiseFrameSize,
                       m_rnnoiseInputScratch.begin(),
                       [](float v)
                       { return v * kRnnoisePcmScale; });

        float vad = rnnoise_process_frame(m_rnnoiseState.get(),
                                          m_rnnoiseOutputScratch.data(),
                                          m_rnnoiseInputScratch.data());
        if (vad >= kRnnoiseVadThreshold)
        {
            m_rnnoiseVadGraceSamplesRemaining = (kRnnoiseSampleRateHz * kRnnoiseVadGraceMs) / 1000;
        }
        else if (m_rnnoiseVadGraceSamplesRemaining > 0)
        {
            m_rnnoiseVadGraceSamplesRemaining -= m_rnnoiseFrameSize;
            if (m_rnnoiseVadGraceSamplesRemaining < 0)
            {
                m_rnnoiseVadGraceSamplesRemaining = 0;
            }
        }
        else
        {
            std::fill(m_rnnoiseOutputScratch.begin(),
                      m_rnnoiseOutputScratch.begin() + m_rnnoiseFrameSize,
                      0.0f);
        }

        // Vectorized PCM scale down
        std::transform(m_rnnoiseOutputScratch.begin(),
                       m_rnnoiseOutputScratch.begin() + m_rnnoiseFrameSize,
                       m_rnnoiseOutputScratch.begin(),
                       [](float v)
                       { return v * kRnnoisePcmInvScale; });
    }

    if (rnnoiseReady && m_rnnoiseResamplerIn && m_rnnoiseResamplerOut)
    {
        spx_uint32_t inLen = static_cast<spx_uint32_t>(m_rnnoiseFrameSize);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(frameSize);
        speex_resampler_process_float(m_rnnoiseResamplerOut.get(),
                                      0,
                                      m_rnnoiseOutputScratch.data(),
                                      &inLen,
                                      captureFrameScratch.data(),
                                      &outLen);
        if (outLen < static_cast<spx_uint32_t>(frameSize))
        {
            std::fill(captureFrameScratch.begin() + outLen, captureFrameScratch.end(), 0.0f);
        }
    }
    else if (rnnoiseReady)
    {
        std::copy(m_rnnoiseOutputScratch.begin(),
                  m_rnnoiseOutputScratch.begin() + frameSize,
                  captureFrameScratch.begin());
    }
}

//
// ValidateAndSetupFormats - Validate and setup input/output formats
//
HRESULT CAecApoMFX::ValidateAndSetupFormats(
    APO_CONNECTION_DESCRIPTOR **ppInputConnections,
    APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    HRESULT hr = S_OK;
    UNCOMPRESSEDAUDIOFORMAT uncompAudioFormat;
    UNCOMPRESSEDAUDIOFORMAT uncompInputFormat;

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
    m_frameSize = static_cast<size_t>(m_sampleRateHz / kFrameDurationDivisor);

Exit:
    return hr;
}

//
// InitializeProcessingBuffers - Initialize FIFOs and pre-allocate scratch buffers
//
void CAecApoMFX::InitializeProcessingBuffers()
{
    m_captureFifo.Init(static_cast<size_t>(m_sampleRateHz));
    m_outputFifo.Init(static_cast<size_t>(m_sampleRateHz));
    m_speexRenderFifo.Init(static_cast<size_t>(m_sampleRateHz));

    // Pre-allocate scratch buffers to maximum expected size to avoid real-time allocations
    constexpr size_t kMaxScratchSize = 48000; // 1 second at 48kHz
    m_captureScratch.reserve(kMaxScratchSize);
    m_outputScratch.reserve(kMaxScratchSize);
    m_captureFrameScratch.reserve(m_frameSize);
}

//
// InitializeSpeexProcessors - Initialize Speex echo cancellation
//
void CAecApoMFX::InitializeSpeexProcessors()
{
    // Reset Speex states (RAII unique_ptr handles destruction)
    m_speexState.reset();

    m_speexFrameSize = static_cast<int>(m_frameSize);
    if (m_speexFrameSize > 0)
    {
        int filterLen = m_speexFrameSize * kFilterTailMultiplier;
        m_speexState.reset(speex_echo_state_init(m_speexFrameSize, filterLen));
        if (m_speexState)
        {
            speex_echo_ctl(m_speexState.get(), SPEEX_ECHO_SET_SAMPLING_RATE, &m_sampleRateHz);
            m_speexMic16.assign(m_frameSize, 0);
            m_speexRef16.assign(m_frameSize, 0);
            m_speexOut16.assign(m_frameSize, 0);
            m_speexRenderFrameScratch.assign(m_frameSize, 0.0f);
        }
    }
}

//
// InitializeRnnoiseProcessors - Initialize RNNoise denoising and resamplers
//
void CAecApoMFX::InitializeRnnoiseProcessors()
{
    m_rnnoiseState.reset();
    m_rnnoiseResamplerIn.reset();
    m_rnnoiseResamplerOut.reset();
    m_rnnoiseFrameSize = 0;
    m_rnnoiseVadGraceSamplesRemaining = 0;

    m_rnnoiseFrameSize = rnnoise_get_frame_size();
    if (m_rnnoiseFrameSize > 0)
    {
        m_rnnoiseState.reset(rnnoise_create(nullptr));
        m_rnnoiseInputScratch.assign(m_rnnoiseFrameSize, 0.0f);
        m_rnnoiseOutputScratch.assign(m_rnnoiseFrameSize, 0.0f);
        m_rnnoiseVadGraceSamplesRemaining = (kRnnoiseSampleRateHz * kRnnoiseVadGraceMs) / 1000;

        if (m_sampleRateHz != kRnnoiseSampleRateHz)
        {
            int err = 0;
            m_rnnoiseResamplerIn.reset(speex_resampler_init(
                1, m_sampleRateHz, kRnnoiseSampleRateHz, SPEEX_RESAMPLER_QUALITY_DEFAULT, &err));
            m_rnnoiseResamplerOut.reset(speex_resampler_init(
                1, kRnnoiseSampleRateHz, m_sampleRateHz, SPEEX_RESAMPLER_QUALITY_DEFAULT, &err));
        }
    }
}

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
STDMETHODIMP_(void)
CAecApoMFX::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY **ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY **ppOutputConnections)
{
    UNREFERENCED_PARAMETER(u32NumInputConnections);
    UNREFERENCED_PARAMETER(u32NumOutputConnections);

    const void *inputBuffer = nullptr;
    void *outputBuffer = nullptr;

    ATLASSERT(m_bIsLocked);

    // assert that the number of input and output connectins fits our registration properties
    ATLASSERT(m_pRegProperties->u32MinInputConnections <= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MaxInputConnections >= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MinOutputConnections <= u32NumOutputConnections);
    ATLASSERT(m_pRegProperties->u32MaxOutputConnections >= u32NumOutputConnections);

    ATLASSERT(ppInputConnections[0]->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);
    ATLASSERT(ppOutputConnections[0]->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);

    APO_CONNECTION_PROPERTY_V2 *inConnection = reinterpret_cast<APO_CONNECTION_PROPERTY_V2 *>(ppInputConnections[0]);
    APO_CONNECTION_PROPERTY_V2 *outConnection = reinterpret_cast<APO_CONNECTION_PROPERTY_V2 *>(ppOutputConnections[0]);
    UNREFERENCED_PARAMETER(inConnection);
    UNREFERENCED_PARAMETER(outConnection);

    // check APO_BUFFER_FLAGS.
    switch (ppInputConnections[0]->u32BufferFlags)
    {
    case BUFFER_INVALID:
    {
        ATLASSERT(false); // invalid flag - should never occur.  don't do anything.
        break;
    }
    case BUFFER_VALID:
    case BUFFER_SILENT:
    {
        inputBuffer = reinterpret_cast<const void *>(ppInputConnections[0]->pBuffer);
        outputBuffer = reinterpret_cast<void *>(ppOutputConnections[0]->pBuffer);

        UINT32 frames = ppInputConnections[0]->u32ValidFrameCount;
        bool inputSilent = (ppInputConnections[0]->u32BufferFlags == BUFFER_SILENT);

        // Ensure scratch buffers are large enough (should be pre-allocated in LockForProcess)
        ATLASSERT(m_captureScratch.capacity() >= frames);
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
            ATLASSERT(m_outputScratch.capacity() >= frames);
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

            ATLASSERT(m_captureFrameScratch.capacity() >= m_frameSize);
            if (m_captureFrameScratch.size() < m_frameSize)
            {
                m_captureFrameScratch.resize(m_frameSize);
            }

            // Process full 10 ms blocks through AEC then RNNoise
            while (m_captureFifo.Count() >= m_frameSize)
            {
                m_captureFifo.Pop(m_captureFrameScratch.data(), m_frameSize);
                ProcessSpeexFrame(m_captureFrameScratch, m_frameSize);
                ProcessRnnoiseFrame(m_captureFrameScratch, m_frameSize);
                m_outputFifo.Push(m_captureFrameScratch.data(), m_frameSize);
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
        ATLASSERT(false); // invalid flag - should never occur
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
STDMETHODIMP CAecApoMFX::GetLatency(HNSTIME *pTime)
{
    ASSERT_NONREALTIME();
    HRESULT hr = S_OK;

    IF_TRUE_ACTION_JUMP(pTime == nullptr, hr = E_POINTER, Exit);

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
                                        APO_CONNECTION_DESCRIPTOR **ppInputConnections,
                                        UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    ASSERT_NONREALTIME();
    HRESULT hr = S_OK;

    // Validate and setup input/output formats
    hr = ValidateAndSetupFormats(ppInputConnections, ppOutputConnections);
    IF_FAILED_JUMP(hr, Exit);

    // Initialize FIFOs and pre-allocate buffers
    InitializeProcessingBuffers();

    // Initialize processors
    InitializeSpeexProcessors();
    InitializeRnnoiseProcessors();

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
//  itself, it is valid to pass nullptr as the pbyData parameter and 0 as
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

HRESULT CAecApoMFX::Initialize(UINT32 cbDataSize, BYTE *pbyData)
{
    HRESULT hr = S_OK;

    IF_TRUE_ACTION_JUMP(((pbyData == nullptr) && (cbDataSize != 0)), hr = E_INVALIDARG, Exit);
    IF_TRUE_ACTION_JUMP(((pbyData != nullptr) && (cbDataSize == 0)), hr = E_INVALIDARG, Exit);

    if (cbDataSize == sizeof(APOInitSystemEffects3))
    {
        //
        // pbyData contains APOInitSystemEffects3 structure describing the microphone endpoint
        //
        APOInitSystemEffects3 *papoSysFxInit3 = reinterpret_cast<APOInitSystemEffects3 *>(pbyData);
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
        if (SUCCEEDED(papoSysFxInit3->pServiceProvider->QueryService(SID_AudioProcessingObjectLoggingService, IID_PPV_ARGS(&m_apoLoggingService))))
        {
            m_apoLoggingService->ApoLog(APO_LOG_LEVEL_INFO, L"CAecApoMFX::Initialize called with APOInitSystemEffects3.");
        }
    }
    else if (cbDataSize == sizeof(APOInitSystemEffects2))
    {
        //
        // pbyData contains APOInitSystemEffects2 structure describing the microphone endpoint
        //
        APOInitSystemEffects2 *papoSysFxInit2 = reinterpret_cast<APOInitSystemEffects2 *>(pbyData);
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

    *ppEffectsIds = nullptr;
    *pcEffects = 0;

    if (m_audioSignalProcessingMode == AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS)
    {
        // Return the list of effects implemented by this APO for COMMUNICATIONS processing mode
        static const GUID effectsList[] = {AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION};

        *ppEffectsIds = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(effectsList)));
        if (!*ppEffectsIds)
        {
            return E_OUTOFMEMORY;
        }
        *pcEffects = ARRAYSIZE(effectsList);
        CopyMemory(*ppEffectsIds, effectsList, sizeof(effectsList));
    }

    return S_OK;
}

STDMETHODIMP CAecApoMFX::GetControllableSystemEffectsList(_Outptr_result_buffer_maybenull_(*numEffects) AUDIO_SYSTEMEFFECT **effects, _Out_ UINT *numEffects, _In_opt_ HANDLE event)
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

        AUDIO_SYSTEMEFFECT *audioEffects = static_cast<AUDIO_SYSTEMEFFECT *>(
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

// Unified format validation function
static HRESULT IsFormatSupportedForAec(
    IAudioMediaType *pMediaType,
    BOOL *pSupported,
    bool requireMono,
    UINT32 maxChannels = kMaxInputChannels)
{
    ASSERT_NONREALTIME();

    HRESULT hr = S_OK;
    UNCOMPRESSEDAUDIOFORMAT format;
    AecSampleFormat sampleFormat = AecSampleFormat::kUnknown;
    bool formatValid = false;

    IF_TRUE_ACTION_JUMP((pMediaType == nullptr || pSupported == nullptr), hr = E_INVALIDARG, exit);
    hr = pMediaType->GetUncompressedAudioFormat(&format);
    IF_FAILED_JUMP(hr, exit);

    sampleFormat = GetAecSampleFormat(format);
    formatValid = sampleFormat != AecSampleFormat::kUnknown &&
                  IsSupportedAecSampleRate(format.fFramesPerSecond);

    if (requireMono)
    {
        // Output must be mono
        *pSupported = formatValid && format.dwSamplesPerFrame == 1;
    }
    else
    {
        // Input can be multi-channel
        *pSupported = formatValid &&
                      format.dwSamplesPerFrame > 0 &&
                      format.dwSamplesPerFrame <= maxChannels;
    }

exit:
    return hr;
}

HRESULT IsInputFormatSupportedForAec(IAudioMediaType *pMediaType, BOOL *pSupported)
{
    return IsFormatSupportedForAec(pMediaType, pSupported, false, kMaxInputChannels);
}

HRESULT IsOutputFormatSupportedForAec(IAudioMediaType *pMediaType, BOOL *pSupported)
{
    return IsFormatSupportedForAec(pMediaType, pSupported, true);
}

HRESULT
CreatePreferredInputMediaType(IAudioMediaType **ppMediaType,
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
CreatePreferredOutputMediaType(IAudioMediaType **ppMediaType,
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
    HRESULT hResult = S_OK;
    BOOL bSupportedOut = FALSE;
    BOOL bSupported = FALSE;

    IF_TRUE_ACTION_JUMP((pRequestedInputFormat == nullptr) || (ppSupportedInputFormat == nullptr), hResult = E_POINTER, Exit);
    *ppSupportedInputFormat = nullptr;

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
        bSupportedOut = FALSE;
        hResult = IsOutputFormatSupportedForAec(pOutputFormat, &bSupportedOut);
        IF_FAILED_JUMP(hResult, Exit);
        if (!bSupportedOut)
        {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    }

    bSupported = FALSE;
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
    HRESULT hResult = S_OK;
    BOOL bSupportedIn = FALSE;
    BOOL bSupported = FALSE;

    IF_TRUE_ACTION_JUMP((pRequestedOutputFormat == nullptr) || (ppSupportedOutputFormat == nullptr), hResult = E_POINTER, Exit);
    *ppSupportedOutputFormat = nullptr;

    if (pInputFormat != nullptr)
    {
        bSupportedIn = FALSE;
        hResult = IsInputFormatSupportedForAec(pInputFormat, &bSupportedIn);
        IF_FAILED_JUMP(hResult, Exit);
        if (!bSupportedIn)
        {
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
    }

    bSupported = FALSE;
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
    APO_CONNECTION_DESCRIPTOR *pInputConnection)
{
    HRESULT hResult = S_OK;
    UNCOMPRESSEDAUDIOFORMAT renderFormat = {};
    BOOL bSupported = FALSE;

    ASSERT_NONREALTIME();

    IF_TRUE_ACTION_JUMP(m_bIsLocked, hResult = APOERR_APO_LOCKED, Exit);
    IF_TRUE_ACTION_JUMP(!m_bIsInitialized, hResult = APOERR_NOT_INITIALIZED, Exit);

    bSupported = FALSE;
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

    IF_TRUE_ACTION_JUMP(((pbyData == nullptr) && (cbDataSize != 0)), hResult = E_INVALIDARG, Exit);
    IF_TRUE_ACTION_JUMP(((pbyData != nullptr) && (cbDataSize == 0)), hResult = E_INVALIDARG, Exit);
    if (cbDataSize == sizeof(APOInitSystemEffects3))
    {
        //
        // pbyData contains APOInitSystemEffects3 structure describing the loopback endpoint
        //
        APOInitSystemEffects3 *papoSysFxInit3 = reinterpret_cast<APOInitSystemEffects3 *>(pbyData);

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
CAecApoMFX::IsInputFormatSupported(IAudioMediaType *pRequestedInputFormat,
                                   IAudioMediaType **ppSupportedInputFormat)
{
    ASSERT_NONREALTIME();
    HRESULT hResult = S_OK;
    BOOL bSupported = FALSE;

    IF_TRUE_ACTION_JUMP((pRequestedInputFormat == nullptr) || (ppSupportedInputFormat == nullptr), hResult = E_POINTER, Exit);

    bSupported = FALSE;
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
                        const APO_CONNECTION_PROPERTY *pInputConnection)
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
    ExtractMonoSamples(reinterpret_cast<const void *>(pInputConnection->pBuffer),
                       m_renderSampleFormat,
                       frames,
                       renderChannels,
                       true,
                       inputSilent,
                       m_renderScratch.data());

    // Lock-free FIFO - no critical section needed
    if (m_speexState)
    {
        m_speexRenderFifo.Push(m_renderScratch.data(), frames);
    }
}

STDMETHODIMP CAecApoMFX::GetApoNotificationRegistrationInfo(_Out_writes_(*count) APO_NOTIFICATION_DESCRIPTOR **apoNotifications, _Out_ DWORD *count)
{
    *apoNotifications = nullptr;
    *count = 0;
    // Placeholder: no endpoint notifications yet.

    return S_OK;
}

static bool IsSameEndpointId(IMMDevice *device1, IMMDevice *device2)
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
STDMETHODIMP_(void)
CAecApoMFX::HandleNotification(_In_ APO_NOTIFICATION * /* apoNotification */)
{
    // Notification handling not yet implemented
}
