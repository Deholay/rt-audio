# AGENTS.md — rt-audio Project 3 (key-click suppression)

Guidance for coding agents working in `components/project3/`. Technical validation, not contract delivery.

## What this is

Real-time pipeline for suppressing saxophone key clicks. Architecture is **not** Project 2's:

```
mic → callback: input hop → Q_in (SPSC) → worker: AudioProcessor → Q_out (SPSC, processed audio + source index)
                    └→ dry delay ring ───────────────→ callback: at playback time pick processed block or aligned dry block → speaker
```

- Worker returns **processed audio**, not parameters. Callback plays it after a **fixed software delay**; late or missing results fall back to the time-aligned dry signal.
- Read first: [GUIDANCE.md](GUIDANCE.md) (reading order), [README.md](README.md) (ports, timing, overload contract).
- Default processor is `PassthroughProcessor`. KCS v23 is integrated as a release: `release/kcs/` ([SPEC](../../release/kcs/SPEC.md): hardware/stream spec §0, API §1), `rt_audio::kcs::make_processor()`, `project3_live --processor kcs-latest`. Device E2E latency is still unmeasured.

## Build and test

From `components/project3/` (standalone):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

From the rt-audio root:

```bash
make test               # P2 + P3 tests and sims
./build/project3_sim    # stops the worker mid-run, checks fixed-delay fallback sample by sample
make live               # needs PortAudio (bash scripts/bootstrap_portaudio.sh)
```

Run `make test` after any change here; `project3_tests` and `project3_sim` must pass. Do not touch `components/project2/` or `release/ahs/` for Project 3 work.

## Invariants (do not break)

- Callback: no user DSP, no alloc/free, no mutex, no waiting on the worker, no I/O. Bounded work (≤2048 frames, ≤32 output pops).
- Each queue has exactly one producer and one consumer. Do not add a second reader of `Engine::try_read_input` while `ProcessingWorker` runs.
- Output blocks keep the **source** `first_sample` (never add lookahead or delay), `frames == block_frames`, start on a hop boundary, strictly increasing, no overlap, no publishing beyond received input.
- Playback decision is made once per block at its first sample; late results are dropped, never spliced mid-block, never replayed.
- `delay_frames >= block_frames + lookahead_frames`, delay ≤ 16384. Queues stay 32 blocks; never grow, overwrite unread data or reset mid-run.
- Lifecycle on one control thread: start worker → start device; stop device → stop/join worker. Worker exceptions surface via `rethrow_if_failed()`, never into the callback.
- `on_audio` block storage is reused: copy into your own rolling window; no references kept.

## Plugging in an algorithm (KCS)

The algorithm lives in the KCS repo (`../KCS`, Python/NumPy research code). Current baseline per KCS README: v23 `scripts/item3_transient_v23.py` `TransientAR32` (16 kHz only, `latency_samples = 144` = 9 ms, `push()`/`flush()`/`process()`), with `scripts/item3_bridge48k.py` (`Bridge48k`, causal 61-tap FIR, 48↔16 kHz).

- Integrate as an `AudioProcessor` (C++ port). Python stays the reference, not the runtime.
- Follow the AHS release pattern in `release/ahs/` ([SPEC](../../release/ahs/SPEC.md)): versioned folder `release/kcs/kcs-<version>-<commit>/`, `PROVENANCE.md` with source repo/branch/commit, file SHA-256 and what was ported.
- Equivalence first: the C++ port must reproduce the KCS Python reference on the same input (state the tolerance; float32 vs float64 will not be bit-identical — report max error and where it occurs). Only then optimise.
- Algorithm changes made only in rt-audio go in the release adapter with their own design/verification files, and must say which KCS version they diverge from.
- Pick `lookahead_frames` / `delay_frames` from the algorithm's real need (v23: 144 samples @16 kHz = 432 @48 kHz, plus bridge delay), not the defaults.

## KCS rules that still apply

`../KCS/AGENTS.md` governs KCS itself. When reading or citing it:

- Never modify KCS frozen files (scorer metrics, `item3_transient_v15`…`v24.py`, `results/item3/vNN*`, prediction files). Work in rt-audio or on a new KCS version with a prediction first.
- Runtime data boundary: the processor gets only input audio and fs — never clean/key layers, `events_s`, case names or SNR.
- Material: only the pinned Release assets (`v13-material`, `holdout1…4-material`) with recomputed waveform digests; mismatch = stop.
- Separate source claims, own measurements, auditor results and hypotheses. Do not fabricate numbers, citations or results.

## Latency and evaluation

- Contract target: end-to-end ≤30 ms at 48 kHz, block ≤128, worst of 20 clicks (KCS `item3/test_plan_v3_2026-10.md`). E2E = software delay (`delay_frames`, default 512 = 10.67 ms) + device input/output buffering. Device part needs a wired interface + loopback; device-reported latency is not a measurement.
- Quality metrics (KSR, LSD, SNR) come from KCS's frozen scorer; do not reimplement or relax them.
- Timing numbers count only when run isolated with load recorded; otherwise label them reference-only.
- Do not judge versions on one case/path/seed: report distributions (lesson from Project 2, where single-path differences of up to 7 dB were noise).

## Reporting

- State the run environment (Local Mac: macOS, compiler, CPU, arm64), commit, commands run and their actual output.
- Say what was not verified (CMake, sanitizers, real device, real recordings) instead of implying it passed.
- Commit only when asked. Commit messages end with the attribution lines the session specifies.
