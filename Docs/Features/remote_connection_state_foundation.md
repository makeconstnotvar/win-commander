# Q2-5 RC-1…RC-5: Remote connection state, retry, host trust, pinning and presentation

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-5.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §46 (Remote Connection Manager: credentials state, status, last successful connection, error history; credentials in Keychain, never logged, typed authentication errors), §23 (remote operations go through the Operation Engine and ConnectionManager).

## Scope

The connection-state and retry rules Q2-5 needs before any of its surfaces can be built: what a remote connection's status is, which failures may be retried automatically, how long to wait, and what a connection remembers.

This is where the decisions with consequences live, and none of it needs a socket.

**RC-2** adds host verification — the classification that produces RC-1's `HostVerificationFailed` in the first place. **RC-3** attaches it to a pin store and enforces who may write a pin; **RC-4** implements that store on the macOS keychain; **RC-5** projects the whole thing into what a Connection Manager row shows.

Not here: the manager UI, actual reconnect scheduling, and system SMB/NFS mounts. Credential *storage* already exists (`KeychainServices` via `ConfigBackedNetworkConnectionsManager`); what §46 was missing was host verification, which is RC-2.

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

## RC-2: host trust

`ClassifyRemoteHost` compares a presented fingerprint against the pin held for that host and returns one of four verdicts. It is total and side-effect free: it never stores a pin and never upgrades a `Mismatch`. Deciding to pin a first-use host belongs to the caller that can actually ask the user.

**Normalization is a security control, not a convenience.** Providers spell one fingerprint several ways — `AA:BB:CC`, `aa bb cc`, `aabbcc` — and comparing them literally would warn about a host that never changed. That trains users to click through the one warning that must never become routine. So comparison is on lowercased hex with separators stripped.

But normalization *rejects* rather than coerces: anything that is not hex-or-separator, or that leaves an odd number of digits, is unusable. Silently dropping unexpected characters could map two different inputs onto the same normalized form, which is the opposite of what a fingerprint is for.

Three refusals worth naming:

- **An unusable presented fingerprint is `Unusable`, not `Mismatch` and not trust** — even when a pin exists, because there was nothing to compare against. It is a failure to verify, and it is reported as such.
- **An unparseable *stored* pin is `Mismatch`, not "no pin".** Degrading a corrupted or tampered store to first-use would silently downgrade an established host into one the user is invited to accept — which is exactly the attack pinning exists to stop.
- **`Mismatch` is never offered as a routine accept.** `MayPromptToTrust` admits only `UnknownFirstUse`. An established pin that suddenly disagrees puts a question to the user they cannot honestly answer from a dialog; replacing such a pin has to be a separate, explicit action.

## RC-3: pinning, and who may write a pin

`RemoteHostPinStore` is an interface rather than a direct Keychain call, so the decisions above it are testable without a keychain and so a **store that fails to persist** is a case the policy must answer rather than something that only appears on a device.

Pinning is split into two operations, and the split is the security property:

- **`TrustOnFirstUse`** is the routine path a connection flow may call after the user accepts an unknown host. It refuses — without touching the store at all — unless the live verdict is exactly `UnknownFirstUse`. So the button that accepts a new host can never also overwrite an established pin, which would silently dismiss an interception warning.
- **`ReplacePin`** is the deliberate path for a host whose key legitimately changed. It accepts a `Mismatch`, because that is the case it exists for; the explicit user decision lives in whatever calls it, which the policy cannot check for it.

Two further refusals:

- Neither path records an unverifiable fingerprint. For `ReplacePin` that matters especially: pinning garbage would leave the host permanently unverifiable rather than merely re-pinned.
- A store that cannot persist reports failure rather than pretending. A pin that did not become durable must not be reported as trusted, or the next launch would present the host as unknown again with no explanation.

`TrustOnFirstUse` re-reads the store rather than trusting a verdict the caller obtained earlier, so a pin written between a caller's `Verify` and its accept still wins.

## RC-4: pins on the keychain

A pin is **not a secret** — it is a public fingerprint — so keeping it in the keychain is not about confidentiality. It is about integrity: the keychain is harder to tamper with than a config file, and a silently edited pin is precisely how an interception warning would be made to disappear.

The service name embeds the provider and the host is the account, rather than concatenating the two into one string. Concatenation is the classic way `("a", "b.c")` and `("a.b", "c")` end up filed in the same place, and a collision here would mean one host silently inheriting another's pin. A fixed `wincommander.hostpin.` prefix also keeps these entries apart from the connection passwords the same keychain already holds, so a pin can never be read as a credential or the reverse.

Two guards run before any keychain call, which is also what makes them testable in a binary that must not touch the user's real keychain:

- An incomplete key — empty provider or host — is refused outright.
- An empty fingerprint is refused rather than stored. Storing one would create an entry that `RemoteHostTrust` must then read as a `Mismatch` forever, locking the user out of a host with no way to tell why.

