#pragma once

#include <complex>
#include <vector>

#ifndef FLOATING_POINT
#define FLOATING_POINT 1
#endif

#include "kiss_fft.h"
#include "kiss_fftr.h"

namespace nkf_aec
{
    class KissFft
    {
    public:
        explicit KissFft(int fft_size);
        ~KissFft();

        KissFft(const KissFft &) = delete;
        KissFft &operator=(const KissFft &) = delete;

        void Forward(const float *in, std::complex<float> *out);
        void Inverse(const std::complex<float> *in, float *out);

    private:
        int m_fftSize = 0;
        int m_numBins = 0;
        kiss_fftr_cfg m_forward = nullptr;
        kiss_fftr_cfg m_inverse = nullptr;
        std::vector<::kiss_fft_cpx> m_freqScratch;
    };
} // namespace nkf_aec
