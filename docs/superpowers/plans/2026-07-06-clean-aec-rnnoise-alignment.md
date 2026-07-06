# Clean AEC and RNNoise Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve capture quality by keeping RNNoise denoise, keeping Speex AEC, removing RNNoise speech gating, and making render-reference alignment measured and observable.

**Architecture:** Keep the existing APO and helper libraries. Refactor the realtime path in-place around three clear responsibilities: RNNoise denoise always runs on complete capture frames, Speex AEC only runs with a trustworthy render reference, and alignment state is tracked with timestamps, energy, and counters. Use conservative behavior first, then enable measured delay selection after diagnostics are present.

**Tech Stack:** Visual Studio 2026/MSBuild 18, Windows SDK/WDK 28000, C++20, ATL/COM APO, SpeexDSP, RNNoise.

## Global Constraints

- Keep RNNoise in the pipeline as denoise.
- Do not use RNNoise VAD to mute, zero, or strongly attenuate speech frames.
- Keep Speex AEC for echo cancellation only.
- If render reference alignment is not trustworthy, bypass Speex AEC for that frame and still run RNNoise.
- Do not change COM identities, INF identities, installer signing behavior, RNNoise model data, or third-party source code.
- Do not add tests unless explicitly requested by the user.
- Verify with Release and Debug x64 builds and WDK tooling.
- Preserve VS 2026 and WDK 28000 project settings.
- Keep realtime code non-blocking and allocation-free after `LockForProcess`.

---

## File Structure

- Modify `src/AecApoMfx.cpp`: pipeline behavior, reference alignment, counters, and diagnostics.
- Modify `src/AecApo.h`: private state for alignment counters, energy history, and delay estimate.
- Modify `README.md`: add a short troubleshooting note for AEC/RNNoise diagnostics if counters are exposed in logs.
- No changes to `third_party/`, `rnnoise_model/`, COM GUIDs, INF identities, or installer identities.

### Task 1: Remove RNNoise Speech Gating

**Files:**
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: existing `ProcessRnnoiseFrame(std::vector<float>&, size_t)`.
- Produces: RNNoise denoise that always writes processed output when `rnnoiseReady == true`.

- [ ] **Step 1: Remove low-confidence attenuation constants**

In `src/AecApoMfx.cpp`, remove this constant:

```cpp
constexpr float kRnnoiseVadLowConfidenceGain = 0.25f;
```

Keep:

```cpp
constexpr float kRnnoiseVadThreshold = 0.6f;
constexpr int kRnnoiseVadGraceMs = 200;
```

They may still be used for diagnostics during this pass.

- [ ] **Step 2: Make RNNoise VAD non-destructive**

In `CAecApoMFX::ProcessRnnoiseFrame`, remove the `attenuateLowConfidenceSpeech` variable and the block that scales `m_rnnoiseOutputScratch` by `kRnnoiseVadLowConfidenceGain`.

The post-`rnnoise_process_frame` logic should become:

```cpp
const float vad = rnnoise_process_frame(m_rnnoiseState.get(),
                                        m_rnnoiseOutputScratch.data(),
                                        m_rnnoiseInputScratch.data());
if (vad >= kRnnoiseVadThreshold)
{
    m_rnnoiseVadGraceSamplesRemaining = (kRnnoiseSampleRateHz * kRnnoiseVadGraceMs) / 1000;
}
else if (m_rnnoiseVadGraceSamplesRemaining > 0)
{
    m_rnnoiseVadGraceSamplesRemaining -= m_rnnoiseFrameSize;
    if (m_rnnoiseVadGraceSamplesRemaining < 0)
    {
        m_rnnoiseVadGraceSamplesRemaining = 0;
    }
}
```

Do not use the VAD result to zero or attenuate output.