`LoadPin` deliberately returns whatever was stored, including an empty string, rather than mapping it to "no pin" — RC-2 treats an unusable stored pin as a mismatch on purpose, and reporting `nullopt` here would downgrade it to first-use, undoing that.

The store holds no cache: a cached pin could answer a verification from memory after the stored one changed underneath it.

## RC-5: what a manager row shows

`PresentRemoteConnection` derives §46's row — status, credentials state, link quality, trust, last successful connection, read-only, and a failure count — from the state plus two facts the state does not own (whether a credential is stored, and the host's trust verdict).

Three judgements are worth naming:

- **A rejected credential is its own state, not "stored".** A credential the server refused is worse than none: it will keep failing until replaced, and labelling it merely stored leaves the user nothing to act on. It reads as `Rejected` regardless of whether anything is actually in the keychain.
- **`needs_attention` is not just `status == Blocked`.** A mismatched host pin needs the user *while the connection sits idle and has never failed once* — so trust is folded in here rather than left for every surface to remember separately. `UnknownFirstUse` deliberately does **not** raise it: first use is a question the connect flow asks, not a standing alert on an idle row.
- **`Offline` alone raises nothing.** It is the retryable outcome — "try again later" — and must not compete for attention with a blocked host the user actually has to resolve.

Link quality thresholds are chosen for interactive browsing rather than throughput: past roughly a quarter second, typing a path and waiting for a listing stops feeling direct. A **negative** latency sample reports `Unknown`, not `Good` — it is a broken measurement, and flattering it would be the most misleading answer available.

Like `RemoteConnectionState`, this value carries no credential material. It is what gets copied into the UI, and giving it somewhere to put a password is how one ends up in a log.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the slice adds files to the Xcode project).
- Focused `WinCommanderUT 'nc::core::RemoteConnectionPresentation*' --rng-seed 424242`: **6/6 cases, 31/31 assertions** — every quality band including both boundaries and the negative sample; a rejected credential reported as such with and without one stored; stored versus missing; a mismatched or unusable pin raising attention on an idle, never-failed connection while first-use and trusted do not; `Offline` raising nothing; and the row's carried-through fields.
- Focused `WinCommanderUT 'nc::core::KeychainHostPinStore*' --rng-seed 424242`: **3/3 cases, 16/16 assertions** — no two providers sharing a service name and the `("a","b.c")` / `("a.b","c")` concatenation collision specifically excluded; pins namespaced away from credentials; every incomplete key and the empty fingerprint refused before any keychain call.
- Focused `WinCommanderUT 'nc::core::RemoteHostTrustPolicy*' --rng-seed 424242`: **6/6 cases, 34/34 assertions** — first-use pinning stored normalized and matching later spellings, scoped per provider and host; the routine accept path refusing a mismatch, a repeat, and an unverifiable fingerprint without touching the store; `ReplacePin` accepting a mismatch but not garbage; forgetting; a failing store reported rather than assumed; and a decision taken against the live store rather than a stale verdict.
- Focused `WinCommanderUT 'nc::core::RemoteHostTrust*' --rng-seed 424242`: **7/7 cases, 44/44 assertions** — six spellings of one fingerprint accepted; malformed and odd-length values rejected rather than coerced; unusable-presented handled with and without a pin; first use never an automatic yes; mismatch neither self-resolving nor promptable; an unparseable stored pin treated as mismatch; only a pinned match connecting unprompted.
- Focused `WinCommanderUT 'nc::core::RemoteConnection*' --rng-seed 424242`: **8/8 cases, 69/69 assertions** — the non-retryable set refused even on a fresh budget; the exponential sequence, ceiling clamp, sub-1 multiplier, and zero-attempt policy; a retryable failure exhausting into `Offline` versus a non-retryable one blocking at once; bounded newest-first history; success resetting the budget and restoring the full backoff sequence; read-only reporting and stale-latency clearing; explicit disconnect; a no-op outcome.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **696/696 cases, 11,232/11,232 assertions**.
- No ASAN/UBSAN/TSan run: a pure value model with no allocation ownership, concurrency or `Operations`/`VFS`/`RoutedIO` involvement. The increment that performs real reconnection scheduling is where that budget changes.

### Coverage gaps

- **The keychain round-trip itself is untested.** The tests cover key derivation and the pre-call guards; actually storing and reading a pin would touch the developer's real keychain, which a unit-test binary must not do. That belongs in an integration fixture with its own keychain.
- **Nothing wires this to providers yet.** No VFS provider presents a fingerprint to `RemoteHostTrustPolicy` today, so host verification is built but not yet consulted on connect — the next increment.
- No scheduler, no UI, and no system SMB/NFS mount handling — all later Q2-5 increments.
