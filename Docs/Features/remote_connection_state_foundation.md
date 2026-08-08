# Q2-5 RC-1…RC-11: Remote connection state, retry, host trust, pinning, presentation, enforcement, classification, scheduling, the loop and the first real caller

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

### Coverage gaps at RC-5

- **The keychain round-trip itself is untested.** The tests cover key derivation and the pre-call guards; actually storing and reading a pin would touch the developer's real keychain, which a unit-test binary must not do. That belongs in an integration fixture with its own keychain.
- **Nothing wires this to providers yet** — closed by RC-6 and RC-7 below.
- No scheduler, no UI, and no system SMB/NFS mount handling — all later Q2-5 increments.

---

# RC-6: the gate, in VFS

Until now SFTP completed its handshake and went straight on to authentication. Whatever key the server presented was accepted — including a key that had changed since last time. Everything RC-2…RC-5 built was correct and consulted by nobody.

## Where the check goes, and why exactly there

Immediately after the handshake and **before every authentication path**. A password, or a passphrase-unlocked private key, sent to an unverified server is already disclosed; refusing afterwards would tell the user about an interception that had already succeeded. The single call sits above both the public-key branch and the keyboard-interactive/password branch, so no future third path can be added that skips it by accident.

## VFS asks; it does not decide

`HostKeyVerifier` is an interface, and pinning, keychain storage and prompting all live above it. That keeps `Source/VFS` free of any dependency on the application, keeps this module testable without a keychain or a user, and means the layer that *can* ask a human is the one that does.

The policy is process-wide rather than per-host. That is not a shortcut: host-key policy genuinely is a property of the process — one `known_hosts` covers every ssh session a user starts — and a host revived from a serialized configuration arrives through a factory with no call site that could hand it one.

## No policy means no connection

An absent policy is a refusal, not a fallback to connecting unverified. Nobody has decided what this process trusts, and proceeding anyway is precisely the silent accept host verification exists to prevent. It also makes a mis-wired build fail loudly at the first connection rather than quietly stop verifying.

Two smaller decisions follow the same rule: a fingerprint the session cannot produce is `host_key_unavailable` rather than an empty fingerprint offered for comparison, and an algorithm libssh2 does not name is reported as unknown rather than guessed — that string is shown to a user making a trust decision, and an invented one is worse than none.

Neither refusal is classified as `Unavailable` or `TimedOut`. That is what keeps them out of RC-1's retryable set: a host-key failure that retried until it succeeded would defeat the check entirely.

The fingerprint is formatted as lowercase hex with no separators — the exact form RC-2's comparison normalizes to, so a fingerprint makes the round trip through storage and back without a normalization step that could differ between the writer and the reader.

# RC-7: the policy, in the application

`SFTPHostKeyPolicy` implements the interface on top of `RemoteHostTrustPolicy` and the keychain store, and it is installed beside `RegisterAvailableVFS` — before anything can spawn an SFTP host.

- A pinned host connects with no interruption.
- An unknown host asks the user once, showing host, port, algorithm and fingerprint, and is pinned on acceptance.
- A **mismatched** host is refused without a prompt. This is where RC-2's `MayPromptToTrust` earns its keep: offering accept/decline for a key that changed would train users to click through the one warning that must never become routine.
- An unverifiable fingerprint is refused without a prompt: nothing was compared, so there is no question worth asking.
- An accepted host whose pin **could not be made durable** is reported as a refusal. Connecting anyway would ask again next launch with no way for the user to tell an unremembered host from a changed one.

**A pin is filed under `[host]:port`.** The port is part of the identity: two services on one machine can legitimately present different keys, and one silently inheriting the other's pin would either accept a host nobody verified or warn about one that never changed. The brackets are OpenSSH's spelling and they are what makes it unambiguous for IPv6 literals, which are full of colons of their own — without them `("a", 22)` and `("a:22", 22)` would be filed in the same place.

