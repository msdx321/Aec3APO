# Clean AEC and RNNoise Alignment Design

## Goal

Improve capture sound quality by keeping RNNoise as the denoiser, keeping Speex as the echo canceller, and replacing fragile fixed-delay reference alignment with measured, observable alignment.

## Problem

The current pipeline can create cutoffs, noise artifacts, and delay because it mixes three separate concerns in one realtime path:

- Speex AEC cancels render echo only when a render reference frame is selected.
- Speex preprocess adds additional denoise and echo suppression after AEC.
- RNNoise both denoises and attenuates low-confidence speech.

The worst audible artifact is likely the RNNoise low-confidence attenuation. RNNoise VAD is useful as a signal, but using it to reduce speech frames to `25%` can create abrupt cutoffs. The AEC weakness is likely a separate alignment problem: the code uses a fixed `20ms` reference delay and a broad `120ms` tolerance, which is not robust across endpoint periods, render devices, resampling delay, and scheduler timing.

## Desired Pipeline

Use a transparent-first pipeline:

```text
capture packet
  -> mono extraction
  -> capture frame assembly
  -> Speex AEC when a reliable render reference exists
  -> RNNoise denoise on every frame
  -> output packet
```

RNNoise remains enabled for denoise. Its VAD score must not mute, zero, or strongly attenuate speech. Speex remains responsible for echo cancellation only. If the render reference is missing or not aligned, Speex AEC is bypassed for that frame, but RNNoise still runs.

## Alignment Model

The reference alignment must move from fixed delay to measured delay:

1. Store render reference frames in a circular buffer with frame samples, frame start QPC, sequence, and basic energy.
2. Store capture frame start QPC using packet QPC plus the consumed sample offset.
3. Track a smoothed estimated echo delay in QPC ticks.
4. Select the render reference frame nearest to:

```text
captureFrameQpc - estimatedEchoDelayQpc
```

5. Update the estimated delay only when render and capture energy are high enough to produce a useful correlation signal.
6. Clamp and smooth delay updates so the estimate cannot jump abruptly.

The first implementation should be conservative: add diagnostics and counters before using correlation to drive behavior. Then enable measured delay selection behind constants or a small internal mode switch.

## RNNoise Behavior

RNNoise should process every complete frame that reaches the denoise stage. The VAD score may be logged for diagnostics, but it must not:

- zero output
- attenuate output based on speech confidence
- control whether the frame is copied to the output

This preserves voice continuity even when noise remains.

## Speex Behavior

Speex should receive a render reference only when a reference frame is available within the selected tolerance around the estimated delay. If no reliable reference exists, the frame should pass through to RNNoise without AEC. This avoids running the echo canceller with the wrong reference, which can make speech sound worse than bypass.

Speex preprocess denoise should be disabled initially. If extra noise suppression is needed after alignment is stable, reintroduce only one additional suppressor at a time.

## Diagnostics

Add low-overhead counters that can be logged outside the realtime path:

- capture frames processed
- render frames received
- render frames published
- AEC frames processed
- AEC frames bypassed due to missing reference
- AEC frames bypassed due to stale or out-of-window reference
- selected reference delta in milliseconds
- smoothed estimated delay in milliseconds
- RNNoise frames processed
- RNNoise average VAD score

These counters are necessary because subjective listening alone cannot identify whether the failure is reference availability, delay selection, FIFO behavior, or denoise aggressiveness.

## Microsoft APO Guidance Applied

The design follows the Windows APO/AEC guidance:

- `AcceptInput` receives auxiliary render reference data and must not block.
- `APOProcess` consumes capture data and uses a circular render-reference buffer.
- Auxiliary input calls are not synchronized with `APOProcess`, so missing reference audio must be handled as a normal condition.
- Timestamps on reference and microphone data should be used to line up speaker and mic data.
- APOs should avoid significant latency in the audio processing chain.

The SysVAD AecApo sample is a lifecycle and interface reference, not a production DSP reference.

## Non-Goals

- Do not remove RNNoise.
- Do not replace Speex AEC in this pass.
- Do not add a user-facing UI.
- Do not add unit tests unless explicitly requested.
- Do not change COM identities, INF identities, or installer behavior.

## Success Criteria

- Voice no longer cuts out because of RNNoise VAD gating.
- AEC bypass is explicit when reference alignment is not trustworthy.
- Delay alignment is observable through counters.
- Release and Debug x64 builds pass with VS 2026 and WDK 28000.
- `Inf2Cat` remains clean after staging the built DLL.
