//
// SampleConverterSIMD.cpp -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  AVX2-optimized implementations of audio sample format conversions
//  Requires AVX2 CPU - no scalar fallbacks
//

#include "SampleConverterSIMD.h"
#include <immintrin.h> // AVX2 intrinsics

namespace
{
    float ClampUnitSample(float v)
    {
        if (v > 1.0f)
            return 1.0f;
        if (v < -1.0f)
            return -1.0f;
        return v;
    }

    float ScaleAndClampSample(float v, float scale_factor, float min_value, float max_value)
    {
        float scaled = ClampUnitSample(v) * scale_factor;
        if (scaled > max_value)
            return max_value;
        if (scaled < min_value)
            return min_value;
        return scaled;
    }
}

namespace AudioSampleConverter
{
    namespace SIMD
    {
        //
        // Float to Int16 Conversion with AVX2 (AVX2 required)
        // Optimized for audio frame processing - assumes count is multiple of 16
        // Typical audio frames: 80 (8kHz), 160 (16kHz), 480 (48kHz), 960 samples - all divisible by 16
        //
        void ConvertFloatToInt16_AVX2(const float *input, int16_t *output, size_t count)
        {
            const __m256 scale = _mm256_set1_ps(32768.0f);
            const __m256 max_val = _mm256_set1_ps(32767.0f);
            const __m256 min_val = _mm256_set1_ps(-32768.0f);
            const __m256 one = _mm256_set1_ps(1.0f);
            const __m256 neg_one = _mm256_set1_ps(-1.0f);

            size_t i = 0;
            // Process in chunks of 16 samples
            for (; i + 15 < count; i += 16)
            {
                __m256 f0 = _mm256_loadu_ps(input + i);
                __m256 f1 = _mm256_loadu_ps(input + i + 8);

                // Clamp to [-1.0, 1.0]
                f0 = _mm256_min_ps(_mm256_max_ps(f0, neg_one), one);
                f1 = _mm256_min_ps(_mm256_max_ps(f1, neg_one), one);

                // Scale and clamp to int16 range
                f0 = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(f0, scale), min_val), max_val);
                f1 = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(f1, scale), min_val), max_val);

                // Convert to int32
                __m256i i0 = _mm256_cvtps_epi32(f0);
                __m256i i1 = _mm256_cvtps_epi32(f1);

                // Pack to int16
                __m128i packed0 = _mm_packs_epi32(_mm256_castsi256_si128(i0), _mm256_extracti128_si256(i0, 1));
                __m128i packed1 = _mm_packs_epi32(_mm256_castsi256_si128(i1), _mm256_extracti128_si256(i1, 1));

                _mm_storeu_si128(reinterpret_cast<__m128i *>(output + i), packed0);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(output + i + 8), packed1);
            }

            for (; i < count; ++i)
            {
                output[i] = static_cast<int16_t>(
                    ScaleAndClampSample(input[i], 32768.0f, -32768.0f, 32767.0f));
            }
        }

        //
        // Int16 to Float Conversion with AVX2 (AVX2 required)
        //
        void ConvertInt16ToFloat_AVX2(const int16_t *input, float *output, size_t count)
        {
            const __m256 inv_scale = _mm256_set1_ps(1.0f / 32768.0f);

            size_t i = 0;
            for (; i + 15 < count; i += 16)
            {
                __m128i i16_0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(input + i));
                __m128i i16_1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(input + i + 8));

                // Unpack to int32 with sign extension
                __m256i i32_0 = _mm256_set_m128i(_mm_cvtepi16_epi32(_mm_srli_si128(i16_0, 8)), _mm_cvtepi16_epi32(i16_0));
                __m256i i32_1 = _mm256_set_m128i(_mm_cvtepi16_epi32(_mm_srli_si128(i16_1, 8)), _mm_cvtepi16_epi32(i16_1));

                // Convert to float and scale
                __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_0), inv_scale);
                __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_1), inv_scale);

                _mm256_storeu_ps(output + i, f0);
                _mm256_storeu_ps(output + i + 8, f1);
            }

            for (; i < count; ++i)
            {
                output[i] = static_cast<float>(input[i]) * (1.0f / 32768.0f);
            }
        }

        //
        // Float Array Scaling with AVX2 (AVX2 required)
        //
        void ScaleFloatArray_AVX2(float *data, size_t count, float scale_factor)
        {
            const __m256 scale = _mm256_set1_ps(scale_factor);

            size_t i = 0;
            for (; i + 7 < count; i += 8)
            {
                __m256 v = _mm256_loadu_ps(data + i);
                v = _mm256_mul_ps(v, scale);
                _mm256_storeu_ps(data + i, v);
            }

            for (; i < count; ++i)
            {
                data[i] *= scale_factor;
            }
        }

        //
        // Extract Stereo to Mono with Averaging (AVX2 required)
        //
        void ExtractStereoToMono_Float_AVX2(const float *input, float *output, size_t frames)
        {
            const __m256 half = _mm256_set1_ps(0.5f);

            size_t i = 0;
            for (; i + 3 < frames; i += 4)
            {
                // Load 8 floats: [L0 R0 L1 R1 L2 R2 L3 R3]
                __m256 stereo = _mm256_loadu_ps(input + i * 2);

                // Shuffle and permute to separate L and R channels
                __m256 shuffled = _mm256_shuffle_ps(stereo, stereo, _MM_SHUFFLE(3, 1, 2, 0));
                __m256 temp = _mm256_permute2f128_ps(shuffled, shuffled, 0x01);
                __m256 left = _mm256_shuffle_ps(shuffled, temp, _MM_SHUFFLE(2, 0, 2, 0));
                __m256 right = _mm256_shuffle_ps(shuffled, temp, _MM_SHUFFLE(3, 1, 3, 1));

                // Average: (L + R) * 0.5
                __m256 avg = _mm256_mul_ps(_mm256_add_ps(left, right), half);

                // Extract and store 4 mono samples
                float temp_out[8];
                _mm256_storeu_ps(temp_out, avg);
                output[i] = temp_out[0];
                output[i + 1] = temp_out[1];
                output[i + 2] = temp_out[4];
                output[i + 3] = temp_out[5];
            }

            for (; i < frames; ++i)
            {
                output[i] = (input[i * 2] + input[i * 2 + 1]) * 0.5f;
            }
        }

        //
        // Write Mono to Stereo with Replication (AVX2 required)
        //
        void WriteMonoToStereo_Float_AVX2(const float *input, float *output, size_t frames)
        {
            size_t i = 0;
            for (; i + 3 < frames; i += 4)
            {
                __m128 mono = _mm_loadu_ps(input + i);
                __m128 stereo_lo = _mm_unpacklo_ps(mono, mono);
                __m128 stereo_hi = _mm_unpackhi_ps(mono, mono);

                _mm_storeu_ps(output + i * 2, stereo_lo);
                _mm_storeu_ps(output + i * 2 + 4, stereo_hi);
            }

            for (; i < frames; ++i)
            {
                output[i * 2] = input[i];
                output[i * 2 + 1] = input[i];
            }
        }

        //
        // Float to Int32 Conversion with AVX2 (for PCM24/PCM32)
        //
        void ConvertFloatToInt32_AVX2(const float *input, int32_t *output, size_t count, float scale_factor)
        {
            const __m256 scale = _mm256_set1_ps(scale_factor);
            const __m256 max_val = _mm256_set1_ps(scale_factor - 1.0f);
            const __m256 min_val = _mm256_set1_ps(-scale_factor);
            const __m256 one = _mm256_set1_ps(1.0f);
            const __m256 neg_one = _mm256_set1_ps(-1.0f);

            size_t i = 0;
            for (; i + 7 < count; i += 8)
            {
                __m256 f = _mm256_loadu_ps(input + i);

                // Clamp to [-1.0, 1.0]
                f = _mm256_min_ps(_mm256_max_ps(f, neg_one), one);

                // Scale and clamp
                f = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(f, scale), min_val), max_val);

                __m256i i32 = _mm256_cvtps_epi32(f);
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(output + i), i32);
            }

            for (; i < count; ++i)
            {
                output[i] = static_cast<int32_t>(
                    ScaleAndClampSample(input[i], scale_factor, -scale_factor, scale_factor - 1.0f));
            }
        }

        //
        // Int32 to Float Conversion with AVX2 (for PCM24/PCM32)
        //
        void ConvertInt32ToFloat_AVX2(const int32_t *input, float *output, size_t count, float scale_factor)
        {
            const __m256 inv_scale = _mm256_set1_ps(1.0f / scale_factor);

            size_t i = 0;
            for (; i + 7 < count; i += 8)
            {
                __m256i i32 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(input + i));
                __m256 f = _mm256_mul_ps(_mm256_cvtepi32_ps(i32), inv_scale);
                _mm256_storeu_ps(output + i, f);
            }

            for (; i < count; ++i)
            {
                output[i] = static_cast<float>(input[i]) / scale_factor;
            }
        }

    } // namespace SIMD
} // namespace AudioSampleConverter
