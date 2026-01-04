# Aec3Apo

I kicked off this project because I wanted hands-free gaming and chat without living in my earphones.

Aec3Apo is a Windows Audio Processing Object (APO) for capture endpoints (microphones). It applies acoustic echo cancellation and noise suppression using SpeexDSP. The APO outputs mono audio.

Prereqs
- Visual Studio 2022 + Windows Driver Kit (WDK)
- CMake 3.20+ (for third-party builds)

Submodules
- `git submodule update --init --recursive`

Build (x64)
- Open `AecApo.sln` (or `AecApo.vcxproj`) and build `Release|x64`.

Install (testing)
- Run PowerShell as Administrator.
- `installer\sign-install.ps1` copies the built DLL, signs the catalog, and installs the driver.
- If `-PfxPath` is omitted, `installer\sign-install.ps1` creates a self-signed test cert, exports it to `installer\aec3apo_test.pfx`, and trusts it for the current user.
- You can also install manually: `pnputil /add-driver installer\aec3apo_component.inf /install`.

Notes
- The APO registers as an Effect Pack targeting capture endpoints (microphones).
- SpeexDSP handles all supported rates (including 8/44.1 kHz).
- Output is mono; input supports up to 16 channels.