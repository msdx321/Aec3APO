# AGENTS.md

Project: Aec3Apo (Windows Audio Processing Object for capture endpoints)

Quick context
- SpeexDSP is a git submodule; build it before building the APO.
- Build target is x64; Visual Studio 2022 + WDK required.
- Primary project file: `AecApo.vcxproj`.
- Installer script: `installer\\sign-install.ps1` (requires admin for install).

Workflow
- Submodules: `git submodule update --init --recursive`
- Build SpeexDSP (x64):
  - `cmake -S third_party\\speexdsp -B third_party\\speexdsp\\build -G "Visual Studio 17 2022" -A x64 -D CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`
  - `cmake --build third_party\\speexdsp\\build --config Release`
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
