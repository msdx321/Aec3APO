//
// AecApoMFX.cpp -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Implementation of CAecApoMFX
//

#include <atlbase.h>
#include <atlcom.h>
#include <mmreg.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <resource.h>

#include <float.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "AecApo.h"
#include "SampleConverter.h"
#include "SampleConverterSIMD.h"

#include "speex/speex_echo.h"
#include "speex/speex_resampler.h"
#include "rnnoise.h"

static AecSampleFormat GetAecSampleFormat(const UNCOMPRESSEDAUDIOFORMAT &format);
static UINT32 GetBytesPerSample(AecSampleFormat format);
static UINT32 GetValidBitsPerSample(AecSampleFormat format);
static GUID GetFormatSubtype(AecSampleFormat format);

namespace
{
    constexpr int kDefaultSampleRateHz = 48000;
    constexpr int kMaxInputChannels = 16;
    constexpr float kSampleRateMatchToleranceHz = 1.0f;
    constexpr std::array<int, 4> kSupportedSampleRatesHz = {8000, 16000, 32000, 48000};

    // Speex configuration constants
    constexpr int kFilterTailMultiplier = 30;  // 300ms tail (30 * 10ms frames)
    constexpr int kFrameDurationDivisor = 100; // 10ms frames (sampleRate / 100)
    constexpr int kRnnoiseSampleRateHz = 48000;
    constexpr float kRnnoiseVadThreshold = 0.6f;
    constexpr int kRnnoiseVadGraceMs = 200;
    constexpr float kRnnoisePcmScale = 32768.0f;
    constexpr float kRnnoisePcmInvScale = 1.0f / 32768.0f;
    constexpr size_t kMaxRealtimeScratchSamples = 48000;
    constexpr size_t kRenderReferenceFrameSlots = 256; // 2.56 seconds at 10ms/frame
    constexpr int kInitialEchoDelayMs = 20;
    constexpr int kMinEchoDelayMs = 0;
    constexpr int kMaxEchoDelayMs = 250;
    constexpr int kRenderReferenceToleranceMs = 120;
    constexpr int kDelayUpdateSmoothingShift = 3;
    constexpr float kDelayEstimatorEnergyFloor = 1.0e-5f;
    const GUID kAecEffects[] = {AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION};

    static float SampleRateDeltaHz(float requestedRateHz, int supportedRateHz)
    {
        return std::fabs(requestedRateHz - static_cast<float>(supportedRateHz));
    }

    static int RnnoiseVadGraceSamples()
    {
        return (kRnnoiseSampleRateHz * kRnnoiseVadGraceMs) / 1000;
    }

    static bool IsSupportedAecSampleRate(float rate_hz)
    {
        return std::any_of(kSupportedSampleRatesHz.begin(), kSupportedSampleRatesHz.end(),
                           [rate_hz](int rate)
                           {
                               return SampleRateDeltaHz(rate_hz, rate) < kSampleRateMatchToleranceHz;
                           });
    }

    static int GetClosestSupportedSampleRate(float rate_hz)
    {
        auto closest = std::min_element(kSupportedSampleRatesHz.begin(), kSupportedSampleRatesHz.end(),
                                        [rate_hz](int a, int b)
                                        {
                                            const float diffA = SampleRateDeltaHz(rate_hz, a);
                                            const float diffB = SampleRateDeltaHz(rate_hz, b);
                                            return diffA < diffB;
                                        });

        return closest != kSupportedSampleRatesHz.end() ? *closest : kDefaultSampleRateHz;
    }

    static float ComputeMeanSquareEnergy(const float *samples, size_t sampleCount)
    {
        if (samples == nullptr || sampleCount == 0)
        {
            return 0.0f;
        }

        double sum = 0.0;
        for (size_t i = 0; i < sampleCount; ++i)
        {
            const double sample = samples[i];
            sum += sample * sample;
        }

        return static_cast<float>(sum / static_cast<double>(sampleCount));
    }

    static SpeexResamplerRAII::ResamplerStatePtr CreateSpeexFloatResampler(int inputRateHz, int outputRateHz)
    {
        int err = RESAMPLER_ERR_SUCCESS;
        SpeexResamplerRAII::ResamplerStatePtr resampler(
            speex_resampler_init(1, inputRateHz, outputRateHz, SPEEX_RESAMPLER_QUALITY_MAX, &err));
        if (err != RESAMPLER_ERR_SUCCESS || !resampler)
        {
            return {};
        }

        return resampler;
    }

    template <typename QpcTicksForSamples, typename FrameReady>
    static void QueueAssembledFrames(const float *samples,
                                     size_t sampleCount,
                                     UINT64 firstSampleQpc,
                                     size_t frameSize,
                                     std::vector<float> &assemblyScratch,
                                     size_t &assemblyCount,
                                     UINT64 &assemblyStartQpc,
                                     QpcTicksForSamples qpcTicksForSamples,
                                     FrameReady frameReady)
    {
        if (samples == nullptr || sampleCount == 0 || frameSize == 0 || assemblyScratch.size() < frameSize)
        {
            return;
        }

        UINT64 currentSampleQpc = firstSampleQpc;
        while (sampleCount > 0)
        {
            if (assemblyCount == 0)
            {
                assemblyStartQpc = currentSampleQpc;
            }

            const size_t remaining = frameSize - assemblyCount;
            const size_t chunk = (std::min)(sampleCount, remaining);
            std::copy_n(samples, chunk, assemblyScratch.data() + assemblyCount);

            assemblyCount += chunk;
            samples += chunk;
            sampleCount -= chunk;
            if (currentSampleQpc != 0)
            {
                currentSampleQpc += qpcTicksForSamples(chunk);
            }

            if (assemblyCount == frameSize)
            {
                frameReady(assemblyScratch.data(), assemblyStartQpc);
                assemblyCount = 0;
                assemblyStartQpc = 0;
            }
        }
    }

