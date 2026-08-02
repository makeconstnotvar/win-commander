# Large Folder Model Baseline — 2026-08-01

## Purpose

This baseline measures the existing panel data model while it loads and naturally sorts synthetic local-style directory listings. It establishes a repeatable M0 reference for 10,000 and 100,000 items before Explorer architecture changes begin.

The measurement covers `panel::data::Model::Load`, including natural name sorting. It does not cover filesystem enumeration, metadata reads, AppKit view creation, first visible render, scrolling, or memory use. Those end-to-end measurements remain part of the M2 Explorer and M7 release-hardening gates.

## Environment

- Hardware: MacBook Pro with Apple M3 Pro, 12 CPU cores, 18 GB memory
- Operating system: macOS 26.5.2 (25F84)
- Build: `PanelPT`, Release, arm64, code signing disabled
- Benchmark framework: Catch2 3.15.1
- Input: deterministic multilingual filenames generated with seed `42`
- Natural collation; one fresh `Model` per measured iteration
- 10 samples per listing size, 100 ms warm-up, statistical resampling disabled

## Reproduction

Build from the repository root:

```sh
xcodebuild \
  -project Source/WinCommander/WinCommander.xcodeproj \
  -scheme PanelPT \
  -configuration Release \
  -destination 'platform=macOS' \
  build CODE_SIGNING_ALLOWED=NO
```

Run the produced executable from Xcode's DerivedData `Build/Products/Release` directory:

```sh
PanelPT 'Large listing model baseline' \
  --benchmark-samples 10 \
  --benchmark-warmup-time 100 \
  --benchmark-no-analysis \
  --colour-mode none
```

## Results

| Scenario | Samples | Iterations per sample | Mean |
|---|---:|---:|---:|
| Load and natural-sort 10,000 items | 10 | 1 | 56.8585 ms |
| Load and natural-sort 100,000 items | 10 | 1 | 753.492 ms |

Both benchmark cases completed successfully.

## Interpretation

The 100,000-item model load is about 13.3 times slower than the 10,000-item load. This is consistent with sorting work growing faster than linearly, but it is not yet an application responsiveness result. The current data is useful as a regression anchor for model changes; it cannot establish the specification's first-render, scrolling, memory, or cancellation targets.

M2 must add the first instrumented Explorer scenario, and M7 must preserve it as a release gate, recording:

- directory enumeration start and completion;
- first usable rows and first complete visible frame;
- main-thread stalls and input latency during loading and sorting;
- peak and retained memory;
- sort, filter, and navigation latency on warm and cold data;
- cancellation and stale-result suppression during rapid navigation.

## Source

The benchmark cases live in [`Source/Panel/tests/Comparator_PT.cpp`](../../Source/Panel/tests/Comparator_PT.cpp).