The prompt is injected rather than called directly, so the refusals — which are the security-relevant part — are testable without a user, and so no test can block on a modal window. The concrete prompt follows the threading contract this connect path already relies on for passwords: on a background queue it hops to the main thread and waits.

## Verification

- `WinCommanderUT`, `VFSUT`, `VFSIT`, `WinCommanderIT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**.
- Focused `VFSUT 'nc::vfs::sftp::HostKeyVerification*'`: **6/6 cases, 22 assertions** — the hex spelling and its lowercase guarantee; every byte kept, including zeros, which is what stops two keys sharing a fingerprint; known algorithms named and unknown ones left empty; no policy until one is installed; a policy replaced mid-flight staying alive and usable for the handshake already holding it.
- Focused `WinCommanderUT 'nc::core::SFTPHostKeyPolicy*'`: **8/8 cases, 36 assertions** — the `[host]:port` naming including IPv6 and the `("a",22)`/`("a:22",22)` collision; asked once and never again, across spellings; declining refusing and recording nothing; a changed key refused without a prompt and the old pin left intact; an unusable fingerprint refused without a prompt; two ports treated as two hosts; an accepted host that could not be stored reported as a refusal; and no prompt available meaning refusal for an unknown host but not for a pinned one.
- Full `WinCommanderUT --rng-seed 424242`: **751/751 cases, 11,475 assertions**. Full `VFSUT`: **191/191 cases** (its assertion total varies run to run — some cases iterate over live filesystem content).
- No ASAN/UBSAN/TSan run: the change adds no allocation ownership or concurrency beyond a mutex-guarded pointer swap, whose contract is covered by the replacement test above.

### Coverage gaps at RC-7

- **The end-to-end refusal is not covered by an automated test.** Proving that a real handshake stops before authentication needs a server; the `VFSIT` SFTP suite runs against a Docker container and now installs an explicit accept-all policy, which is where a "reject and assert no credential was sent" case belongs.
- **SFTP only.** FTP has no host key to verify, and WebDAV's identity is a TLS certificate — a different mechanism that needs its own slice.
- The keychain round-trip, the reconnect scheduler, the manager UI and system SMB/NFS mounts remain as listed above.

---

# RC-8: giving the state machine something to classify

RC-1 decided which failures may be retried. Nothing produced its vocabulary from a real error, so none of those decisions could ever be reached — the retry policy was correct and unreachable, in the same way host trust was before RC-6.

`ClassifyRemoteFailure` is that producer: a VFS `Error` in, a `RemoteConnectionFailure` out.

## The loop it closes

An SFTP host key refused by RC-7 raises `host_verification_failed`. That arrives here and becomes `HostVerificationFailed`, which RC-1 refuses to retry. Without this hop, a reconnect timer would go on reaching a server whose identity failed to check out — which is precisely what verification exists to stop. The same holds for `host_key_unavailable`: nothing was verified, so it is a verification failure, not a transport one.

## Unrecognised means unretryable

Anything this cannot name becomes `ProtocolError`, which is not in the retryable set. Retrying is the privilege of failures we can name; an unrecognised error put on a timer is how a connection ends up hammering a server for reasons nobody understands. That covers an unknown error domain, an unknown code inside a known domain, and a zero code — callers classify only what already failed, so a zero arriving here is a caller bug, and it must land somewhere that will not be retried rather than on `None`.

## Two distinctions worth keeping

- **A rejected credential is not a permission refusal.** Both mean "no", but one sends the user to replace a credential and the other tells them the account may not do this. Collapsing them would send someone to change a password that is working perfectly.
- **A timeout is not unreachability**, even though both are retryable. They differ in what the backoff is waiting for, and RC-1's history shows the user which one they have.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::ClassifyRemoteFailure*'`: **6/6 cases, 38 assertions** — both host-key errors reaching `HostVerificationFailed` *and* asserted non-retryable in the same breath; all four credential failures plus POSIX `EAUTH` non-retryable; the timeout and unreachable sets, both asserted retryable; permission kept apart from credentials; an unknown domain, an unknown code and a zero code all landing on something that will not be retried.
- Full `WinCommanderUT --rng-seed 424242`: **751/751 cases, 11,479 assertions**.
- No sanitizer run: a pure switch over an error code, with no allocation, ownership or concurrency.