    struct RequestedFormatInfo
    {
        float sampleRate = static_cast<float>(kDefaultSampleRateHz);
        AecSampleFormat sampleFormat = AecSampleFormat::kUnknown;
        UINT32 channelCount = 1;
    };

    static UNCOMPRESSEDAUDIOFORMAT CreatePreferredUncompressedFormat(
        UINT32 channelCount,
        float requestedSampleRate,
        AecSampleFormat requestedFormat)
    {
        const AecSampleFormat formatType = (requestedFormat != AecSampleFormat::kUnknown)
                                               ? requestedFormat
                                               : AecSampleFormat::kFloat32;

        UNCOMPRESSEDAUDIOFORMAT format = {};
        format.guidFormatType = GetFormatSubtype(formatType);
        format.dwSamplesPerFrame = channelCount;
        format.dwBytesPerSampleContainer = GetBytesPerSample(formatType);
        format.dwValidBitsPerSample = GetValidBitsPerSample(formatType);
        format.fFramesPerSecond = static_cast<float>(GetClosestSupportedSampleRate(requestedSampleRate));
        format.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;
        return format;
    }

    static RequestedFormatInfo GetRequestedFormatInfo(IAudioMediaType *mediaType)
    {
        RequestedFormatInfo info = {};
        if (mediaType == nullptr)
        {
            return info;
        }

        if (const WAVEFORMATEX *audioFormat = mediaType->GetAudioFormat())
        {
            info.channelCount = audioFormat->nChannels;
        }

        UNCOMPRESSEDAUDIOFORMAT uncompressedFormat = {};
        if (SUCCEEDED(mediaType->GetUncompressedAudioFormat(&uncompressedFormat)))
        {
            info.sampleRate = uncompressedFormat.fFramesPerSecond;
            info.sampleFormat = GetAecSampleFormat(uncompressedFormat);
        }

        return info;
    }

