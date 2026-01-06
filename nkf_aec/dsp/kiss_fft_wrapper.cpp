#include "kiss_fft_wrapper.h"

#include <stdexcept>
#include <cstdlib>

#include "kiss_fftr.h"

namespace nkf_aec
{
    KissFft::KissFft(int fft_size) : m_fftSize(fft_size), m_numBins(fft_size / 2 + 1)
    {
        if (fft_size <= 0)
        {
            throw std::runtime_error("Invalid FFT size");
        }

        m_forward = kiss_fftr_alloc(m_fftSize, 0, nullptr, nullptr);
        m_inverse = kiss_fftr_alloc(m_fftSize, 1, nullptr, nullptr);
        if (!m_forward || !m_inverse)
        {
            throw std::runtime_error("Failed to allocate kiss_fftr");
        }
        m_freqScratch.resize(static_cast<size_t>(m_numBins));
    }

    KissFft::~KissFft()
    {
        if (m_forward)
        {
            free(m_forward);
            m_forward = nullptr;
        }
        if (m_inverse)
        {
            free(m_inverse);
            m_inverse = nullptr;
        }
    }

    void KissFft::Forward(const float *in, std::complex<float> *out)
    {
        kiss_fftr(m_forward, in, m_freqScratch.data());
        for (int i = 0; i < m_numBins; ++i)
        {
            out[i] = std::complex<float>(m_freqScratch[i].r, m_freqScratch[i].i);
        }
    }

    void KissFft::Inverse(const std::complex<float> *in, float *out)
    {
        for (int i = 0; i < m_numBins; ++i)
        {
            m_freqScratch[i].r = in[i].real();
            m_freqScratch[i].i = in[i].imag();
        }
        kiss_fftri(m_inverse, m_freqScratch.data(), out);
    }
} // namespace nkf_aec
