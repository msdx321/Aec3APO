//
// SampleConverterSIMD.h -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Description:
//
//  AVX2-optimized audio sample format conversion utilities
//

#pragma once

#include <cstdint>
#include <cstddef>

namespace AudioSampleConverter
{
    namespace SIMD
    {
        //
        // Float to/from Int16 conversions
        // Used in ProcessSpeexFrame - called every 10ms frame
        //

        // Convert float array to int16 with AVX2 optimization
        // Processes 16 samples per iteration
        // input: Float array in range [-1.0, 1.0]
        // output: Int16 array in range [-32768, 32767]
        // count: Number of samples to convert
        void ConvertFloatToInt16_AVX2(const float *input, int16_t *output, size_t count);

        // Convert int16 array to float with AVX2 optimization
        // Processes 16 samples per iteration
        // input: Int16 array in range [-32768, 32767]
        // output: Float array in range [-1.0, 1.0]
        // count: Number of samples to convert
        void ConvertInt16ToFloat_AVX2(const int16_t *input, float *output, size_t count);

        //
        // PCM scaling operations
        // Used in ProcessRnnoiseFrame for PCM normalization
        //

        // Scale float array by constant factor with AVX2 optimization
        // Processes 8 samples per iteration
        // data: Float array (in-place modification)
        // count: Number of samples to scale
        // scale_factor: Multiplication factor (e.g., 32768.0f or 1/32768.0f)
        void ScaleFloatArray_AVX2(float *data, size_t count, float scale_factor);

        //
        // Channel operations
        //

        // Extract stereo to mono with averaging (AVX2 optimized)
        // Processes 4 stereo pairs (8 floats) per iteration
        // input: Interleaved stereo float array [L0 R0 L1 R1 ...]
        // output: Mono float array [(L0+R0)/2, (L1+R1)/2, ...]
        // frames: Number of output mono frames
        void ExtractStereoToMono_Float_AVX2(const float *input, float *output, size_t frames);

        // Write mono to stereo with replication (AVX2 optimized)
        // Processes 4 mono samples per iteration
        // input: Mono float array [M0 M1 M2 ...]
        // output: Interleaved stereo float array [M0 M0 M1 M1 M2 M2 ...]
        // frames: Number of input mono frames
        void WriteMonoToStereo_Float_AVX2(const float *input, float *output, size_t frames);

        //
        // Int32 conversions
        //

        // Convert float to int32 with AVX2 optimization (for PCM24/PCM32)
        // Processes 8 samples per iteration
        void ConvertFloatToInt32_AVX2(const float *input, int32_t *output, size_t count, float scale_factor);

        // Convert int32 to float with AVX2 optimization (for PCM24/PCM32)
        // Processes 8 samples per iteration
        void ConvertInt32ToFloat_AVX2(const int32_t *input, float *output, size_t count, float scale_factor);

    } // namespace SIMD
} // namespace AudioSampleConverter
