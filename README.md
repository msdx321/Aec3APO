# Aec3Apo

I kicked off this project because I wanted hands-free gaming and chat without living in my earphones.

Aec3Apo is a Windows Audio Processing Object (APO) for capture endpoints (microphones). It applies acoustic echo cancellation using SpeexDSP and noise suppression using RNNoise. The APO outputs mono audio.

Prereqs
- Visual Studio 2026 (v18) for most development.
- Visual Studio 2022 (v17) is still required for the driver/WDK build path.
- Windows Driver Kit (WDK)
- CMake 3.20+ (for third-party builds)

Submodules
- `git submodule update --init --recursive`

RNNoise model
- `rnnoise_model\\rnnoise_data.c` and `rnnoise_model\\rnnoise_data.h` must exist (built into rnnoise.lib).

Build (x64)
- Open `AecApo.sln` (or `AecApo.vcxproj`) and build `Release|x64`.

Install (testing)
- Run PowerShell as Administrator.
- `installer\sign-install.ps1` copies the built DLL, signs the catalog, and installs the driver.
- If `-PfxPath` is omitted, `installer\sign-install.ps1` creates a self-signed test cert, exports it to `installer\aec3apo_test.pfx`, and trusts it for the current user.
- You can also install manually: `pnputil /add-driver installer\aec3apo_component.inf /install`.

Notes
- The APO registers as an Effect Pack targeting capture endpoints (microphones).
- SpeexDSP handles AEC; RNNoise denoising runs at 48 kHz with resampling when needed.
- Output is mono; input supports up to 16 channels.
