# VS 2026 WDK NuGet Modernization Design

## Goal

Modernize Aec3Apo into a clean Visual Studio 2026 project that uses the current WDK path, keeps the APO build reproducible, and removes stale project/source inconsistencies without changing audio processing behavior.

## Current State

The repository builds the APO through `AecApo.sln` and `AecApo.vcxproj`. The solution still identifies as Visual Studio 17, while the project pre-build already invokes the Visual Studio 18 2026 CMake generator for SpeexDSP and RNNoise. The local machine has Visual Studio 18 and Windows Kit `10.0.28000.0` installed.

The active project compiles these source files:

- `src/AecApoDll.cpp`
- `src/AecApoMFX.cpp`
- `src/SampleConverterSIMD.cpp`

The repository also contains split implementation files such as `src/AecApoProcessing.cpp`, `src/AecApoAuxiliary.cpp`, `src/AecApoSystemEffects.cpp`, and `src/AecApoFormatUtils.cpp`. They are not compiled by the project and reference newer `m_runtime` state that is not declared by the active `src/AecApo.h`. These files are stale relative to the current build.

The baseline Release x64 build under Visual Studio 18 selects Windows SDK `10.0.28000.0`, but emits linker warning `LNK4098` because the APO and CMake-built static libraries do not use the same effective CRT. Investigation showed that the WDK `WindowsApplicationForDrivers10.0` targets use a hybrid CRT path: the project file may request `MultiThreadedDLL`, but WDK rewrites the compile task to `MultiThreaded` unless `OverrideDefaultRuntimeLibrary` is set. Forcing `OverrideDefaultRuntimeLibrary=true` removes `LNK4098`, but WDK API validation then rejects imports from `vcruntime140.dll` and `msvcp140.dll`. The clean WDK path is therefore to align the helper libraries with the APO's effective static CRT.

## External Toolchain Target

The target WDK package is `Microsoft.Windows.WDK.x64` version `10.0.28000.1839`. Microsoft lists the Windows 11 26H1 WDK build `28000.1839` with Visual Studio 2026 as the current default supported driver kit. The project should prefer this NuGet-backed WDK path while preserving a local installed WDK fallback when required by native WDK/MSBuild behavior.

The project remains x64-only for this pass.

## Architecture

Keep MSBuild as the authoritative build for the APO DLL and installer-facing WDK artifacts. This preserves the existing driver/APO project shape and avoids forcing WDK packaging through a pure CMake build.

Use CMake only for third-party static libraries:

- `cmake/speexdsp/CMakeLists.txt` builds `speexdsp.lib`.
- `cmake/rnnoise/CMakeLists.txt` builds `rnnoise.lib`.
- Both CMake helper projects must use the same effective CRT flavor as the APO under `WindowsApplicationForDrivers10.0`.

Add NuGet package metadata for the WDK path in the main native project where Visual Studio/MSBuild accepts it cleanly. If WDK NuGet package restore cannot be made reliable for this project type, document the installed WDK fallback and keep the project explicitly pinned to SDK build `10.0.28000.0`.

## Source Organization

The currently compiled source set remains authoritative. The stale split files are not wired into the build in this pass because doing so would require a broader runtime-state refactor and could alter real-time processing behavior.

The cleanup should delete the stale split files because their content is superseded by `src/AecApoMFX.cpp` for the active build. Preserving uncompiled alternates would keep the codebase ambiguous.

The active project file and filters must list only files that are intended to participate in the build or be visible project assets.

## Build Configuration

The solution should identify as Visual Studio 18.

The project should explicitly target Windows SDK `10.0.28000.0` and keep:

- `Debug|x64`
- `Release|x64`
- `WindowsApplicationForDrivers10.0`
- `ConfigurationType=DynamicLibrary`
- C++20
- warnings as errors for C++ compilation

The third-party CMake helper libraries should use the APO's effective WDK CRT settings:

- Release: `MultiThreaded`
- Debug: `MultiThreadedDebug`

The goal is a warning-free link for the APO under both Debug and Release.

## Documentation

Update repository documentation to describe Visual Studio 2026 and WDK 28000 as the primary build path.

Remove the stale claim that Visual Studio 2022 is required for the WDK build path unless verification proves a still-current reason to keep it. If a fallback is documented, it must be clearly marked as fallback, not the standard path.

Document the intended relationship between MSBuild and CMake:

- MSBuild builds and packages the APO.
- CMake builds third-party static dependencies.
- Third-party source submodules remain untouched unless a future task explicitly requires changes.

## Verification

The implementation is complete only after these checks pass or have documented tool-availability results:

- Build `Release|x64` with Visual Studio 18 MSBuild.
- Build `Debug|x64` with Visual Studio 18 MSBuild.
- Confirm the build selects Windows SDK `10.0.28000.0`.
- Confirm the APO link does not emit `LNK4098`.
- Run available WDK INF validation tools against `installer/*.inf`, or document that the tools are not available on the machine.

## Out of Scope

This pass must not change APO processing behavior, COM identities, INF identities, installer signing behavior, RNNoise model data, or third-party source code.

Completing the source split into smaller runtime modules is a separate refactor. It should start from the cleaned VS 2026/WDK baseline and include dedicated review of real-time processing state, auxiliary input state, and format utility boundaries.
