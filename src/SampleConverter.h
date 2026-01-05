//
// SampleConverter.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Template-based audio sample format conversion utilities
//

#pragma once

#include <cstdint>
#include <algorithm>

namespace AudioSampleConverter
{

    // Sample format conversion scale factors
    constexpr float kInt16ScaleFactor = 32768.0f;
    constexpr float kInt16MaxValue = 32767.0f;
    constexpr float kInt16MinValue = -32768.0f;

    constexpr float kInt24ScaleFactor = 8388608.0f;
    constexpr float kInt24MaxValue = 8388607.0f;
    constexpr float kInt24MinValue = -8388608.0f;

    constexpr float kInt32ScaleFactor = 2147483648.0f;
    constexpr float kInt32MaxValue = 2147483647.0f;
    constexpr float kInt32MinValue = -2147483648.0f;

    // PCM24 bit manipulation constants
    constexpr int32_t kPcm24SignBit = 0x800000;
    constexpr int32_t kPcm24SignExtensionMask = ~0xFFFFFF;

    //
    // Converter trait - specialized for each sample type
    //
    template <typename T>
    struct ConverterTraits;

    // Float32 specialization
    template <>
    struct ConverterTraits<float>
    {
        static float ToFloat(float v) { return v; }
        static float FromFloat(float v) { return v; }
    };

    // int16 specialization
    template <>
    struct ConverterTraits<int16_t>
    {
        static float ToFloat(int16_t v)
        {
            return static_cast<float>(v) / kInt16ScaleFactor;
        }

        static int16_t FromFloat(float v)
        {
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            float scaled = v * kInt16ScaleFactor;
            if (scaled > kInt16MaxValue)
                scaled = kInt16MaxValue;
            if (scaled < kInt16MinValue)
                scaled = kInt16MinValue;
            return static_cast<int16_t>(scaled);
        }
    };

    // int32 specialization (for both PCM24 and PCM32)
    template <>
    struct ConverterTraits<int32_t>
    {
        // For PCM32
        static float ToFloat32(int32_t v)
        {
            return static_cast<float>(v) / kInt32ScaleFactor;
        }

        static int32_t FromFloat32(float v)
        {
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            float scaled = v * kInt32ScaleFactor;
            if (scaled > kInt32MaxValue)
                scaled = kInt32MaxValue;
            if (scaled < kInt32MinValue)
                scaled = kInt32MinValue;
            return static_cast<int32_t>(scaled);
        }

        // For PCM24
        static float ToFloat24(int32_t v)
        {
            return static_cast<float>(v) / kInt24ScaleFactor;
        }

        static int32_t FromFloat24(float v)
        {
            if (v > 1.0f)
                v = 1.0f;
            if (v < -1.0f)
                v = -1.0f;
            float scaled = v * kInt24ScaleFactor;
            if (scaled > kInt24MaxValue)
                scaled = kInt24MaxValue;
            if (scaled < kInt24MinValue)
                scaled = kInt24MinValue;
            return static_cast<int32_t>(scaled);
        }
    };

    //
    // PCM24 packed format helpers
    //
    inline int32_t ReadPcm24PackedSample(const uint8_t *data, size_t sampleIndex)
    {
        const uint8_t *src = data + (sampleIndex * 3);
        int32_t value = static_cast<int32_t>(src[0]) |
                        (static_cast<int32_t>(src[1]) << 8) |
                        (static_cast<int32_t>(src[2]) << 16);
        if (value & kPcm24SignBit)
        {
            value |= kPcm24SignExtensionMask;
        }
        return value;
    }

    inline void WritePcm24PackedSample(uint8_t *data, size_t sampleIndex, int32_t value)
    {
        uint8_t *dst = data + (sampleIndex * 3);
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    }

    inline int32_t SignExtend24(int32_t value)
    {
        return (value << 8) >> 8;
    }

    //
    // Extract mono samples from multi-channel input with template-based conversion
    //
    template <typename T, typename ConvertFunc>
    inline void ExtractMonoSamplesTyped(
        const void *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        ConvertFunc toFloat,
        float *out)
    {
        const T *in = static_cast<const T *>(input);

        if (averageChannels && channels > 1)
        {
            // Average all channels to mono
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const T *framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    sum += toFloat(framePtr[ch]);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            // Extract first channel only
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = toFloat(in[frame * channels]);
            }
        }
    }

    //
    // Write mono samples to multi-channel output with template-based conversion
    //
    template <typename T, typename ConvertFunc>
    inline void WriteMonoSamplesTyped(
        void *output,
        uint32_t frames,
        uint32_t channels,
        ConvertFunc fromFloat,
        const float *in)
    {
        T *out = static_cast<T *>(output);

        if (channels == 1)
        {
            // Mono output - direct conversion
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = fromFloat(in[frame]);
            }
        }
        else
        {
            // Multi-channel output - replicate mono to all channels
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                T value = fromFloat(in[frame]);
                T *framePtr = out + (frame * channels);
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    framePtr[ch] = value;
                }
            }
        }
    }

    //
    // Specialization for PCM24 packed format (extraction)
    //
    inline void ExtractMonoSamplesPcm24Packed(
        const void *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        float *out)
    {
        const uint8_t *in = static_cast<const uint8_t *>(input);

        if (averageChannels && channels > 1)
        {
            // Average all channels to mono
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    int32_t sample = ReadPcm24PackedSample(in, frame * channels + ch);
                    sum += ConverterTraits<int32_t>::ToFloat24(sample);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            // Extract first channel only
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                int32_t sample = ReadPcm24PackedSample(in, frame * channels);
                out[frame] = ConverterTraits<int32_t>::ToFloat24(sample);
            }
        }
    }

    //
    // Specialization for PCM24 packed format (writing)
    //
    inline void WriteMonoSamplesPcm24Packed(
        void *output,
        uint32_t frames,
        uint32_t channels,
        const float *in)
    {
        uint8_t *out = static_cast<uint8_t *>(output);

        if (channels == 1)
        {
            // Mono output - direct conversion
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                int32_t value = ConverterTraits<int32_t>::FromFloat24(in[frame]);
                WritePcm24PackedSample(out, frame, value);
            }
        }
        else
        {
            // Multi-channel output - replicate mono to all channels
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                int32_t value = ConverterTraits<int32_t>::FromFloat24(in[frame]);
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    WritePcm24PackedSample(out, frame * channels + ch, value);
                }
            }
        }
    }

} // namespace AudioSampleConverter
