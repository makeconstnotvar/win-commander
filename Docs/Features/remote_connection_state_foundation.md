# Q2-5 RC-1: Remote connection state and retry policy

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-5.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §46 (Remote Connection Manager: credentials state, status, last successful connection, error history; credentials in Keychain, never logged, typed authentication errors), §23 (remote operations go through the Operation Engine and ConnectionManager).

## Scope

The connection-state and retry rules Q2-5 needs before any of its surfaces can be built: what a remote connection's status is, which failures may be retried automatically, how long to wait, and what a connection remembers.

This is where the decisions with consequences live, and none of it needs a socket. Not here: Keychain storage, the manager UI, actual reconnect scheduling, and system SMB/NFS mounts.

## The decision that matters: what must never be retried

`IsRetryableRemoteFailure` admits exactly two failures — `Unreachable` and `TimedOut`. Everything else is excluded on purpose:

- **`AuthenticationRejected`** — repeating a rejected credential is precisely how an account gets locked out, and the credential cannot fix itself between attempts. An automatic retry here turns one typo into a lockout.
- **`HostVerificationFailed`** — a mismatched host key or certificate is the signature of an interception. Retrying until it succeeds would defeat the check entirely, so this must stop and reach the user.
- **`PermissionDenied`, `ProtocolError`** — nothing about either changes with time.

The exclusion holds even on the first attempt with the whole budget unspent, which is what the tests assert.

Because of that split, a connection that gives up does **not** land in one generic failed state. A retryable failure that exhausts its budget becomes `Offline`; a non-retryable one becomes `Blocked` immediately. They need different things from the user — try again later, versus supply new credentials or make a trust decision — and collapsing them would make the manager unable to say which.

## The type holds no credentials

`RemoteConnectionState` has nowhere to put a password, token or key. §46 requires credentials to live in the Keychain and never be logged, and the cheapest way to keep that true is for the type that gets copied into snapshots, error histories and UI to be structurally incapable of carrying them. The error history stores a provider-supplied `detail` string, which is the one place a careless caller could leak something — called out in the header so the Keychain increment has to answer it.

## Backoff details worth naming

- The delay is chosen from the attempts completed **before** the current failure. Getting this wrong was a real defect caught by these tests: the first retry waited `initial × multiplier` instead of `initial`, so every connection's first reconnect was twice as slow as configured.
- Growth is stepped and clamped as it goes rather than computed with `pow` and clamped afterwards, because a large attempt count overflows to infinity long before the ceiling would matter.
- A `multiplier` below 1 is treated as 1: backoff never shrinks.
- `maximum_attempts = 0` disables automatic reconnection outright, which is a real configuration rather than an edge case.
- A success resets the attempt budget. Without that, a link that flaps over a long uptime would eventually exhaust its retries and refuse to come back.
- A failed attempt clears the latency sample: keeping the old one would show a stale "fast" reading beside an offline connection.
- An explicit disconnect is not a failure — it clears the budget but keeps the last-success timestamp and the history, which are what the manager lists.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the slice adds files to the Xcode project).
- Focused `WinCommanderUT 'nc::core::RemoteConnection*' --rng-seed 424242`: **8/8 cases, 69/69 assertions** — the non-retryable set refused even on a fresh budget; the exponential sequence, ceiling clamp, sub-1 multiplier, and zero-attempt policy; a retryable failure exhausting into `Offline` versus a non-retryable one blocking at once; bounded newest-first history; success resetting the budget and restoring the full backoff sequence; read-only reporting and stale-latency clearing; explicit disconnect; a no-op outcome.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **674/674 cases, 11,107/11,107 assertions**.
- No ASAN/UBSAN/TSan run: a pure value model with no allocation ownership, concurrency or `Operations`/`VFS`/`RoutedIO` involvement. The increment that performs real reconnection scheduling is where that budget changes.

### Coverage gaps

- **Latency is stored but nothing classifies it.** §46 asks for a status; a "degraded" threshold is a presentation decision the manager surface should own, and inventing one here without a surface to show it would be guessing.
- No Keychain, no scheduler, no UI, and no system SMB/NFS mount handling — all later Q2-5 increments.
