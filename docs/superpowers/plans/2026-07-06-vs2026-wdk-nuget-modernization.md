# VS 2026 WDK NuGet Modernization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Modernize Aec3Apo into a clean Visual Studio 2026 project that uses the current WDK path and removes stale project/source inconsistencies without changing APO behavior.

**Architecture:** Keep MSBuild as the authoritative APO and WDK build entry point. Keep CMake only for SpeexDSP and RNNoise static libraries. Use WDK NuGet metadata where native MSBuild accepts it, while keeping the installed WDK `10.0.28000.0` path explicit and verifiable. Align third-party helper libraries with the APO's effective WDK CRT settings.

**Tech Stack:** Visual Studio 2026/MSBuild 18, Windows SDK/WDK 28000, `Microsoft.Windows.WDK.x64` `10.0.28000.1839`, CMake 3.20+, C++20, ATL/COM, SpeexDSP, RNNoise.

## Global Constraints

- Target WDK package: `Microsoft.Windows.WDK.x64` version `10.0.28000.1839`.
- Target installed SDK directory: `10.0.28000.0`.
- Supported configurations remain `Debug|x64` and `Release|x64`.
- Keep `WindowsApplicationForDrivers10.0`.
- Keep `ConfigurationType=DynamicLibrary`.
- Keep C++20.
- Keep warnings as errors for C++ compilation.
- Align SpeexDSP and RNNoise to the APO's effective `WindowsApplicationForDrivers10.0` CRT settings: Release `MultiThreaded`, Debug `MultiThreadedDebug`.
- Do not change APO processing behavior, COM identities, INF identities, installer signing behavior, RNNoise model data, or third-party source code.
- Do not add tests for this pass; verify with builds and WDK tools.
- The worktree has unrelated dirty files. Stage only files intentionally changed by this plan.

---

## File Structure

- Modify `AecApo.sln`: update Visual Studio metadata from version 17 to version 18.
- Modify `AecApo.vcxproj`: pin Windows SDK `10.0.28000.0`, add WDK NuGet package metadata, and keep x64 Debug/Release.
- Modify `AecApo.vcxproj.Filters`: remove duplicate `None` entries for compiled `.cpp` files and list the actual compiled SIMD source/header cleanly.
- Modify `cmake/speexdsp/CMakeLists.txt`: use the APO's effective WDK CRT setting for the static helper library.
- Modify `cmake/rnnoise/CMakeLists.txt`: use the APO's effective WDK CRT setting for the static helper library.
- Delete stale uncompiled source files:
  - `src/AecApoAuxiliary.cpp`
  - `src/AecApoFormatUtils.cpp`
  - `src/AecApoFormatUtils.h`
  - `src/AecApoMonoSampleIO.h`
  - `src/AecApoProcessing.cpp`
  - `src/AecApoSystemEffects.cpp`
- Modify `README.md`: document the VS 2026 and WDK 28000 build path.
- Modify `AGENTS.md`: document the current agent workflow and remove the stale VS 2022 WDK requirement.

### Task 1: Pin VS 2026, SDK 28000, and WDK NuGet Metadata

**Files:**
- Modify: `AecApo.sln`
- Modify: `AecApo.vcxproj`

**Interfaces:**
- Consumes: approved spec `docs/superpowers/specs/2026-07-06-vs2026-wdk-nuget-modernization-design.md`.
- Produces: MSBuild project metadata that advertises Visual Studio 18, targets SDK `10.0.28000.0`, and references WDK package `Microsoft.Windows.WDK.x64` `10.0.28000.1839`.

- [ ] **Step 1: Update solution metadata**

In `AecApo.sln`, change:

```text
# Visual Studio Version 18
VisualStudioVersion = 18.0.36248.0
MinimumVisualStudioVersion = 10.0.40219.1
```

Replace the existing `# Visual Studio Version 17` and `VisualStudioVersion = 17.0.31903.59` lines only.

- [ ] **Step 2: Pin the Windows SDK in the project globals**

In `AecApo.vcxproj`, inside `<PropertyGroup Label="Globals">`, add these properties after `<RootNamespace>$(MSBuildProjectName)</RootNamespace>`:

```xml
    <VCProjectVersion>18.0</VCProjectVersion>
    <WindowsTargetPlatformVersion>10.0.28000.0</WindowsTargetPlatformVersion>
```

- [ ] **Step 3: Add WDK NuGet package metadata**

In `AecApo.vcxproj`, add this item group before the existing `WrappedTaskItems` item group:

```xml
  <ItemGroup>
    <PackageReference Include="Microsoft.Windows.WDK.x64" Version="10.0.28000.1839" />
  </ItemGroup>
```

If `MSBuild.exe /t:Restore AecApo.vcxproj /p:Configuration=Release /p:Platform=x64` rejects `PackageReference` for this native project, remove this item group and document the installed WDK fallback in `README.md` and `AGENTS.md`. Keep `WindowsTargetPlatformVersion` pinned either way.

- [ ] **Step 4: Verify restore behavior**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.vcxproj /t:Restore /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Expected if WDK PackageReference works: exit code `0`.