### Coverage gap at RC-8

**Nothing calls it yet** — the connect paths still return a raw `Error`. RC-9 below builds the half that decides *when*; the wiring that feeds real errors in remains open.

---

# RC-9: when the retry actually happens

RC-1 folds one outcome into one connection and says how long to wait. RC-9 owns the collection and the clock-facing half: turning that duration into a deadline that survives until it comes due.

## The deadline is absolute, and computed once

A delay recomputed on every tick would restart the wait each time anybody asked, and nothing would ever come due — the connection would sit "about to retry in 500ms" forever. The registry stores `now + retry_after` at the moment the failure is folded, and every later question reads it rather than deriving it.

## Retries are claimed, not observed

`ClaimDueRetries` hands out the due connections **and disarms them in the same locked step**. Two ticks that both merely *saw* the same due connection would each start an attempt, and the second would race the first — against a server that is, by definition, already having trouble.

That is also why the type is internally synchronized rather than documented as single-threaded. Connection attempts run on background queues while the manager reads the same rows to draw them, and a claim is only meaningful if it is atomic. A test drives eight threads at sixty-four due connections and asserts the total handed out is exactly sixty-four — not fewer, not more.

## What disarms a pending retry

- **A success.** Leaving it armed would start an attempt against a link that is already up.
- **An explicit disconnect.** The user asked this one to stop; reconnecting on a timer they never set would undo it.
- **A non-retryable failure**, which never arms one in the first place. A refused host key put on a timer would defeat verification entirely — the same rule RC-1 states, now with something that actually schedules.
- **A spent budget**, which settles into `Offline` — try again later — rather than `Blocked`, which is what a user has to resolve themselves.

Connections are keyed by whatever identity the caller already uses. Inventing a second scheme here would be a way for one connection to be filed under two names and retried twice.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::RemoteConnectionRegistry*'`: **8/8 cases, 51 assertions** — a deadline that does not move however often it is asked; a due connection handed out exactly once; nothing armed for a refused host key or a rejected credential; the budget spent across a full backoff sequence and settling on `Offline`; success and disconnect both disarming; the earliest deadline reported and consumed in order; connections kept apart, one forgotten without disturbing the other, and an unknown key answered rather than created.
- The same focused suite again under **TSan** (see below), covering the eight-thread claim.
- Full `WinCommanderUT --rng-seed 424242`: **759/759 cases, 11,530 assertions**.

### A pre-existing blocker found on the way

`WinCommanderUT` **cannot be built under TSan as it stands**: `PanelPresentationGeometry_UT.mm:1696` has a test whose stack frame is 33,536 bytes against a 32,768 limit, and TSan's instrumentation is what pushes it over. It is a `-Werror` error, so the whole scheme fails to build — nothing to do with this slice, but it means the suite's concurrency tests were unreachable by the sanitizer that exists to check them.

The TSan run above was obtained by relaxing that one warning for the build only (`-Wno-frame-larger-than`), which is a workaround rather than a fix: it silences the check for every file. Fixing the oversized test is spun off separately; the limit is doing its job and should not be raised.

### Coverage gap at RC-9

**Still nothing drives it** — closed by RC-10 below.

---

# RC-10: the loop that asks the three decisions in order

Three decisions existed with no caller. `IsRetryableRemoteFailure` says *whether*, `ClassifyRemoteFailure` says *what a failure was*, `RemoteConnectionRegistry` says *when*. `RemoteReconnectDriver` is the piece that asks them in order: claim what is due, attempt it, classify the outcome, fold it back.

