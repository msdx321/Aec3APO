#include "nkf_aec/nkf_aec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

#include "dsp/kiss_fft_wrapper.h"
#include "math/activations.h"
#include "math/matrix_helper.h"
#include "nn/complex_gru.h"
#include "nn/kgnet.h"

namespace nkf_aec
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kFftSize = 1024;
        constexpr int kHopSize = 256;
        constexpr int kNumBins = 513;
        constexpr int kL = 4;
        constexpr int kInFeatLen = 2 * kL + 1;
        constexpr float kAmpScale = 0.5f;
        constexpr float kRefMagGate = 1e-5f;
    }

    struct NkfAec::Impl
    {
        Impl() : kgnet(kL, 18, 1, 18, kNumBins), fft_mic(kFftSize), fft_ref(kFftSize), ifft(kFftSize)
        {
            h_prior_re.resize(kL * kNumBins, 0.0f);
            h_prior_im.resize(kL * kNumBins, 0.0f);
            h_posterior_re.resize(kL * kNumBins, 0.0f);
            h_posterior_im.resize(kL * kNumBins, 0.0f);
            y_hat_re.resize(kNumBins, 0.0f);
            y_hat_im.resize(kNumBins, 0.0f);
            echo_hat_re.resize(kNumBins, 0.0f);
            echo_hat_im.resize(kNumBins, 0.0f);
            diff_re.resize(kNumBins, 0.0f);
            diff_im.resize(kNumBins, 0.0f);
            input_feature_re.resize(kInFeatLen * kNumBins, 0.0f);
            input_feature_im.resize(kInFeatLen * kNumBins, 0.0f);
            kg_e_re.resize(kNumBins * kL, 0.0f);
            kg_e_im.resize(kNumBins * kL, 0.0f);

            for (int i = 0; i < kFftSize; ++i)
            {
                window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / static_cast<float>(kFftSize))));
            }

            for (int i = 0; i < kL; ++i)
            {
                M_ptrs[i] = &M_store[i];
                R_ptrs[i] = &R_store[i];
                for (int j = 0; j < kNumBins; ++j)
                {
                    (*M_ptrs[i])[j] = std::complex<float>(0.0f, 0.0f);
                    (*R_ptrs[i])[j] = std::complex<float>(0.0f, 0.0f);
                }
            }
        }

        bool ProcessHop(const float *mic_in, const float *ref_in, float *out, unsigned frames)
        {
            if (!out || frames != kHopSize)
            {
                return false;
            }

            if (!mic_in)
            {
                std::fill(mic_hop.begin(), mic_hop.end(), 0.0f);
            }
            else
            {
                std::copy_n(mic_in, kHopSize, mic_hop.begin());
            }

            if (!ref_in)
            {
                std::fill(ref_hop.begin(), ref_hop.end(), 0.0f);
            }
            else
            {
                std::copy_n(ref_in, kHopSize, ref_hop.begin());
            }

            std::memmove(mic_buf.data(), mic_buf.data() + kHopSize, (kFftSize - kHopSize) * sizeof(float));
            std::memmove(ref_buf.data(), ref_buf.data() + kHopSize, (kFftSize - kHopSize) * sizeof(float));
            std::copy_n(mic_hop.data(), kHopSize, mic_buf.data() + (kFftSize - kHopSize));
            std::copy_n(ref_hop.data(), kHopSize, ref_buf.data() + (kFftSize - kHopSize));

            std::memmove(out_buf.data(), out_buf.data() + kHopSize, (kFftSize - kHopSize) * sizeof(float));
            std::fill(out_buf.begin() + (kFftSize - kHopSize), out_buf.end(), 0.0f);

            for (int i = 0; i < kFftSize; ++i)
            {
                ordered_mic[i] = window[i] * mic_buf[i];
                ordered_ref[i] = window[i] * ref_buf[i];
            }

            auto *tmpR = R_ptrs[0];
            for (int i = 1; i < kL; ++i)
            {
                R_ptrs[i - 1] = R_ptrs[i];
            }
            R_ptrs[kL - 1] = tmpR;
            fft_ref.Forward(ordered_ref.data(), R_ptrs[kL - 1]->data());

            float ref_mag_sum = 0.0f;
            for (int i = 0; i < kL; ++i)
            {
                for (int j = 0; j < kNumBins; ++j)
                {
                    const auto &val = (*R_ptrs[i])[j];
                    ref_mag_sum += std::sqrt(val.real() * val.real() + val.imag() * val.imag());
                }
            }
            ref_mag_sum /= static_cast<float>(kL * kNumBins);
            if (ref_mag_sum < kRefMagGate)
            {
                std::copy_n(mic_hop.data(), kHopSize, out);
                return true;
            }

            auto *tmpM = M_ptrs[0];
            for (int i = 1; i < kL; ++i)
            {
                M_ptrs[i - 1] = M_ptrs[i];
            }
            M_ptrs[kL - 1] = tmpM;
            fft_mic.Forward(ordered_mic.data(), M_ptrs[kL - 1]->data());

            for (int i = 0; i < kNumBins * kL; ++i)
            {
                dh_re[i] = h_posterior_re[i] - h_prior_re[i];
                dh_im[i] = h_posterior_im[i] - h_prior_im[i];
                h_prior_re[i] = h_posterior_re[i];
                h_prior_im[i] = h_posterior_im[i];
            }

            matmulFft(y_hat_re, y_hat_im, kNumBins, 1, kL, 1, R_ptrs, h_prior_re, h_prior_im);

            for (int i = 0; i < kNumBins; ++i)
            {
                diff_re[i] = (*M_ptrs[kL - 1])[i].real() - y_hat_re[i];
                diff_im[i] = (*M_ptrs[kL - 1])[i].imag() - y_hat_im[i];
            }

            for (int i = 0; i < kNumBins; ++i)
            {
                for (int j = 0; j < kL; ++j)
                {
                    input_feature_re[i * kInFeatLen + j] = (*R_ptrs[j])[i].real();
                    input_feature_im[i * kInFeatLen + j] = (*R_ptrs[j])[i].imag();
                }
                input_feature_re[i * kInFeatLen + kL] = diff_re[i];
                input_feature_im[i * kInFeatLen + kL] = diff_im[i];
                for (int j = kL + 1; j < kInFeatLen; ++j)
                {
                    input_feature_re[i * kInFeatLen + j] = dh_re[i * kL + (j - kL - 1)];
                    input_feature_im[i * kInFeatLen + j] = dh_im[i * kL + (j - kL - 1)];
                }
            }

            kgnet.forward(input_feature_re, input_feature_im);
            std::vector<float> &kg_re = kgnet.get_kg_re();
            std::vector<float> &kg_im = kgnet.get_kg_im();

            matmul(kg_e_re, kg_e_im, kNumBins, kL, 1, 1, kg_re, kg_im, diff_re, diff_im);

            for (int i = 0; i < kNumBins; ++i)
            {
                for (int j = 0; j < kL; ++j)
                {
                    const int pos = i * kL + j;
                    h_posterior_re[pos] = h_prior_re[pos] + kg_e_re[pos];
                    h_posterior_im[pos] = h_prior_im[pos] + kg_e_im[pos];
                }
            }

            matmulFft(echo_hat_re, echo_hat_im, kNumBins, 1, kL, 1, R_ptrs, h_posterior_re, h_posterior_im);
            for (int i = 0; i < kNumBins; ++i)
            {
                (*M_ptrs[kL - 1])[i].real((*M_ptrs[kL - 1])[i].real() - echo_hat_re[i]);
                (*M_ptrs[kL - 1])[i].imag((*M_ptrs[kL - 1])[i].imag() - echo_hat_im[i]);
            }

            (*M_ptrs[kL - 1])[0].imag((*M_ptrs[kL - 1])[kFftSize / 2].real());
            (*M_ptrs[kL - 1])[kFftSize / 2].real(0.0f);

            ifft.Inverse(M_ptrs[kL - 1]->data(), ifft_buf.data());

            for (int i = 0; i < kFftSize; ++i)
            {
                out_buf[i] += window[i] * ifft_buf[i] * kAmpScale / static_cast<float>(kFftSize);
            }

            std::copy_n(out_buf.data(), kHopSize, out);
            return true;
        }

        KGNet kgnet;
        KissFft fft_mic;
        KissFft fft_ref;
        KissFft ifft;

        std::array<float, kFftSize> window{};
        std::array<float, kFftSize> mic_buf{};
        std::array<float, kFftSize> ref_buf{};
        std::array<float, kFftSize> ordered_mic{};
        std::array<float, kFftSize> ordered_ref{};
        std::array<float, kFftSize> ifft_buf{};
        std::array<float, kFftSize> out_buf{};
        std::array<float, kHopSize> mic_hop{};
        std::array<float, kHopSize> ref_hop{};

        std::array<std::array<std::complex<float>, kNumBins>, kL> M_store{};
        std::array<std::array<std::complex<float>, kNumBins>, kL> R_store{};
        std::array<std::array<std::complex<float>, kNumBins>*, kL> M_ptrs{};
        std::array<std::array<std::complex<float>, kNumBins>*, kL> R_ptrs{};

        std::vector<float> h_prior_re;
        std::vector<float> h_prior_im;
        std::vector<float> h_posterior_re;
        std::vector<float> h_posterior_im;
        std::array<float, kNumBins * kL> dh_re{};
        std::array<float, kNumBins * kL> dh_im{};
        std::vector<float> y_hat_re;
        std::vector<float> y_hat_im;
        std::vector<float> echo_hat_re;
        std::vector<float> echo_hat_im;
        std::vector<float> diff_re;
        std::vector<float> diff_im;
        std::vector<float> input_feature_re;
        std::vector<float> input_feature_im;
        std::vector<float> kg_e_re;
        std::vector<float> kg_e_im;
    };

    NkfAec::NkfAec() : m_impl(std::make_unique<Impl>()) {}

    NkfAec::~NkfAec() = default;

    bool NkfAec::Initialize(const char * /*model_path*/)
    {
        return true;
    }

    bool NkfAec::ProcessBlock(const float *mic_in, const float *ref_in, float *out, unsigned frames)
    {
        return m_impl->ProcessHop(mic_in, ref_in, out, frames);
    }

    unsigned NkfAec::HopSize() const
    {
        return kHopSize;
    }
} // namespace nkf_aec
