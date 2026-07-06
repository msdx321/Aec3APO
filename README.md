# Aec3Apo

I kicked off this project because I wanted hands-free gaming and chat without living in my earphones.

Aec3Apo is a Windows Audio Processing Object (APO) for capture endpoints (microphones). It applies acoustic echo cancellation using SpeexDSP and noise suppression using RNNoise. The APO outputs mono audio.

Prereqs
- Visual Studio 2026 (v18) with Desktop development with C++.
- Windows Driver Kit 28000. The NuGet package target is `Microsoft.Windows.WDK.x64` `10.0.28000.1839`; an installed SDK/WDK `10.0.28000.0` is used as the local fallback.
- CMake 3.20+ (for SpeexDSP and RNNoise helper builds)

Submodules
- `git submodule update --init --recursive`

RNNoise model
- `rnnoise_model\\rnnoise_data.c` and `rnnoise_model\\rnnoise_data.h` must exist (built into rnnoise.lib).

Build (x64)
- Open `AecApo.sln` in Visual Studio 2026 and build `Release|x64`, or run:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m`
- MSBuild invokes CMake to build SpeexDSP and RNNoise into `build\speexdsp` and `build\rnnoise`.

Install (testing)
- Run PowerShell as Administrator.
- `installer\sign-install.ps1` copies the built DLL, signs the catalog, and installs the driver.
- If `-PfxPath` is omitted, `installer\sign-install.ps1` creates a self-signed test cert, exports it to `installer\aec3apo_test.pfx`, and trusts it for the current user.
- You can also install manually: `pnputil /add-driver installer\aec3apo_component.inf /install`.

Notes
- The APO registers as an Effect Pack targeting capture endpoints (microphones).
- SpeexDSP handles AEC; RNNoise denoising runs at 48 kHz with resampling when needed.
- Output is mono; input supports up to 16 channels.

AEC and RNNoise tuning
- The realtime pipeline is capture frame assembly, Speex echo cancellation, then RNNoise denoise. RNNoise VAD is used only for diagnostics and grace tracking; it does not gate or attenuate speech.
- Render loopback is assembled into timestamped 10 ms mono frames. Capture is assembled the same way, so AEC reference lookup is based on the QPC for the actual capture frame.
- Speex preprocess denoise is disabled by default. RNNoise owns noise suppression, which avoids stacked suppressors causing cutoff, pumping, or extra speech damage.
- AEC delay starts at 20 ms and updates from matched energetic render/capture frames. The estimate is clamped to 0 to 250 ms and smoothed before it is used for the next render reference lookup.
- Useful internal counters are `m_captureFramesProcessed`, `m_renderFramesPublished`, `m_aecFramesProcessed`, `m_aecFramesBypassedNoReference`, `m_aecFramesBypassedBadReference`, `m_rnnoiseFramesProcessed`, `m_lastReferenceDeltaQpc`, and `m_estimatedEchoDelayQpc`.
- If `m_aecFramesBypassedNoReference` grows, the APO is not receiving render loopback reference frames. If `m_aecFramesBypassedBadReference` grows, render frames exist but are outside the delay window or were being written concurrently. If `m_aecFramesProcessed` is low while render and capture counters are high, focus on timestamp alignment and loopback registration before changing denoise settings.