Expected if unsupported by this native project shape: a restore/package error. In that case, remove the `<PackageReference>` item group added in Step 3 and continue with the installed WDK fallback documented in Task 4.

- [ ] **Step 5: Commit project metadata**

If Step 4 succeeds with PackageReference:

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- AecApo.sln AecApo.vcxproj
& 'C:\Program Files\Git\cmd\git.exe' commit -m "build: target VS 2026 WDK 28000"
```

If Step 4 requires fallback:

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- AecApo.sln AecApo.vcxproj
& 'C:\Program Files\Git\cmd\git.exe' commit -m "build: target VS 2026 SDK 28000"
```

### Task 2: Align Third-Party Helper Libraries with WDK CRT

**Files:**
- Modify: `cmake/speexdsp/CMakeLists.txt`
- Modify: `cmake/rnnoise/CMakeLists.txt`

**Interfaces:**
- Consumes: `AecApo.vcxproj` under WDK `WindowsApplicationForDrivers10.0`, which rewrites the effective compile runtime to `MultiThreaded`/`MultiThreadedDebug` unless `OverrideDefaultRuntimeLibrary` is set.
- Produces: CMake helper projects that generate static libraries with the same effective WDK CRT flavor as the APO.

- [ ] **Step 1: Update SpeexDSP runtime library**

In `cmake/speexdsp/CMakeLists.txt`, set the MSVC runtime property to:

```cmake
if (MSVC)
    set_property(TARGET speexdsp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()
```

This replaces any existing `MSVC_RUNTIME_LIBRARY` setting for `speexdsp`.

- [ ] **Step 2: Update RNNoise runtime library**

In `cmake/rnnoise/CMakeLists.txt`, set the MSVC runtime property to:

```cmake
if (MSVC)
    set_property(TARGET rnnoise PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()
```

This replaces any existing `MSVC_RUNTIME_LIBRARY` setting for `rnnoise`.

- [ ] **Step 3: Remove generated helper build directories**

Run:

```powershell
$paths = @(
  (Resolve-Path -LiteralPath 'build\speexdsp' -ErrorAction SilentlyContinue),
  (Resolve-Path -LiteralPath 'build\rnnoise' -ErrorAction SilentlyContinue)
)
foreach ($path in $paths) {
  if ($path -and $path.Path.StartsWith((Resolve-Path -LiteralPath '.').Path)) {
    Remove-Item -LiteralPath $path.Path -Recurse -Force
  }
}
```

Expected: no output. The next MSBuild run regenerates both helper projects.

- [ ] **Step 4: Commit CRT alignment**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- cmake/speexdsp/CMakeLists.txt cmake/rnnoise/CMakeLists.txt
& 'C:\Program Files\Git\cmd\git.exe' commit -m "build: align third-party CRT settings"
```

### Task 3: Remove Stale Uncompiled Split Files and Clean Filters

**Files:**
- Delete: `src/AecApoAuxiliary.cpp`
- Delete: `src/AecApoFormatUtils.cpp`
- Delete: `src/AecApoFormatUtils.h`
- Delete: `src/AecApoMonoSampleIO.h`
- Delete: `src/AecApoProcessing.cpp`
- Delete: `src/AecApoSystemEffects.cpp`
- Modify: `AecApo.vcxproj.Filters`

**Interfaces:**
- Consumes: active source authority in `src/AecApoMFX.cpp`.
- Produces: a project tree without stale uncompiled alternates that reference undeclared `m_runtime` state.

- [ ] **Step 1: Confirm stale files are not compiled**

Run:

```powershell
Select-String -Path AecApo.vcxproj -Pattern 'AecApoAuxiliary|AecApoFormatUtils|AecApoMonoSampleIO|AecApoProcessing|AecApoSystemEffects'
```

Expected: no output.

- [ ] **Step 2: Delete stale files**

Run:

```powershell
$staleFiles = @(
  'src\AecApoAuxiliary.cpp',
  'src\AecApoFormatUtils.cpp',
  'src\AecApoFormatUtils.h',
  'src\AecApoMonoSampleIO.h',
  'src\AecApoProcessing.cpp',
  'src\AecApoSystemEffects.cpp'
)
foreach ($file in $staleFiles) {
  $resolved = Resolve-Path -LiteralPath $file -ErrorAction SilentlyContinue
  if ($resolved -and $resolved.Path.StartsWith((Resolve-Path -LiteralPath '.').Path)) {
    Remove-Item -LiteralPath $resolved.Path -Force
  }
}
```

Expected: no output.

- [ ] **Step 3: Clean Visual Studio filters**

In `AecApo.vcxproj.Filters`, remove these duplicate `None` entries:

```xml
    <None Include="src\AecApoDll.cpp">
      <Filter>Source Files</Filter>
    </None>
    <None Include="src\AecApoMfx.cpp">
      <Filter>Source Files</Filter>
    </None>
```

Add filters for the currently compiled SIMD source and headers:

```xml
    <ClCompile Include="src\SampleConverterSIMD.cpp">
      <Filter>Source Files</Filter>
    </ClCompile>
