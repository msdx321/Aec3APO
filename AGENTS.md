# AGENTS.md

Project: Aec3Apo (Windows Audio Processing Object for capture endpoints)

Quick context
- SpeexDSP and RNNoise are git submodules; MSBuild invokes CMake helper projects to build them before linking the APO.
- Build target is x64; Visual Studio 2026 (v18) with WDK 28000 is the standard build path.
- Primary project file: `AecApo.vcxproj`.
- Installer script: `installer\\sign-install.ps1` (requires admin for install).
- RNNoise model sources live in `rnnoise_model\\` (built into rnnoise.lib).

Workflow
- Submodules: `git submodule update --init --recursive`
- Build APO and third-party helper libraries (x64):
  - `& 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m`
- Build APO (x64):
  - Open `AecApo.sln` (or `AecApo.vcxproj`) in Visual Studio 2026 and build `Release|x64`.

Note: The project pins Windows SDK `10.0.28000.0` and uses the locally installed WDK when native WDK NuGet restore is unavailable.

Code locations
- APO implementation: `src\\`
- Public headers: `include\\`
- Installer assets: `installer\\`

Notes for agents
- Avoid touching third-party sources unless the task explicitly requires it.
- Prefer minimal, targeted changes; this is driver/COM-style code.
- If you need to modify build or installer scripts, keep PowerShell compatibility.