- [ ] **Step 3: Verify builds**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 4: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "fix: keep RNNoise denoise non-gating"
```

### Task 2: Add Realtime-Safe Alignment Counters

**Files:**
- Modify: `src/AecApo.h`
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: current render-reference ring and capture processing path.
- Produces: counters that measure frame flow and AEC reference selection.

- [ ] **Step 1: Add counter state**

In `src/AecApo.h`, add these private members near the render-reference state:

```cpp
std::atomic<uint64_t> m_captureFramesProcessed{0};
std::atomic<uint64_t> m_renderFramesPublished{0};
std::atomic<uint64_t> m_aecFramesProcessed{0};
std::atomic<uint64_t> m_aecFramesBypassedNoReference{0};
std::atomic<uint64_t> m_aecFramesBypassedBadReference{0};
std::atomic<uint64_t> m_rnnoiseFramesProcessed{0};
std::atomic<int64_t> m_lastReferenceDeltaQpc{0};
std::atomic<uint64_t> m_estimatedEchoDelayQpc{0};
```

Use atomics because `AcceptInput` and `APOProcess` are not synchronized.

- [ ] **Step 2: Reset counters on lock initialization**

In `InitializeProcessingBuffers`, after `ResetRenderReferenceState();`, reset all counters:

```cpp
m_captureFramesProcessed.store(0, std::memory_order_relaxed);
m_renderFramesPublished.store(0, std::memory_order_relaxed);
m_aecFramesProcessed.store(0, std::memory_order_relaxed);
m_aecFramesBypassedNoReference.store(0, std::memory_order_relaxed);
m_aecFramesBypassedBadReference.store(0, std::memory_order_relaxed);
m_rnnoiseFramesProcessed.store(0, std::memory_order_relaxed);
m_lastReferenceDeltaQpc.store(0, std::memory_order_relaxed);
m_estimatedEchoDelayQpc.store((m_qpcTicksPerSecond * 20) / 1000, std::memory_order_relaxed);
```

- [ ] **Step 3: Increment counters in frame flow**

In `PublishRenderReferenceFrame`, after publishing the frame, add:

```cpp
m_renderFramesPublished.fetch_add(1, std::memory_order_relaxed);
```

In `APOProcess`, after popping each complete capture frame for processing, add:

```cpp
m_captureFramesProcessed.fetch_add(1, std::memory_order_relaxed);
```

In `ProcessRnnoiseFrame`, after successful `rnnoise_process_frame`, add:

```cpp
m_rnnoiseFramesProcessed.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 4: Verify builds**

Run the Release and Debug MSBuild commands from Task 1.

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 5: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApo.h src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "chore: add AEC alignment counters"
```

### Task 3: Classify Reference Lookup Failures

**Files:**
- Modify: `src/AecApo.h`
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: `TryGetRenderReferenceFrame(UINT64, float*, size_t)`.
- Produces: explicit reference lookup status so AEC bypass reasons are observable.

- [ ] **Step 1: Add lookup status enum**

In `src/AecApo.h`, add this private enum before helper method declarations:

```cpp
enum class ReferenceLookupStatus
{
    kMatched,
    kNoReference,
    kOutOfWindow,
    kConcurrentWrite
};
```

Change the helper declaration from:

```cpp
bool TryGetRenderReferenceFrame(UINT64 captureQpc, float *outFrame, size_t frameSize);
```

to:

```cpp
ReferenceLookupStatus TryGetRenderReferenceFrame(UINT64 captureQpc,
                                                 float *outFrame,
                                                 size_t frameSize,
                                                 UINT64 *matchedReferenceQpc);
```

- [ ] **Step 2: Return classified statuses**

In `src/AecApoMfx.cpp`, update the method signature and return values:

```cpp
CAecApoMFX::ReferenceLookupStatus CAecApoMFX::TryGetRenderReferenceFrame(UINT64 captureQpc,
                                                                          float *outFrame,
                                                                          size_t frameSize,
                                                                          UINT64 *matchedReferenceQpc)
```

Use:

```cpp
return ReferenceLookupStatus::kNoReference;
```

for missing buffers or `published == 0`.

Use:

```cpp
return ReferenceLookupStatus::kOutOfWindow;
```

when a frame exists but `bestDelta > toleranceQpc`.

After copying the selected frame, return:

```cpp
if (matchedReferenceQpc != nullptr)
{
    *matchedReferenceQpc = m_renderReferenceQpc[bestSlot];
}
if (endSequence == bestSequence && ((endSequence & 1u) == 0))
{
    return ReferenceLookupStatus::kMatched;
}
return ReferenceLookupStatus::kConcurrentWrite;
```

- [ ] **Step 3: Count bypass reasons in Speex path**

In `ProcessSpeexFrame`, replace the boolean check with:

```cpp
const ReferenceLookupStatus lookupStatus = TryGetRenderReferenceFrame(captureQpc,
                                                                      renderFrameScratch.data(),
                                                                      frameSize,
                                                                      nullptr);
if (lookupStatus != ReferenceLookupStatus::kMatched)
{
    if (lookupStatus == ReferenceLookupStatus::kNoReference)
    {
        m_aecFramesBypassedNoReference.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        m_aecFramesBypassedBadReference.fetch_add(1, std::memory_order_relaxed);
    }
    return;
}
```

After `speex_echo_cancellation`, add:

```cpp
m_aecFramesProcessed.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 4: Verify builds**

