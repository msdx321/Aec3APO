#pragma once

#include <memory>

namespace nkf_aec
{
    class NkfAec
    {
    public:
        NkfAec();
        ~NkfAec();

        bool Initialize(const char *model_path);
        bool ProcessBlock(const float *mic_in, const float *ref_in, float *out, unsigned frames);
        unsigned HopSize() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace nkf_aec