## It owns no timer

Arming one is the caller's business, and the caller needs only `next_deadline` from the pass report to do it. Keeping the timer out means this type is tested against a supplied instant rather than against real elapsed time — the difference between a suite that runs in milliseconds and one that sleeps.

## A connection is never attempted twice in one pass

The due list is claimed up front, and the pass works only from that list. Without the rule, a failure whose fresh backoff lands in the past — a zero-backoff policy, or simply a slow pass — would be claimed again inside the same call, and the loop would spin against a server that is plainly not answering. A test uses exactly that configuration and asserts the connector was asked once.

## A connector that threw told us nothing about the server

It is recorded as a protocol error, which is not retryable. A broken caller is not evidence that the server will answer next time, and putting it on a timer would spin on our own bug. The same reasoning covers a driver constructed with no connector at all.

## What the failure detail may contain

The recorded detail is the provider's own description of the error and nothing composed on top. It is the one field on a connection that a careless caller could use to leak a credential, which `RemoteConnectionState` calls out and this is the first code that has to answer.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::RemoteReconnectDriver*'`: **6/6 cases, 43 assertions** — the three decisions asked in order with nothing attempted before its deadline; the same connection never attempted twice in one pass under a zero backoff; **a refused host key carried from the connector through classification to a retry that is never armed and never picked up again**, which is the end of the chain built across RC-6…RC-9; several due connections in one pass with a blocked one not attempted at all and the next deadline armed from the failure; a throwing connector kept out of the retryable set; and a success established by hand disarming a pending retry.
- Full `WinCommanderUT --rng-seed 424242`: **765/765 cases, 11,573 assertions**.

### Coverage gap at RC-10

**No production caller yet** — the first one arrives in RC-11 below.

---

# RC-11: the first real outcomes

Everything from RC-1 to RC-10 reasoned about outcomes nobody produced. `ConfigBackedNetworkConnectionsManager::SpawnHostFromConnection` is where a connection actually succeeds or fails, and it now records what happened, keyed by the connection's own UUID.

## Nothing about the existing behaviour changes

`RecordConnectionAttempt` returns whatever the spawn returned and re-raises whatever it raised, after recording it. Every caller of `SpawnHostFromConnection` handles success and failure exactly as it did — the recording is invisible to them, which is what makes this safe to put on a path that several surfaces already depend on.

## A null host is not a failure

That is what a cancelled password prompt returns, and nothing was ever asked of the server. Recording it as a failure would spend a retry budget and eventually mark a connection blocked because somebody pressed Cancel. It is the case that makes this a function with a name rather than two lines at the call site.

## An exception that is not an `Error` is recorded as unretryable

Something we cannot name is not evidence that trying again will help. It is rethrown all the same.

## The identity is the connection's own UUID

Not a composed one. A second identity scheme is how one connection ends up filed under two names — and RC-9 already refused to invent one for exactly that reason.

## Verification

- `WinCommanderUT`, `WinCommanderIT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::RecordConnectionAttempt*'`: **8/8 cases, 31 assertions** — a host recorded and handed straight back; read-only carried through from the host; a cancelled prompt recording nothing at all; a raised error recorded *and rethrown unchanged*, with a deadline armed for the retryable case; **a refused host key arriving as `Blocked` with nothing armed**, which is the handshake-to-policy chain end to end; an unnameable exception kept out of the retryable set; a success clearing what an earlier failure left behind; and nothing recorded when there is nothing to run.
- Full `WinCommanderUT --rng-seed 424242`: **812/812 cases, 11,796 assertions**.

### Coverage gap

**Nothing reads the states yet, and nothing drives the retries.** The registry is exposed on the manager for a surface that does not exist, and `RemoteReconnectDriver` still has no timer arming it — a reconnect also needs an owner for the host it produces, since a panel holds its own. That, the manager UI, and system SMB/NFS mounts are what remain of Q2-5.
