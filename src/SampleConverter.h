//
// SampleConverter.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  Template-based audio sample format conversion utilities
//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    constexpr size_t kPcm24PackedBytes = 3;
    constexpr int32_t kPcm24SignBit = 0x800000;
    constexpr int32_t kPcm24SignExtensionMask = ~0xFFFFFF;

    inline float ClampUnitSample(float v)
    {
        return (std::clamp)(v, -1.0f, 1.0f);
    }

    inline float ScaleAndClampSample(float v, float scaleFactor, float minValue, float maxValue)
    {
        return (std::clamp)(ClampUnitSample(v) * scaleFactor, minValue, maxValue);
    }

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
            return static_cast<int16_t>(ScaleAndClampSample(v, kInt16ScaleFactor, kInt16MinValue, kInt16MaxValue));
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
            return static_cast<int32_t>(ScaleAndClampSample(v, kInt32ScaleFactor, kInt32MinValue, kInt32MaxValue));
        }

        // For PCM24
        static float ToFloat24(int32_t v)
        {
            return static_cast<float>(v) / kInt24ScaleFactor;
        }

        static int32_t FromFloat24(float v)
        {
            return static_cast<int32_t>(ScaleAndClampSample(v, kInt24ScaleFactor, kInt24MinValue, kInt24MaxValue));
        }

    };

    //
    // PCM24 packed format helpers
    //
    inline int32_t SignExtend24(int32_t value)
    {
        uint32_t sample = static_cast<uint32_t>(value) & 0x00FFFFFFu;
        if ((sample & static_cast<uint32_t>(kPcm24SignBit)) != 0)
        {
            sample |= static_cast<uint32_t>(kPcm24SignExtensionMask);
        }
        return static_cast<int32_t>(sample);
    }

    inline int32_t ReadPcm24PackedSample(const uint8_t *src)
    {
        const int32_t value = static_cast<int32_t>(src[0]) |
                              (static_cast<int32_t>(src[1]) << 8) |
                              (static_cast<int32_t>(src[2]) << 16);
        return SignExtend24(value);
    }

    inline void WritePcm24PackedSample(uint8_t *dst, int32_t value)
    {
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    }

    template <typename ConvertFunc>
    inline void ExtractMonoSamplesInt32Scalar(
        const int32_t *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        ConvertFunc toFloat,
        float *out)
    {
        if (averageChannels && channels > 1)
        {
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const int32_t *framePtr = input + (frame * channels);
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch)
                {
                    sum += toFloat(framePtr[ch]);
                }
                out[frame] = sum / static_cast<float>(channels);
            }
            return;
        }

        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            out[frame] = toFloat(input[frame * channels]);
        }
    }

    template <typename ConvertFunc>
    inline void WriteMonoSamplesInt32Scalar(
        int32_t *output,
        uint32_t frames,
        uint32_t channels,
        const float *mono,
        ConvertFunc fromFloat)
    {
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            const int32_t value = fromFloat(mono[frame]);
            int32_t *framePtr = output + (frame * channels);
            std::fill_n(framePtr, channels, value);
        }
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

        ExtractMonoSamplesInt32Scalar(
            in,
            frames,
            channels,
            averageChannels,
            ConverterTraits<int32_t>::ToFloat32,
            out);
    }

    inline void ExtractMonoSamplesInt32_PCM24In32(
        const void *input,
        uint32_t frames,
        uint32_t channels,
        bool averageChannels,
        float *out)
    {
        const int32_t *in = static_cast<const int32_t *>(input);

        ExtractMonoSamplesInt32Scalar(
            in,
            frames,
            channels,
            averageChannels,
            [](int32_t sample)
            {
                return ConverterTraits<int32_t>::ToFloat24(SignExtend24(sample));
            },
            out);
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

        WriteMonoSamplesInt32Scalar(out, frames, channels, mono, ConverterTraits<int32_t>::FromFloat32);
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

        WriteMonoSamplesInt32Scalar(out, frames, channels, mono, ConverterTraits<int32_t>::FromFloat24);
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
            if (channels == 1)
            {
                std::copy_n(in, frames, out);
                return;
            }

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
            if (channels == 1)
            {
                std::copy_n(in, frames, static_cast<float *>(output));
                return;
            }

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
                const T value = fromFloat(in[frame]);
                T *framePtr = out + (frame * channels);
                std::fill_n(framePtr, channels, value);
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
                const uint8_t *samplePtr =
                    in + (static_cast<size_t>(frame) * channels * kPcm24PackedBytes);
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < channels; ++ch, samplePtr += kPcm24PackedBytes)
                {
                    const int32_t sample = ReadPcm24PackedSample(samplePtr);
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
                const uint8_t *samplePtr =
                    in + (static_cast<size_t>(frame) * channels * kPcm24PackedBytes);
                const int32_t sample = ReadPcm24PackedSample(samplePtr);
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
                const int32_t value = ConverterTraits<int32_t>::FromFloat24(in[frame]);
                WritePcm24PackedSample(
                    out + (static_cast<size_t>(frame) * kPcm24PackedBytes),
                    value);
            }
        }
        else
        {
            // Multi-channel output - replicate mono to all channels
            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const int32_t value = ConverterTraits<int32_t>::FromFloat24(in[frame]);
                uint8_t *samplePtr =
                    out + (static_cast<size_t>(frame) * channels * kPcm24PackedBytes);
                for (uint32_t ch = 0; ch < channels; ++ch, samplePtr += kPcm24PackedBytes)
                {
                    WritePcm24PackedSample(samplePtr, value);
                }
            }
        }
    }

} // namespace AudioSampleConverter