```

```xml
    <ClInclude Include="src\SampleConverter.h">
      <Filter>Header Files</Filter>
    </ClInclude>
    <ClInclude Include="src\SampleConverterSIMD.h">
      <Filter>Header Files</Filter>
    </ClInclude>
```

- [ ] **Step 4: Confirm stale references are gone**

Run:

```powershell
Select-String -Path AecApo.vcxproj,AecApo.vcxproj.Filters,README.md,AGENTS.md -Pattern 'AecApoAuxiliary|AecApoFormatUtils|AecApoMonoSampleIO|AecApoProcessing|AecApoSystemEffects'
```

Expected: no output.

- [ ] **Step 5: Commit source cleanup**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- AecApo.vcxproj.Filters src/AecApoAuxiliary.cpp src/AecApoFormatUtils.cpp src/AecApoFormatUtils.h src/AecApoMonoSampleIO.h src/AecApoProcessing.cpp src/AecApoSystemEffects.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "refactor: remove stale uncompiled APO sources"
```

### Task 4: Update Build Documentation

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: final project shape from Tasks 1-3.
- Produces: documentation that describes VS 2026, WDK 28000, MSBuild as APO entry point, and CMake as third-party helper build only.

- [ ] **Step 1: Update README prerequisites**

In `README.md`, replace the current prereq block with:

```markdown
Prereqs
- Visual Studio 2026 (v18) with Desktop development with C++.
- Windows Driver Kit 28000. The NuGet package target is `Microsoft.Windows.WDK.x64` `10.0.28000.1839`; an installed SDK/WDK `10.0.28000.0` is used as the local fallback.
- CMake 3.20+ (for SpeexDSP and RNNoise helper builds).
```

- [ ] **Step 2: Update README build section**

In `README.md`, replace the `Build (x64)` section with:

```markdown
Build (x64)
- Open `AecApo.sln` in Visual Studio 2026 and build `Release|x64`, or run:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m`
- MSBuild invokes CMake to build SpeexDSP and RNNoise into `build\speexdsp` and `build\rnnoise`.
```

- [ ] **Step 3: Update AGENTS quick context**

In `AGENTS.md`, replace the first quick-context bullets with:

```markdown
- SpeexDSP and RNNoise are git submodules; MSBuild invokes CMake helper projects to build them before linking the APO.
- Build target is x64; Visual Studio 2026 (v18) with WDK 28000 is the standard build path.
- Primary project file: `AecApo.vcxproj`.
- Installer script: `installer\\sign-install.ps1` (requires admin for install).
- RNNoise model sources live in `rnnoise_model\\` (built into rnnoise.lib).
```

- [ ] **Step 4: Update AGENTS workflow**

In `AGENTS.md`, replace the manual VS 2022 helper build commands with:

```markdown
- Submodules: `git submodule update --init --recursive`
- Build APO and third-party helper libraries (x64):
  - `& 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m`
- Build APO (x64) from Visual Studio:
  - Open `AecApo.sln` (or `AecApo.vcxproj`) in Visual Studio 2026 and build `Release|x64`.
```

If Task 1 removed the WDK PackageReference fallback, add this note to both files:

```markdown
Note: The project pins Windows SDK `10.0.28000.0` and uses the locally installed WDK when native WDK NuGet restore is unavailable.
```

- [ ] **Step 5: Commit docs**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- README.md AGENTS.md
& 'C:\Program Files\Git\cmd\git.exe' commit -m "docs: document VS 2026 WDK build path"
```

### Task 5: Verify Builds and WDK Tooling

**Files:**
- No source edits expected.
- If WDK PackageReference restore fails and fallback is used, modify `README.md` and `AGENTS.md` as described in Task 4.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: evidence that the project builds cleanly with VS 2026 and SDK 28000.

- [ ] **Step 1: Verify Release x64**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Expected:

```text
AecApo.vcxproj -> C:\Users\msdx321\Desktop\workspace\AecAPO\build\x64\Release\AecApo.dll
```

The output must include SDK `10.0.28000.0` and must not include `LNK4098`.

- [ ] **Step 2: Verify Debug x64**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected:

```text
AecApo.vcxproj -> C:\Users\msdx321\Desktop\workspace\AecAPO\build\x64\Debug\AecApo.dll
```

The output must include SDK `10.0.28000.0` and must not include `LNK4098`.

- [ ] **Step 3: Run available INF tool**

Run:

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:installer /os:10_GE_X64 /verbose
```

Expected: exit code `0`, or a specific INF/package issue to fix before completion. `10_GE_X64` is listed by the local WDK 28000 `Inf2Cat.exe` help output.

- [ ] **Step 4: Inspect final status**

Run:

```powershell
& 'C:\Program Files\Git\cmd\git.exe' status --short
```

Expected: only unrelated pre-existing dirty files remain, or a clean tree if all dirty files were part of this implementation.

- [ ] **Step 5: Commit verification doc fallback if needed**

If Task 5 required documentation changes for WDK fallback or INF tool availability, commit them:

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- README.md AGENTS.md
& 'C:\Program Files\Git\cmd\git.exe' commit -m "docs: record WDK verification fallback"
```

If no files changed in Task 5, do not create an empty commit.
