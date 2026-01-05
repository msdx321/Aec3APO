# AGENTS.md

Project: Aec3Apo (Windows Audio Processing Object for capture endpoints)

Quick context
- SpeexDSP and RNNoise are git submodules; build them before building the APO.
- Build target is x64; Visual Studio 2026 (v18) is standard, but VS 2022 (v17) is still required for the driver/WDK build path.
- Primary project file: `AecApo.vcxproj`.
- Installer script: `installer\\sign-install.ps1` (requires admin for install).
- RNNoise model sources live in `rnnoise_model\\` (built into rnnoise.lib).

Workflow
- Submodules: `git submodule update --init --recursive`
- Build SpeexDSP and RNNoise (x64):
  - SpeexDSP: `cmake -S third_party\\speexdsp -B third_party\\speexdsp\\build -G "Visual Studio 17 2022" -A x64 -D CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`
  - SpeexDSP: `cmake --build third_party\\speexdsp\\build --config Release`
  - RNNoise: `cmake -S cmake\\rnnoise -B build\\rnnoise -G "Visual Studio 17 2022" -A x64`
  - RNNoise: `cmake --build build\\rnnoise --config Release`
- Build APO (x64):
  - Open `AecApo.sln` (or `AecApo.vcxproj`) and build `Release|x64`.

Code locations
- APO implementation: `src\\`
- Public headers: `include\\`
- Installer assets: `installer\\`

Notes for agents
- Avoid touching third-party sources unless the task explicitly requires it.
- Prefer minimal, targeted changes; this is driver/COM-style code.
- If you need to modify build or installer scripts, keep PowerShell compatibility.