Run the Release and Debug MSBuild commands from Task 1.

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 5: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApo.h src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "refactor: classify AEC reference lookup"
```

### Task 4: Replace Capture FIFO Timing with a Capture Frame Assembler

**Files:**
- Modify: `src/AecApo.h`
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: packet-sized capture samples from `APOProcess`.
- Produces: complete capture frames plus accurate frame start QPC, including frames that cross packet boundaries.

- [ ] **Step 1: Add capture assembly state**

In `src/AecApo.h`, add:

```cpp
std::vector<float> m_captureAssemblyScratch;
size_t m_captureAssemblyCount = 0;
UINT64 m_captureAssemblyStartQpc = 0;
```

- [ ] **Step 2: Reset cursor state**

In `InitializeProcessingBuffers`, set:

```cpp
m_captureAssemblyScratch.assign(m_frameSize, 0.0f);
m_captureAssemblyCount = 0;
m_captureAssemblyStartQpc = 0;
```

- [ ] **Step 3: Add capture frame processing helper**

In `src/AecApo.h`, add this private helper declaration:

```cpp
void ProcessCaptureFrame(const float *frameData, UINT64 captureFrameQpc);
```

In `src/AecApoMfx.cpp`, add this method near `ProcessSpeexFrame`:

```cpp
void CAecApoMFX::ProcessCaptureFrame(const float *frameData, UINT64 captureFrameQpc)
{
    if (frameData == nullptr || m_frameSize == 0 || m_captureFrameScratch.size() < m_frameSize)
    {
        return;
    }

    std::copy_n(frameData, m_frameSize, m_captureFrameScratch.data());
    m_captureFramesProcessed.fetch_add(1, std::memory_order_relaxed);
    ProcessSpeexFrame(m_captureFrameScratch, m_frameSize, captureFrameQpc);
    ProcessRnnoiseFrame(m_captureFrameScratch, m_frameSize);
    m_outputFifo.Push(m_captureFrameScratch.data(), m_frameSize);
}
```

- [ ] **Step 4: Add capture frame assembler helper**

In `src/AecApo.h`, add this private helper declaration:

```cpp
void QueueCaptureSamples(const float *samples, size_t sampleCount, UINT64 firstSampleQpc);
```

In `src/AecApoMfx.cpp`, add this method near `QueueRenderReferenceSamples`:

```cpp
void CAecApoMFX::QueueCaptureSamples(const float *samples, size_t sampleCount, UINT64 firstSampleQpc)
{
    if (samples == nullptr || sampleCount == 0 || m_frameSize == 0 || m_captureAssemblyScratch.size() < m_frameSize)
    {
        return;
    }

    UINT64 currentSampleQpc = firstSampleQpc;
    while (sampleCount > 0)
    {
        if (m_captureAssemblyCount == 0)
        {
            m_captureAssemblyStartQpc = currentSampleQpc;
        }

        const size_t remaining = m_frameSize - m_captureAssemblyCount;
        const size_t chunk = (std::min)(sampleCount, remaining);
        std::copy_n(samples, chunk, m_captureAssemblyScratch.data() + m_captureAssemblyCount);

        m_captureAssemblyCount += chunk;
        samples += chunk;
        sampleCount -= chunk;
        if (currentSampleQpc != 0)
        {
            currentSampleQpc += SamplesToQpcTicks(chunk, m_sampleRateHz);
        }

        if (m_captureAssemblyCount == m_frameSize)
        {
            ProcessCaptureFrame(m_captureAssemblyScratch.data(), m_captureAssemblyStartQpc);
            m_captureAssemblyCount = 0;
            m_captureAssemblyStartQpc = 0;
        }
    }
}
```

- [ ] **Step 5: Use the capture assembler in APOProcess**

In `APOProcess`, replace the capture FIFO push and processing loop:

```cpp
if (m_captureFifo.Count() == 0)
{
    m_captureFifoStartQpc = inputQpc;
}
m_captureFifo.Push(m_captureScratch.data(), frames);

