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
#include <type_traits>

namespace AudioSampleConverter
{
    // Forward declarations for SIMD functions
    namespace SIMD
    {
        void ExtractStereoToMono_Float_AVX2(const float *input, float *output, size_t frames);
        void WriteMonoToStereo_Float_AVX2(const float *input, float *output, size_t frames);
        void ConvertFloatToInt32_AVX2(const float *input, int32_t *output, size_t count, float scale_factor);
        void ConvertInt32ToFloat_AVX2(const int32_t *input, float *output, size_t count, float scale_factor);
    }

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

        // Batch conversion helpers for SIMD
        static void ToFloat32Batch(const int32_t *input, float *output, size_t count)
        {
            SIMD::ConvertInt32ToFloat_AVX2(input, output, count, kInt32ScaleFactor);
        }

        static void FromFloat32Batch(const float *input, int32_t *output, size_t count)
        {
            SIMD::ConvertFloatToInt32_AVX2(input, output, count, kInt32ScaleFactor);
        }

        static void ToFloat24Batch(const int32_t *input, float *output, size_t count)
        {
            SIMD::ConvertInt32ToFloat_AVX2(input, output, count, kInt24ScaleFactor);
        }

        static void FromFloat24Batch(const float *input, int32_t *output, size_t count)
        {
            SIMD::ConvertFloatToInt32_AVX2(input, output, count, kInt24ScaleFactor);
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
    // Specialized extraction for Int32 formats with SIMD
    //
    inline void ExtractMonoSamplesInt32_PCM32(
        const void *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        float *out)
    {
        const int32_t *in = static_cast<const int32_t *>(input);

        // AVX2 fast path for mono (channels == 1, no averaging needed)
        if (channels == 1)
        {
            SIMD::ConvertInt32ToFloat_AVX2(in, out, frames, kInt32ScaleFactor);
            return;
        }

        // Fall back to scalar for multi-channel
        if (averageChannels && channels > 1)
        {
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const int32_t *framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    sum += ConverterTraits<int32_t>::ToFloat32(framePtr[ch]);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            // Extract first channel without heap allocation in the realtime path.
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = ConverterTraits<int32_t>::ToFloat32(in[frame * channels]);
            }
        }
    }

    inline void ExtractMonoSamplesInt32_PCM24In32(
        const void *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        float *out)
    {
        const int32_t *in = static_cast<const int32_t *>(input);

        // Mono PCM24-in-32 needs sign extension; keep it allocation-free.
        if (channels == 1)
        {
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = ConverterTraits<int32_t>::ToFloat24(SignExtend24(in[frame]));
            }
            return;
        }

        // Fall back to scalar for multi-channel
        if (averageChannels && channels > 1)
        {
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const int32_t *framePtr = in + (frame * channels);
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    sum += ConverterTraits<int32_t>::ToFloat24(SignExtend24(framePtr[ch]));
                }
                out[frame] = sum / static_cast<float>(channels);
            }
        }
        else
        {
            // Extract first channel without heap allocation in the realtime path.
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = ConverterTraits<int32_t>::ToFloat24(SignExtend24(in[frame * channels]));
            }
        }
    }

    //
    // Specialized writing for Int32 formats with SIMD
    //
    inline void WriteMonoSamplesInt32_PCM32(
        void *output,
        uint32_t frames,
        uint32_t channels,
        const float *mono)
    {
        int32_t *out = static_cast<int32_t *>(output);

        // AVX2 fast path for mono (channels == 1)
        if (channels == 1)
        {
            SIMD::ConvertFloatToInt32_AVX2(mono, out, frames, kInt32ScaleFactor);
            return;
        }

        // Multi-channel output is uncommon here; keep it scalar to avoid heap allocation.
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            int32_t value = ConverterTraits<int32_t>::FromFloat32(mono[frame]);
            int32_t *framePtr = out + (frame * channels);
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                framePtr[ch] = value;
            }
        }
    }

    inline void WriteMonoSamplesInt32_PCM24In32(
        void *output,
        uint32_t frames,
        uint32_t channels,
        const float *mono)
    {
        int32_t *out = static_cast<int32_t *>(output);

        // AVX2 fast path for mono (channels == 1)
        if (channels == 1)
        {
            SIMD::ConvertFloatToInt32_AVX2(mono, out, frames, kInt24ScaleFactor);
            return;
        }

        // Multi-channel output is uncommon here; keep it scalar to avoid heap allocation.
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            int32_t value = ConverterTraits<int32_t>::FromFloat24(mono[frame]);
            int32_t *framePtr = out + (frame * channels);
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                framePtr[ch] = value;
            }
        }
    }

    //
    // Extract mono samples from multi-channel input
    // AVX2-optimized for float stereo, scalar fallback for int16 and edge cases
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

        // AVX2 fast path for float stereo averaging
        if constexpr (std::is_same_v<T, float>)
        {
            if (averageChannels && channels == 2)
            {
                SIMD::ExtractStereoToMono_Float_AVX2(
                    static_cast<const float *>(input),
                    out,
                    frames);
                return;
            }
        }

        // Scalar path for int16 and multi-channel (>2) edge cases
        if (averageChannels && channels > 1)
        {
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
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = toFloat(in[frame * channels]);
            }
        }
    }

    //
    // Write mono samples to multi-channel output
    // AVX2-optimized for float stereo, scalar fallback for int16 and edge cases
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

        // AVX2 fast path for float stereo replication
        if constexpr (std::is_same_v<T, float>)
        {
            if (channels == 2)
            {
                SIMD::WriteMonoToStereo_Float_AVX2(
                    in,
                    static_cast<float *>(output),
                    frames);
                return;
            }
        }

        // Scalar path for int16 and multi-channel (>2) edge cases
        if (channels == 1)
        {
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                out[frame] = fromFloat(in[frame]);
            }
        }
        else
        {
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
