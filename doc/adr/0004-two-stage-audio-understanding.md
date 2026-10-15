# ADR 0004: Two-Stage Audio Understanding (DSP Analysis + Perceptual Critique)

<!-- docs-scope: decision-record -->

- **Status**: accepted
- **Date**: 2026-10-14
- **Domain**: domain-audio-dsp
- **Owner**: Audio DSP Maintainers

---

## Context

An autonomous music agent must be able to evaluate its creative output. Relying purely on multimodal audio LLMs to inspect waveforms leads to hallucinated volume measurements, missed digital clipping, and inaccurate timing judgments. Conversely, purely technical metrics (RMS, peak) cannot determine whether a chord progression sounds melancholic or a vocal chop is rhythmically compelling.

## Decision

We split audio evaluation into two distinct, decoupled stages:
1. **Technical DSP Analysis (`audio.analyze`)**: Native C++ DSP calculations that deterministically report exact physical properties: peak dBFS, RMS power, clipping sample count, silence ratio, and spectral centroid.
2. **Subjective Perceptual Critique (`audio.listen`)**: Rendered audio is submitted to an explicitly configured external multimodal audio reasoning provider (`role: "audio"` in `session.provider.set`) to obtain creative qualitative feedback.

## Consequences

- **Positive**: Strict objective verification of technical mixing rules (zero digital clipping, adequate headroom).
- **Positive**: Clear separation between mathematical DSP facts and subjective musical aesthetic critique.
- **Trade-off**: Requires network access and an API key when executing the perceptual listening stage.