while (m_captureFifo.Count() >= m_frameSize)
{
    const UINT64 captureFrameQpc = m_captureFifoStartQpc;
    m_captureFifo.Pop(m_captureFrameScratch.data(), m_frameSize);
    ProcessSpeexFrame(m_captureFrameScratch, m_frameSize, captureFrameQpc);
    ProcessRnnoiseFrame(m_captureFrameScratch, m_frameSize);
    m_outputFifo.Push(m_captureFrameScratch.data(), m_frameSize);
    if (m_captureFifoStartQpc != 0)
    {
        m_captureFifoStartQpc += SamplesToQpcTicks(m_frameSize, m_sampleRateHz);
    }
}
```

with:

```cpp
QueueCaptureSamples(m_captureScratch.data(), frames, inputQpc);
```

Do not remove `m_captureFifo` in this task. Leaving it unused keeps the edit smaller and makes cleanup a later reviewable change.

- [ ] **Step 6: Verify builds**

Run the Release and Debug MSBuild commands from Task 1.

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 7: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApo.h src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "fix: assemble capture frames with accurate timestamps"
```

### Task 5: Add Energy-Based Delay Estimator

**Files:**
- Modify: `src/AecApo.h`
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: render-reference ring, capture frames, and alignment counters.
- Produces: smoothed `m_estimatedEchoDelayQpc` used by reference selection.

- [ ] **Step 1: Add constants**

In the anonymous namespace in `src/AecApoMfx.cpp`, add:

```cpp
constexpr int kInitialEchoDelayMs = 20;
constexpr int kMinEchoDelayMs = 0;
constexpr int kMaxEchoDelayMs = 250;
constexpr int kDelayUpdateSmoothingShift = 4;
constexpr float kAlignmentEnergyFloor = 1.0e-5f;
```

Replace direct uses of `20` for initial delay with `kInitialEchoDelayMs`.

- [ ] **Step 2: Add energy helper**

In the anonymous namespace in `src/AecApoMfx.cpp`, add:

```cpp
static float ComputeMeanSquareEnergy(const float *samples, size_t count)
{
    if (samples == nullptr || count == 0)
    {
        return 0.0f;
    }

    double sum = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double sample = samples[i];
        sum += sample * sample;
    }
    return static_cast<float>(sum / static_cast<double>(count));
}
```

- [ ] **Step 3: Store render frame energy**

In `src/AecApo.h`, add:

```cpp
std::vector<float> m_renderReferenceEnergy;
```

In `InitializeProcessingBuffers`, initialize it:

```cpp
m_renderReferenceEnergy.assign(m_renderReferenceSlotCount, 0.0f);
```

In `PublishRenderReferenceFrame`, set:

```cpp
m_renderReferenceEnergy[slot] = ComputeMeanSquareEnergy(frameData, m_frameSize);
```

- [ ] **Step 4: Update selected reference delta**

In `TryGetRenderReferenceFrame`, when a matched frame is selected and `targetQpc != 0`, store:

```cpp
const int64_t signedDelta = static_cast<int64_t>(m_renderReferenceQpc[bestSlot]) -
                            static_cast<int64_t>(targetQpc);
m_lastReferenceDeltaQpc.store(signedDelta, std::memory_order_relaxed);
```

- [ ] **Step 5: Smooth estimated delay conservatively**

After selecting a valid reference frame, update `m_estimatedEchoDelayQpc` only if both capture and render energy are above `kAlignmentEnergyFloor`.

Use this logic in `ProcessSpeexFrame` before calling `speex_echo_cancellation`:

```cpp
UINT64 matchedReferenceQpc = 0;
const ReferenceLookupStatus lookupStatus = TryGetRenderReferenceFrame(captureQpc,
                                                                      renderFrameScratch.data(),
                                                                      frameSize,
                                                                      &matchedReferenceQpc);
if (lookupStatus != ReferenceLookupStatus::kMatched)
{
    if (lookupStatus == ReferenceLookupStatus::kNoReference)
    {
        m_aecFramesBypassedNoReference.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        m_aecFramesBypassedBadReference.fetch_add(1, std::memory_order_relaxed);
    }
    return;
}

const float captureEnergy = ComputeMeanSquareEnergy(captureFrameScratch.data(), frameSize);
const float renderEnergy = ComputeMeanSquareEnergy(renderFrameScratch.data(), frameSize);
if (captureQpc > matchedReferenceQpc &&
    captureEnergy > kAlignmentEnergyFloor &&
    renderEnergy > kAlignmentEnergyFloor)
{
    uint64_t estimate = m_estimatedEchoDelayQpc.load(std::memory_order_relaxed);
    const uint64_t measuredDelay = captureQpc - matchedReferenceQpc;
    const uint64_t minDelay = (m_qpcTicksPerSecond * kMinEchoDelayMs) / 1000;
    const uint64_t maxDelay = (m_qpcTicksPerSecond * kMaxEchoDelayMs) / 1000;
    const uint64_t clamped = (std::max)(minDelay, (std::min)(measuredDelay, maxDelay));
    const int64_t delta = static_cast<int64_t>(clamped) - static_cast<int64_t>(estimate);
    const int64_t step = delta / (1 << kDelayUpdateSmoothingShift);
    const uint64_t smoothed = static_cast<uint64_t>(static_cast<int64_t>(estimate) + step);
    m_estimatedEchoDelayQpc.store(smoothed, std::memory_order_relaxed);
}
```