    static HRESULT CopyEffectIdsToCoTaskMem(const GUID *effectIds,
                                            UINT effectCount,
                                            LPGUID *outputEffects,
                                            UINT *outputCount)
    {
        *outputEffects = nullptr;
        *outputCount = 0;

        const size_t byteCount = static_cast<size_t>(effectCount) * sizeof(GUID);
        LPGUID copiedEffects = static_cast<LPGUID>(CoTaskMemAlloc(byteCount));
        if (copiedEffects == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        std::copy_n(effectIds, effectCount, copiedEffects);
        *outputEffects = copiedEffects;
        *outputCount = effectCount;
        return S_OK;
    }

    static HRESULT CreateControllableEffects(const GUID *effectIds,
                                             UINT effectCount,
                                             AUDIO_SYSTEMEFFECT **effects,
                                             UINT *numEffects)
    {
        *effects = nullptr;
        *numEffects = 0;

        AUDIO_SYSTEMEFFECT *audioEffects = static_cast<AUDIO_SYSTEMEFFECT *>(
            CoTaskMemAlloc(static_cast<size_t>(effectCount) * sizeof(AUDIO_SYSTEMEFFECT)));
        if (audioEffects == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        for (UINT i = 0; i < effectCount; i++)
        {
            audioEffects[i].id = effectIds[i];
            audioEffects[i].state = AUDIO_SYSTEMEFFECT_STATE_ON;
            audioEffects[i].canSetState = FALSE;
        }

        *effects = audioEffects;
        *numEffects = effectCount;
        return S_OK;
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
        AudioSampleConverter::ExtractMonoSamplesTyped<float>(
            input,
            frames,
            channels,
            averageChannels,
            AudioSampleConverter::ConverterTraits<float>::ToFloat,
            out);
        break;

    case AecSampleFormat::kPcm16:
        AudioSampleConverter::ExtractMonoSamplesTyped<int16_t>(
            input,
            frames,
            channels,
            averageChannels,
            AudioSampleConverter::ConverterTraits<int16_t>::ToFloat,
            out);
        break;

    case AecSampleFormat::kPcm24Packed:
        AudioSampleConverter::ExtractMonoSamplesPcm24Packed(input, frames, channels, averageChannels, out);
        break;

    case AecSampleFormat::kPcm24In32:
        AudioSampleConverter::ExtractMonoSamplesInt32_PCM24In32(input, frames, channels, averageChannels, out);
        break;

    case AecSampleFormat::kPcm32:
        AudioSampleConverter::ExtractMonoSamplesInt32_PCM32(input, frames, channels, averageChannels, out);
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
        AudioSampleConverter::WriteMonoSamplesTyped<float>(
            output,
            frames,
            channels,
            AudioSampleConverter::ConverterTraits<float>::FromFloat,
            mono);
        break;

    case AecSampleFormat::kPcm16:
        AudioSampleConverter::WriteMonoSamplesTyped<int16_t>(
            output,
            frames,
            channels,
            AudioSampleConverter::ConverterTraits<int16_t>::FromFloat,
            mono);
        break;

    case AecSampleFormat::kPcm24Packed:
        AudioSampleConverter::WriteMonoSamplesPcm24Packed(output, frames, channels, mono);
        break;

    case AecSampleFormat::kPcm24In32:
        AudioSampleConverter::WriteMonoSamplesInt32_PCM24In32(output, frames, channels, mono);
        break;

    case AecSampleFormat::kPcm32:
        AudioSampleConverter::WriteMonoSamplesInt32_PCM32(output, frames, channels, mono);
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
    (APO_FLAG)(APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH),
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
UINT64 CAecApoMFX::SamplesToQpcTicks(size_t sampleCount, int sampleRateHz) const
{
    if (sampleCount == 0 || sampleRateHz <= 0 || m_qpcTicksPerSecond == 0)
    {
        return 0;
    }

    return ((static_cast<UINT64>(sampleCount) * m_qpcTicksPerSecond) +
            (static_cast<UINT64>(sampleRateHz) / 2)) /
           static_cast<UINT64>(sampleRateHz);
}

void CAecApoMFX::ResetRenderReferenceState()
{
    m_renderAssemblyCount = 0;
    m_renderAssemblyStartQpc = 0;
    m_renderReferenceWriteCounter.store(0, std::memory_order_release);

    if (m_renderReferenceSequence)
    {
        for (size_t i = 0; i < m_renderReferenceSlotCount; ++i)
        {
            m_renderReferenceSequence[i].store(0, std::memory_order_release);
        }
    }

    std::fill(m_renderReferenceQpc.begin(), m_renderReferenceQpc.end(), 0);
    std::fill(m_renderReferenceEnergy.begin(), m_renderReferenceEnergy.end(), 0.0f);
}

void CAecApoMFX::ProcessCaptureFrame(const float *frameData, UINT64 captureFrameQpc)
{
    if (frameData == nullptr || m_frameSize == 0 || m_captureFrameScratch.size() < m_frameSize)
    {
        return;
    }

    std::copy_n(frameData, m_frameSize, m_captureFrameScratch.data());
    ProcessSpeexFrame(m_captureFrameScratch, captureFrameQpc);
    ProcessRnnoiseFrame(m_captureFrameScratch);
    m_outputFifo.Push(m_captureFrameScratch.data(), m_frameSize);
}

void CAecApoMFX::QueueCaptureSamples(const float *samples, size_t sampleCount, UINT64 firstSampleQpc)
{
    QueueAssembledFrames(
        samples,
        sampleCount,
        firstSampleQpc,
        m_frameSize,
        m_captureAssemblyScratch,
        m_captureAssemblyCount,
        m_captureAssemblyStartQpc,
        [this](size_t chunk)
        {
            return SamplesToQpcTicks(chunk, m_sampleRateHz);
        },
        [this](const float *frameData, UINT64 frameStartQpc)
        {
            ProcessCaptureFrame(frameData, frameStartQpc);
        });
}

void CAecApoMFX::PublishRenderReferenceFrame(const float *frameData, UINT64 frameStartQpc)
{
    if (frameData == nullptr || m_frameSize == 0 || m_renderReferenceSlotCount == 0 || !m_renderReferenceSequence)
    {
        return;
    }

    const uint64_t frameId = m_renderReferenceWriteCounter.load(std::memory_order_relaxed);
    const size_t slot = static_cast<size_t>(frameId % m_renderReferenceSlotCount);
    uint32_t sequence = m_renderReferenceSequence[slot].load(std::memory_order_relaxed);
    if ((sequence & 1u) != 0)
    {
        ++sequence;
    }

    m_renderReferenceSequence[slot].store(sequence + 1, std::memory_order_release);
    std::copy_n(frameData, m_frameSize, m_renderReferenceRing.data() + (slot * m_frameSize));
    m_renderReferenceQpc[slot] = frameStartQpc;
    m_renderReferenceEnergy[slot] = ComputeMeanSquareEnergy(frameData, m_frameSize);
    m_renderReferenceSequence[slot].store(sequence + 2, std::memory_order_release);
    m_renderReferenceWriteCounter.store(frameId + 1, std::memory_order_release);
}

void CAecApoMFX::QueueRenderReferenceSamples(const float *samples, size_t sampleCount, UINT64 firstSampleQpc)
{
    QueueAssembledFrames(
        samples,
        sampleCount,
        firstSampleQpc,
        m_frameSize,
        m_renderAssemblyScratch,
        m_renderAssemblyCount,
        m_renderAssemblyStartQpc,
        [this](size_t chunk)
        {
            return SamplesToQpcTicks(chunk, m_sampleRateHz);
        },
        [this](const float *frameData, UINT64 frameStartQpc)
        {
            PublishRenderReferenceFrame(frameData, frameStartQpc);
        });
}

CAecApoMFX::ReferenceLookupStatus CAecApoMFX::TryGetRenderReferenceFrame(UINT64 captureQpc,
                                                                         float *outFrame,
                                                                         size_t frameSize,
                                                                         UINT64 *matchedReferenceQpc,
                                                                         float *matchedReferenceEnergy)
{
    if (outFrame == nullptr || frameSize != m_frameSize || m_renderReferenceSlotCount == 0 || !m_renderReferenceSequence)
    {
        return ReferenceLookupStatus::kNoReference;
    }

    const uint64_t published = m_renderReferenceWriteCounter.load(std::memory_order_acquire);
    if (published == 0)
    {
        return ReferenceLookupStatus::kNoReference;
    }

    UINT64 targetQpc = 0;
    UINT64 toleranceQpc = 0;
    if (captureQpc != 0 && m_qpcTicksPerSecond != 0)
    {
        const UINT64 delayQpc = m_estimatedEchoDelayQpc.load(std::memory_order_relaxed);
        toleranceQpc = (m_qpcTicksPerSecond * kRenderReferenceToleranceMs) / 1000;
        targetQpc = (captureQpc > delayQpc) ? (captureQpc - delayQpc) : captureQpc;
    }

    bool found = false;
    size_t bestSlot = 0;
    uint32_t bestSequence = 0;
    UINT64 bestDelta = (std::numeric_limits<UINT64>::max)();
    const uint64_t slotsToCheck = (std::min)(published, static_cast<uint64_t>(m_renderReferenceSlotCount));

    for (uint64_t i = 0; i < slotsToCheck; ++i)
    {
        const size_t slot = static_cast<size_t>((published - 1 - i) % m_renderReferenceSlotCount);
        const uint32_t sequence = m_renderReferenceSequence[slot].load(std::memory_order_acquire);
        if ((sequence & 1u) != 0)
        {
            continue;
        }

        const UINT64 frameQpc = m_renderReferenceQpc[slot];
        if (captureQpc == 0 || targetQpc == 0)
        {
            bestSlot = slot;
            bestSequence = sequence;
            found = true;
            break;
        }

        if (frameQpc == 0)
        {
            continue;
        }

        const UINT64 delta = (frameQpc > targetQpc) ? (frameQpc - targetQpc) : (targetQpc - frameQpc);
        if (delta < bestDelta)
        {
            bestDelta = delta;
            bestSlot = slot;
            bestSequence = sequence;
            found = true;
        }
    }

    if (!found || (targetQpc != 0 && bestDelta > toleranceQpc))
    {
        return ReferenceLookupStatus::kOutOfWindow;
    }

    std::copy_n(m_renderReferenceRing.data() + (bestSlot * frameSize), frameSize, outFrame);
    const uint32_t endSequence = m_renderReferenceSequence[bestSlot].load(std::memory_order_acquire);
    if (endSequence != bestSequence || ((endSequence & 1u) != 0))
    {
        return ReferenceLookupStatus::kConcurrentWrite;
    }

    if (matchedReferenceQpc != nullptr)
    {
        *matchedReferenceQpc = m_renderReferenceQpc[bestSlot];
    }
    if (matchedReferenceEnergy != nullptr)
    {
        *matchedReferenceEnergy = m_renderReferenceEnergy[bestSlot];
    }

    return ReferenceLookupStatus::kMatched;
}

void CAecApoMFX::ProcessSpeexFrame(std::vector<float> &captureFrameScratch, UINT64 captureQpc)
{
    if (!m_speexState)
    {
        return;
    }

    const size_t frameSize = m_frameSize;
    std::vector<float> &renderFrameScratch = m_speexRenderFrameScratch;
    std::vector<int16_t> &speexMic16 = m_speexMic16;
    std::vector<int16_t> &speexRef16 = m_speexRef16;
    std::vector<int16_t> &speexOut16 = m_speexOut16;

    UINT64 matchedReferenceQpc = 0;
    float matchedReferenceEnergy = 0.0f;
    const ReferenceLookupStatus lookupStatus =
        TryGetRenderReferenceFrame(captureQpc,
                                   renderFrameScratch.data(),
                                   frameSize,
                                   &matchedReferenceQpc,
                                   &matchedReferenceEnergy);
    if (lookupStatus != ReferenceLookupStatus::kMatched)
    {
        return;
    }

    if (captureQpc != 0 && matchedReferenceQpc != 0 && m_qpcTicksPerSecond != 0)
    {
        const int64_t referenceDelta = static_cast<int64_t>(captureQpc) - static_cast<int64_t>(matchedReferenceQpc);

        const float captureEnergy = ComputeMeanSquareEnergy(captureFrameScratch.data(), frameSize);
        if (referenceDelta >= 0 &&
            captureEnergy >= kDelayEstimatorEnergyFloor &&
            matchedReferenceEnergy >= kDelayEstimatorEnergyFloor)
        {
            const UINT64 minDelayQpc = (m_qpcTicksPerSecond * kMinEchoDelayMs) / 1000;
            const UINT64 maxDelayQpc = (m_qpcTicksPerSecond * kMaxEchoDelayMs) / 1000;
            const UINT64 observedDelayQpc = static_cast<UINT64>(referenceDelta);
            const UINT64 clampedDelayQpc = (std::min)((std::max)(observedDelayQpc, minDelayQpc), maxDelayQpc);
            const UINT64 estimatedDelayQpc = m_estimatedEchoDelayQpc.load(std::memory_order_relaxed);

            if (estimatedDelayQpc == 0)
            {
                m_estimatedEchoDelayQpc.store(clampedDelayQpc, std::memory_order_relaxed);
            }
            else
            {
                const int64_t delta =
                    static_cast<int64_t>(clampedDelayQpc) - static_cast<int64_t>(estimatedDelayQpc);
                const int64_t step = delta / (1 << kDelayUpdateSmoothingShift);
                const UINT64 smoothedDelayQpc =
                    static_cast<UINT64>(static_cast<int64_t>(estimatedDelayQpc) + step);
                m_estimatedEchoDelayQpc.store(smoothedDelayQpc, std::memory_order_relaxed);
            }
        }
    }

    // AVX2-optimized float->int16 conversion
    AudioSampleConverter::SIMD::ConvertFloatToInt16_AVX2(
        captureFrameScratch.data(),
        speexMic16.data(),
        frameSize);

    AudioSampleConverter::SIMD::ConvertFloatToInt16_AVX2(
        renderFrameScratch.data(),
        speexRef16.data(),
        frameSize);

    speex_echo_cancellation(m_speexState.get(),
                            speexMic16.data(),
                            speexRef16.data(),
                            speexOut16.data());

    // AVX2-optimized int16->float conversion
    AudioSampleConverter::SIMD::ConvertInt16ToFloat_AVX2(
        speexOut16.data(),
        captureFrameScratch.data(),
        frameSize);
}

bool CAecApoMFX::PrepareRnnoiseInput(std::vector<float> &captureFrameScratch)
{
    const size_t frameSize = m_frameSize;
    if (m_rnnoiseInputScratch.size() < static_cast<size_t>(m_rnnoiseFrameSize) ||
        m_rnnoiseOutputScratch.size() < static_cast<size_t>(m_rnnoiseFrameSize))
    {
        return false;
    }

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
        return true;
    }

    if (frameSize != static_cast<size_t>(m_rnnoiseFrameSize))
    {
        return false;
    }

    std::copy_n(captureFrameScratch.begin(), frameSize, m_rnnoiseInputScratch.begin());
    return true;
}

void CAecApoMFX::RunRnnoiseFrame()
{
    // AVX2-optimized PCM scale up
    AudioSampleConverter::SIMD::ScaleFloatArray_AVX2(
        m_rnnoiseInputScratch.data(),
        m_rnnoiseFrameSize,
        kRnnoisePcmScale);

    const float vad = rnnoise_process_frame(m_rnnoiseState.get(),
                                            m_rnnoiseOutputScratch.data(),
                                            m_rnnoiseInputScratch.data());
    if (vad >= kRnnoiseVadThreshold)
    {
        m_rnnoiseVadGraceSamplesRemaining = RnnoiseVadGraceSamples();
    }
    else if (m_rnnoiseVadGraceSamplesRemaining > 0)
    {
        m_rnnoiseVadGraceSamplesRemaining =
            (std::max)(0, m_rnnoiseVadGraceSamplesRemaining - m_rnnoiseFrameSize);
    }

    // AVX2-optimized PCM scale down
    AudioSampleConverter::SIMD::ScaleFloatArray_AVX2(
        m_rnnoiseOutputScratch.data(),
        m_rnnoiseFrameSize,
        kRnnoisePcmInvScale);
}

void CAecApoMFX::CopyRnnoiseOutputToCapture(std::vector<float> &captureFrameScratch)
{
    const size_t frameSize = m_frameSize;
    if (m_rnnoiseResamplerIn && m_rnnoiseResamplerOut)
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
    else
    {
        std::copy_n(m_rnnoiseOutputScratch.begin(), frameSize, captureFrameScratch.begin());
    }
}

void CAecApoMFX::ProcessRnnoiseFrame(std::vector<float> &captureFrameScratch)
{
    if (!m_rnnoiseState || m_rnnoiseFrameSize <= 0)
    {
        return;
    }

    if (!PrepareRnnoiseInput(captureFrameScratch))
    {
        return;
    }

    RunRnnoiseFrame();
    CopyRnnoiseOutputToCapture(captureFrameScratch);
}

//
// ValidateAndSetupFormats - Validate and setup input/output formats
//
HRESULT CAecApoMFX::ValidateAndSetupFormats(
    APO_CONNECTION_DESCRIPTOR **ppInputConnections,
    APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    HRESULT hr = S_OK;
    UNCOMPRESSEDAUDIOFORMAT uncompAudioFormat = {};
    UNCOMPRESSEDAUDIOFORMAT uncompInputFormat = {};

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
    m_outputFifo.Init(static_cast<size_t>(m_sampleRateHz));

    // Pre-allocate scratch buffers to maximum expected size to avoid real-time allocations
    m_captureScratch.assign(kMaxRealtimeScratchSamples, 0.0f);
    m_outputScratch.assign(kMaxRealtimeScratchSamples, 0.0f);
    m_renderScratch.assign(kMaxRealtimeScratchSamples, 0.0f);
    m_renderResampledScratch.assign(kMaxRealtimeScratchSamples, 0.0f);
    m_captureFrameScratch.assign(m_frameSize, 0.0f);
    m_speexRenderFrameScratch.assign(m_frameSize, 0.0f);
    m_renderAssemblyScratch.assign(m_frameSize, 0.0f);
    m_captureAssemblyScratch.assign(m_frameSize, 0.0f);
    m_captureAssemblyCount = 0;
    m_captureAssemblyStartQpc = 0;

    LARGE_INTEGER qpcFrequency = {};
    if (QueryPerformanceFrequency(&qpcFrequency))
    {
        m_qpcTicksPerSecond = static_cast<UINT64>(qpcFrequency.QuadPart);
    }
    else
    {
        m_qpcTicksPerSecond = 0;
    }

    m_renderReferenceSlotCount = (m_frameSize > 0) ? kRenderReferenceFrameSlots : 0;
    m_renderReferenceRing.assign(m_renderReferenceSlotCount * m_frameSize, 0.0f);
    m_renderReferenceQpc.assign(m_renderReferenceSlotCount, 0);
    m_renderReferenceEnergy.assign(m_renderReferenceSlotCount, 0.0f);
    if (m_renderReferenceSlotCount > 0)
    {
        m_renderReferenceSequence = std::make_unique<std::atomic<uint32_t>[]>(m_renderReferenceSlotCount);
    }
    else
    {
        m_renderReferenceSequence.reset();
    }
    ResetRenderReferenceState();
    m_estimatedEchoDelayQpc.store((m_qpcTicksPerSecond * kInitialEchoDelayMs) / 1000, std::memory_order_relaxed);
}

//
// InitializeSpeexProcessors - Initialize Speex echo cancellation
//
void CAecApoMFX::InitializeSpeexProcessors()
{
    // Reset Speex states (RAII unique_ptr handles destruction)
    m_speexState.reset();

    const int speexFrameSize = static_cast<int>(m_frameSize);
    if (speexFrameSize <= 0)
    {
        return;
    }

    const int filterLen = speexFrameSize * kFilterTailMultiplier;
    m_speexState.reset(speex_echo_state_init(speexFrameSize, filterLen));
    if (!m_speexState)
    {
        return;
    }

    speex_echo_ctl(m_speexState.get(), SPEEX_ECHO_SET_SAMPLING_RATE, &m_sampleRateHz);
    m_speexMic16.assign(m_frameSize, 0);
    m_speexRef16.assign(m_frameSize, 0);
    m_speexOut16.assign(m_frameSize, 0);
    m_speexRenderFrameScratch.assign(m_frameSize, 0.0f);
}

//
// InitializeRenderReferenceProcessors - Initialize render-to-capture resampling
//
void CAecApoMFX::InitializeRenderReferenceProcessors()
{
    m_renderResampler.reset();
    ResetRenderReferenceState();

    if (m_renderSampleRateHz <= 0 || m_sampleRateHz <= 0 || m_renderSampleRateHz == m_sampleRateHz)
    {
        return;
    }

    m_renderResampler = CreateSpeexFloatResampler(m_renderSampleRateHz, m_sampleRateHz);
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
    if (m_rnnoiseFrameSize <= 0)
    {
        return;
    }

    m_rnnoiseState.reset(rnnoise_create(nullptr));
    m_rnnoiseInputScratch.assign(m_rnnoiseFrameSize, 0.0f);
    m_rnnoiseOutputScratch.assign(m_rnnoiseFrameSize, 0.0f);
    m_rnnoiseVadGraceSamplesRemaining = RnnoiseVadGraceSamples();

    if (m_sampleRateHz == kRnnoiseSampleRateHz)
    {
        return;
    }

    m_rnnoiseResamplerIn = CreateSpeexFloatResampler(m_sampleRateHz, kRnnoiseSampleRateHz);
    m_rnnoiseResamplerOut = CreateSpeexFloatResampler(kRnnoiseSampleRateHz, m_sampleRateHz);
    if (!m_rnnoiseResamplerIn || !m_rnnoiseResamplerOut)
    {
        m_rnnoiseResamplerIn.reset();
        m_rnnoiseResamplerOut.reset();
    }
}

bool CAecApoMFX::HasRealtimeScratchCapacity(UINT32 frames) const
{
    const size_t frameCount = frames;
    return frameCount <= m_captureScratch.size() &&
           frameCount <= m_outputScratch.size();
}

void CAecApoMFX::WriteSilentOutput(APO_CONNECTION_PROPERTY *outputConnection, void *outputBuffer, UINT32 frames) const
{
    const UINT32 outputBytesPerSample = GetBytesPerSample(m_outputSampleFormat);
    if (outputBuffer != nullptr && outputBytesPerSample != 0)
    {
        ZeroMemory(outputBuffer, static_cast<SIZE_T>(frames) * m_u32SamplesPerFrame * outputBytesPerSample);
    }

    outputConnection->u32ValidFrameCount = frames;
    outputConnection->u32BufferFlags = BUFFER_SILENT;
}

void CAecApoMFX::WriteBypassOutput(void *outputBuffer, UINT32 frames) const
{
    WriteMonoSamples(outputBuffer,
                     m_outputSampleFormat,
                     frames,
                     m_u32SamplesPerFrame,
                     m_captureScratch.data());
}

APO_BUFFER_FLAGS CAecApoMFX::WriteProcessedOutput(void *outputBuffer,
                                                  UINT32 frames,
                                                  UINT64 inputQpc,
                                                  APO_BUFFER_FLAGS outputBufferFlags)
{
    const size_t frameCount = frames;
    QueueCaptureSamples(m_captureScratch.data(), frames, inputQpc);

    // Emit processed samples; if not enough yet, fall back to input.
    const size_t produced = m_outputFifo.Pop(m_outputScratch.data(), frameCount);
    if (produced > 0)
    {
        outputBufferFlags = BUFFER_VALID;
    }
    if (produced < frameCount)
    {
        std::copy_n(m_captureScratch.begin() + produced,
                    frameCount - produced,
                    m_outputScratch.begin() + produced);
    }

    WriteMonoSamples(outputBuffer,
                     m_outputSampleFormat,
                     frames,
                     m_u32SamplesPerFrame,
                     m_outputScratch.data());
    return outputBufferFlags;
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
//  routine that blocks, or touch pageable memory.
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

    ATLASSERT(m_bIsLocked);

    // assert that the number of input and output connections fits our registration properties
    ATLASSERT(m_pRegProperties->u32MinInputConnections <= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MaxInputConnections >= u32NumInputConnections);
    ATLASSERT(m_pRegProperties->u32MinOutputConnections <= u32NumOutputConnections);
    ATLASSERT(m_pRegProperties->u32MaxOutputConnections >= u32NumOutputConnections);

    const APO_CONNECTION_PROPERTY *inputConnection = ppInputConnections[0];
    APO_CONNECTION_PROPERTY *outputConnection = ppOutputConnections[0];

    ATLASSERT(inputConnection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);
    ATLASSERT(outputConnection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE);

    const APO_CONNECTION_PROPERTY_V2 *inputConnectionV2 =
        (inputConnection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE)
            ? reinterpret_cast<const APO_CONNECTION_PROPERTY_V2 *>(inputConnection)
            : nullptr;

    // check APO_BUFFER_FLAGS.
    switch (inputConnection->u32BufferFlags)
    {
    case BUFFER_INVALID:
    {
        ATLASSERT(false); // invalid flag - should never occur.  don't do anything.
        break;
    }
    case BUFFER_VALID:
    case BUFFER_SILENT:
    {
        const void *inputBuffer = reinterpret_cast<const void *>(inputConnection->pBuffer);
        void *outputBuffer = reinterpret_cast<void *>(outputConnection->pBuffer);

        const UINT32 frames = inputConnection->u32ValidFrameCount;
        const bool inputSilent = (inputConnection->u32BufferFlags == BUFFER_SILENT);
        const UINT64 inputQpc = (inputConnectionV2 != nullptr) ? inputConnectionV2->u64QPCTime : 0;
        APO_BUFFER_FLAGS outputBufferFlags = inputConnection->u32BufferFlags;

        if (!HasRealtimeScratchCapacity(frames))
        {
            WriteSilentOutput(outputConnection, outputBuffer, frames);
            break;
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
            WriteBypassOutput(outputBuffer, frames);
        }
        else
        {
            outputBufferFlags = WriteProcessedOutput(outputBuffer, frames, inputQpc, outputBufferFlags);
        }

        // Set the valid frame count.
        outputConnection->u32ValidFrameCount = frames;
        outputConnection->u32BufferFlags = outputBufferFlags;

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

    *pTime = (m_frameSize > 0 && m_sampleRateHz > 0)
                 ? static_cast<HNSTIME>((static_cast<UINT64>(m_frameSize) * 10000000ULL) /
                                        static_cast<UINT64>(m_sampleRateHz))
                 : 0;

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
    InitializeRenderReferenceProcessors();
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

        // Support for all processing modes; log when not COMMUNICATIONS.
        m_audioSignalProcessingMode = papoSysFxInit3->AudioProcessingMode;

        IF_TRUE_ACTION_JUMP(papoSysFxInit3->pDeviceCollection == nullptr, hr = E_INVALIDARG, Exit);
        // Get the endpoint on which this APO has been created. It is the last device in the device collection.
        UINT32 numDevices;
        hr = papoSysFxInit3->pDeviceCollection->GetCount(&numDevices);
        IF_FAILED_JUMP(hr, Exit);
        IF_TRUE_ACTION_JUMP(numDevices <= 0, hr = E_INVALIDARG, Exit);

        CComPtr<IMMDevice> captureDevice;
        hr = papoSysFxInit3->pDeviceCollection->Item(numDevices - 1, &captureDevice);
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

    if (ppEffectsIds == nullptr || pcEffects == nullptr)
    {
        return E_POINTER;
    }

    *ppEffectsIds = nullptr;
    *pcEffects = 0;

    if (m_audioSignalProcessingMode == AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS)
    {
        return CopyEffectIdsToCoTaskMem(kAecEffects, ARRAYSIZE(kAecEffects), ppEffectsIds, pcEffects);
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
        return CreateControllableEffects(kAecEffects, ARRAYSIZE(kAecEffects), effects, numEffects);
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
    UNCOMPRESSEDAUDIOFORMAT format = {};

    IF_TRUE_ACTION_JUMP((pMediaType == nullptr || pSupported == nullptr), hr = E_INVALIDARG, exit);
    hr = pMediaType->GetUncompressedAudioFormat(&format);
    IF_FAILED_JUMP(hr, exit);

    {
        const AecSampleFormat sampleFormat = GetAecSampleFormat(format);
        const bool formatValid = sampleFormat != AecSampleFormat::kUnknown &&
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

    UINT32 channelCount = 1;

    // Match the channel count of the input if it is less than 16
    if (requestedInputChannelCount <= kMaxInputChannels)
    {
        channelCount = requestedInputChannelCount;
    }

    UNCOMPRESSEDAUDIOFORMAT format =
        CreatePreferredUncompressedFormat(channelCount, requestedSampleRate, requestedFormat);
    return CreateAudioMediaTypeFromUncompressedAudioFormat(&format, ppMediaType);
}

HRESULT
CreatePreferredOutputMediaType(IAudioMediaType **ppMediaType,
                               float requestedSampleRate,
                               AecSampleFormat requestedFormat)
{
    ASSERT_NONREALTIME();

    UNCOMPRESSEDAUDIOFORMAT format =
        CreatePreferredUncompressedFormat(1, requestedSampleRate, requestedFormat);
    return CreateAudioMediaTypeFromUncompressedAudioFormat(&format, ppMediaType);
}

static HRESULT CreatePreferredInputMediaTypeForRequest(IAudioMediaType *requestedInputFormat,
                                                       IAudioMediaType **supportedInputFormat)
{
    const RequestedFormatInfo requestedInfo = GetRequestedFormatInfo(requestedInputFormat);
    return CreatePreferredInputMediaType(supportedInputFormat,
                                         requestedInfo.channelCount,
                                         requestedInfo.sampleRate,
                                         requestedInfo.sampleFormat);
}

static HRESULT CreatePreferredOutputMediaTypeForRequest(IAudioMediaType *requestedOutputFormat,
                                                        IAudioMediaType **supportedOutputFormat)
{
    const RequestedFormatInfo requestedInfo = GetRequestedFormatInfo(requestedOutputFormat);
    return CreatePreferredOutputMediaType(supportedOutputFormat,
                                          requestedInfo.sampleRate,
                                          requestedInfo.sampleFormat);
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
        hResult = CreatePreferredInputMediaTypeForRequest(pRequestedInputFormat, ppSupportedInputFormat);
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
        hResult = CreatePreferredOutputMediaTypeForRequest(pRequestedOutputFormat, ppSupportedOutputFormat);
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

        IF_TRUE_ACTION_JUMP(papoSysFxInit3->pDeviceCollection == nullptr, hResult = E_INVALIDARG, Exit);
        UINT32 numDevices;
        hResult = papoSysFxInit3->pDeviceCollection->GetCount(&numDevices);
        IF_FAILED_JUMP(hResult, Exit);
        IF_TRUE_ACTION_JUMP(numDevices <= 0, hResult = E_INVALIDARG, Exit);

        CComPtr<IMMDevice> loopbackDevice;
        hResult = papoSysFxInit3->pDeviceCollection->Item(numDevices - 1, &loopbackDevice);
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
    m_renderResampler.reset();
    ResetRenderReferenceState();

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
        hResult = CreatePreferredInputMediaTypeForRequest(pRequestedInputFormat, ppSupportedInputFormat);
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

    if (!m_speexState || m_frameSize == 0)
    {
        return;
    }

    const APO_CONNECTION_PROPERTY_V2 *connectionV2 =
        (pInputConnection->u32Signature == APO_CONNECTION_PROPERTY_V2_SIGNATURE)
            ? reinterpret_cast<const APO_CONNECTION_PROPERTY_V2 *>(pInputConnection)
            : nullptr;

    const UINT32 frames = pInputConnection->u32ValidFrameCount;
    if (frames == 0 || frames > m_renderScratch.size())
    {
        return;
    }

    const bool inputSilent = (pInputConnection->u32BufferFlags == BUFFER_SILENT ||
                              pInputConnection->pBuffer == 0);
    const UINT64 inputQpc = (connectionV2 != nullptr) ? connectionV2->u64QPCTime : 0;
    const UINT32 renderChannels = (m_renderSamplesPerFrame != 0) ? m_renderSamplesPerFrame : 1;
    ExtractMonoSamples(reinterpret_cast<const void *>(pInputConnection->pBuffer),
                       m_renderSampleFormat,
                       frames,
                       renderChannels,
                       true,
                       inputSilent,
                       m_renderScratch.data());

    if (!m_renderResampler)
    {
        QueueRenderReferenceSamples(m_renderScratch.data(), frames, inputQpc);
        return;
    }

    size_t inputOffset = 0;
    while (inputOffset < frames)
    {
        spx_uint32_t inLen = static_cast<spx_uint32_t>(frames - inputOffset);
        spx_uint32_t outLen = static_cast<spx_uint32_t>(m_renderResampledScratch.size());
        const UINT64 chunkQpc = (inputQpc != 0)
                                  ? inputQpc + SamplesToQpcTicks(inputOffset, m_renderSampleRateHz)
                                  : 0;
        const int err = speex_resampler_process_float(m_renderResampler.get(),
                                                      0,
                                                      m_renderScratch.data() + inputOffset,
                                                      &inLen,
                                                      m_renderResampledScratch.data(),
                                                      &outLen);
        if (err != RESAMPLER_ERR_SUCCESS)
        {
            break;
        }

        if (outLen > 0)
        {
            QueueRenderReferenceSamples(m_renderResampledScratch.data(), outLen, chunkQpc);
        }

        if (inLen == 0)
        {
            break;
        }
        inputOffset += inLen;
    }
}

STDMETHODIMP CAecApoMFX::GetApoNotificationRegistrationInfo(_Out_writes_(*count) APO_NOTIFICATION_DESCRIPTOR **apoNotifications, _Out_ DWORD *count)
{
    *apoNotifications = nullptr;
    *count = 0;
    // Placeholder: no endpoint notifications yet.

    return S_OK;
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
