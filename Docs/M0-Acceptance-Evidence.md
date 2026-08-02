# M0 acceptance evidence

> Status: local acceptance complete; hosted CI evidence pending publication
>
> Audited: 2026-08-02
>
> Baseline revision: `ec7f9f4` plus the current working-tree change set

## Acceptance audit

| Development-plan criterion | Authoritative evidence | Result |
|---|---|---|
| Every section 7 authoritative system has a module, API boundary and state owner | [`current_architecture_audit.md`](current_architecture_audit.md), section 3, maps all nine systems: `PaneStore`, `FileSystemProvider`, `OperationEngine`, `SearchEngine`, `ConnectionManager`, `PermissionManager`, `CommandRegistry`, `VisualStateMapper` and `SettingsStore`. | Local evidence complete |
| Every explicit Core/P0/Alpha requirement is assigned to a milestone | [`feature_gap_matrix.md`](feature_gap_matrix.md), sections 3–7, contains 42 unique requirement rows and an explicit coverage index for Core Product, every P0 group and Alpha. | Local evidence complete |
| Unsigned build and aggregate unit tests run through one command locally and in CI | [`Scripts/verify_m0.sh`](../Scripts/verify_m0.sh) builds `WinCommander-Unsigned` and delegates to the aggregate unit runner. [`.github/workflows/m0-verification.yml`](../.github/workflows/m0-verification.yml) invokes the same command on `macos-26` with Xcode 26.5. | Local execution complete; hosted execution pending |
| The local vertical slice follows the canonical feature cycle | [`Features/local_explorer_vertical_slice.md`](Features/local_explorer_vertical_slice.md) covers user problem, behavior, scenarios, UI states, commands, data model, provider capabilities, operation lifecycle, errors, edge cases, accessibility, acceptance and tests. | Local evidence complete |
| Reproducible 10,000/100,000-item baseline exists | [`Performance/large_folder_baseline_2026-08-01.md`](Performance/large_folder_baseline_2026-08-01.md) records the environment, command, deterministic input and measured model load/natural-sort results. | Local evidence complete |

## Verification contract

Run from the repository root:

```sh
Scripts/verify_m0.sh
```

The current local gate builds the unsigned Debug application and executes every product discovered from the aggregate `UnitTests` scheme. The fixed Catch2 seed is `424242`; an exploratory seed can be supplied through `WINCOMMANDER_TEST_SEED`.

Recorded evidence:

- 2026-08-01 — full seeded local gate: unsigned Debug application plus ten aggregate binaries, 897 cases / 132,011 assertions;
- 2026-08-02 — focused script contract checks: valid ten-product discovery succeeds, missing `ViewerUT` fails before build, Xcode 99.0 is rejected, and an invalid `DEVELOPER_DIR` is rejected;
- 2026-08-02 — `bash -n` passes for the M0 and aggregate unit runners, and the workflow parses as YAML.

The workflow contract was checked on 2026-08-02 against the current GitHub-hosted macOS 26 image: the `macos-26` label, `/Applications/Xcode_26.5.app` and `actions/checkout@v6` are available. The workflow file also parses as YAML locally. This compatibility check is preparation evidence, not a substitute for a hosted run.

## Remaining closure condition

The public `main` branch does not yet contain the M0 workflow or this change set, so GitHub Actions has no run for the current tree. M0 remains `partial` until the change is intentionally published and `M0 Verification` completes successfully for the published commit. Record the commit SHA, run URL, conclusion and test totals here and in [`Development-Plan.md`](Development-Plan.md); hosted required-check policy remains an M7 gate.