This is deliberately conservative. It prevents jumps but allows slow correction.

- [ ] **Step 6: Use estimated delay for lookup**

In `TryGetRenderReferenceFrame`, replace:

```cpp
const UINT64 delayQpc = (m_qpcTicksPerSecond * kRenderReferenceDelayMs) / 1000;
```

with:

```cpp
const UINT64 delayQpc = m_estimatedEchoDelayQpc.load(std::memory_order_relaxed);
```

Keep `kRenderReferenceToleranceMs` for this task.

- [ ] **Step 7: Verify builds**

Run the Release and Debug MSBuild commands from Task 1.

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 8: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApo.h src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "feat: estimate AEC reference delay from frame energy"
```

### Task 6: Disable Speex Preprocess Denoise by Default

**Files:**
- Modify: `src/AecApoMfx.cpp`

**Interfaces:**
- Consumes: Speex AEC output and RNNoise denoise stage.
- Produces: single-denoiser behavior, with RNNoise as the denoise stage.

- [ ] **Step 1: Stop creating Speex preprocess state**

In `InitializeSpeexProcessors`, keep:

```cpp
m_speexPreprocessState.reset();
```

Remove the block that calls `speex_preprocess_state_init` and all `speex_preprocess_ctl` calls.

- [ ] **Step 2: Keep ProcessSpeexFrame tolerant**

Leave this existing guard in `ProcessSpeexFrame`:

```cpp
if (m_speexPreprocessState)
{
    speex_preprocess_run(m_speexPreprocessState.get(), speexOut16.data());
}
```

This keeps the code safe if preprocess is reintroduced later.

- [ ] **Step 3: Verify builds**

Run the Release and Debug MSBuild commands from Task 1.

Expected: both exit `0`; neither output contains `LNK4098`.

- [ ] **Step 4: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- src/AecApoMfx.cpp
& 'C:\Program Files\Git\cmd\git.exe' commit -m "fix: use RNNoise as the only denoise stage"
```

### Task 7: Document Diagnostics and Manual Tuning Procedure

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: alignment counters and build outputs from prior tasks.
- Produces: operator guidance for diagnosing weak AEC without guessing.

- [ ] **Step 1: Add troubleshooting section**

In `README.md`, add:

```markdown
AEC/RNNoise tuning notes
- RNNoise is used for denoise and must not gate speech.
- Speex AEC runs only when the APO has a trustworthy render reference frame.
- If echo cancellation is weak, inspect reference availability before changing DSP constants.
- Useful counters are capture frames, render frames, AEC processed frames, AEC bypass reasons, selected reference delta, estimated delay, and RNNoise frames.
- High AEC bypass counts usually mean reference stream or timestamp alignment problems, not an RNNoise problem.
```

- [ ] **Step 2: Verify docs text**

Run:

```powershell
rg -n "RNNoise is used for denoise|Speex AEC runs only|AEC bypass" README.md
```

Expected: all three phrases appear.

- [ ] **Step 3: Commit**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' add -- README.md
& 'C:\Program Files\Git\cmd\git.exe' commit -m "docs: add AEC tuning notes"
```

### Task 8: Final Verification

**Files:**
- No source edits expected.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: build and package evidence for the full pipeline change.

- [ ] **Step 1: Run Release build**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Expected: exit `0`, output includes `build\x64\Release\AecApo.dll`, no `LNK4098`.

- [ ] **Step 2: Run Debug build**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' AecApo.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Expected: exit `0`, output includes `build\x64\Debug\AecApo.dll`, no `LNK4098`.

- [ ] **Step 3: Run Inf2Cat package check**

Stage the Release DLL before `Inf2Cat`, matching `installer\sign-install.ps1` behavior:

```powershell
Copy-Item -LiteralPath build\x64\Release\AecApo.dll -Destination installer\AecApo.dll -Force
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe' /driver:installer /os:10_GE_X64 /verbose
```

Expected: exit `0`, `Errors: None`, `Warnings: None`.

- [ ] **Step 4: Inspect final status**

```powershell
& 'C:\Program Files\Git\cmd\git.exe' status --short
```

Expected: no uncommitted files, except generated installer artifacts if the local workflow leaves them dirty. Do not commit generated binaries unless explicitly requested.
