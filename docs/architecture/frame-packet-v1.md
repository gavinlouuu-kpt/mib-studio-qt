# Desktop frame packet v1

One Tauri invocation returns metadata and pixels encoded from one owned cxx
`BridgeFrame`. No pointer escapes C++; no native padded structure is serialized.
Rust releases the existing native-command mutex after obtaining the owned frame,
then validates and encodes it. No background worker or lifecycle authority is
introduced. Old split-cache commands fail with `FRAME_PROTOCOL_UPGRADE_REQUIRED`.

## Encoding

All integers are unsigned little-endian. The header is exactly 96 bytes.
The schema is `crates/mib-bridge/contract/bridge-contract.json`; the existing
generator checks TypeScript and Rust constants. Wire protocol v1 is independent
of cxx ABI 11 and scientific Processing Contract v1. No scientific field changes.

| Offset | Width | Meaning |
|---|---|---|
| 0 | 4 | ASCII MIBF |
| 4 | 2 | Version, 1 |
| 6 | 2 | Header length, 96 |
| 8 | 4 | Flags: bit 0 valid; other bits must be zero |
| 12 | 4 | Pull kind: latest 1, indexed 2, review 3, background 4 |
| 16 | 8 | Native frame index (lossless decimal string in TypeScript) |
| 24 | 8 | Legacy raw timestamp_ns value; clock/unit validity unavailable |
| 32, 40 | 8 each | Width, height |
| 48 | 8 | Existing format: PFNC Mono8 0x01080001 or facade-normalized Mono8 0 |
| 56 | 8 | Row stride bytes |
| 64 | 8 | Payload bytes, exactly stride × height |
| 72, 80, 88 | 8 each | Reserved session/config/source identity; zero, decoded as unavailable |
| 96 | payload length | Pixels including row padding |

The pull kind describes the API route, **not** an authoritative camera/file
source identity. Unknown required flags/versions/identities fail closed. A future
accepted backend identity contract requires a negotiated protocol extension;
reserved zeros cannot be interpreted as a real session. No overlay is authorized
by frame index alone. Timestamp values are preserved, never converted to a
latency or freshness estimate while the clock domain is unknown.

Invalid/no-frame responses have flag zero, no payload, and zero numeric fields
except the pull kind. Malformed frames return an error; they are not successful
empty frames. JSON index inputs require canonical decimal strings up to u64::MAX;
frontend helpers reject unsafe JavaScript numbers before invoking Rust.

## Bounds and ownership

| Resource | Policy |
|---|---|
| Packet payload | At most 32 MiB plus 96-byte header |
| Geometry | Each dimension ≤8192; at most 16,777,216 pixels |
| RGBA conversion | At most 64 MiB per drawn frame |
| Scheduler views | At most 4 registered; application currently registers live/review |
| In-flight native pulls through scheduler | 1 aggregate |
| Pending pulls | At most 1 per registered view, at most 4 aggregate; closures only, no images |
| Retained response cache | None in Rust adapter or scheduler |
| Scheduler high-water counters | peakPending, replacements, staleReplies; current views/pending/inFlight |
| Preview / routine metric scheduling | 30 Hz target / at most 5 Hz; no throughput claim |

Validation precedes Rust packet allocation and browser RGBA allocation. The
owned native BridgeFrame has **already** been allocated by the existing facade
and marshaler; bounding that upstream snapshot allocation remains an Agent A
contract/integration dependency. These limits do not establish a bound on total
WebKit/GPU/canvas storage or hostile direct IPC callers. Aggregate native RSS,
IPC buffering and retired canvas memory require native measurement before M4.

View unmount, navigation and superseded Review intents discard obsolete results.
The in-flight native call remains owned until it resolves; no SDK resource is
freed to interrupt it. Local source/config mutations invalidate earlier replies.
This local guard cannot replace authoritative session/config identities for
unsolicited native changes. A wedged native call leaves the lane occupied rather
than creating an unbounded replacement queue. Operation cancellation/timeout
reconciliation belongs to the backend operation API and remains M2 work.

## Evidence and open gates

`frameTransaction.test.ts` first failed against the pre-fix client: live A
metadata plus indexed B pixels. It now covers reversed response completion,
unequal geometry, u64 precision, source mutation, and 10,000 interleaved pulls.
`framePacket.test.ts` rejects malformed headers/lengths/geometry/identity/format.
`framePullScheduler.test.ts` covers 100 view lifetimes, 10,000 pulls, 50 pending
replacements, slow-preview non-starvation and cancellation without native abort.
Vitest's bounded test watchdog applies; races use promise barriers, not sleeps.
Rust encoder tests are added but have not run in the local environment.

This does not finish all of #271/#372 M1: non-frame DTO integers, typed events,
metric validity, authoritative identity/time, operation recovery and snapshots
remain open. M3 layout/drafts, panel geometry, 50 panel toggles (different from
50 queued requests), native Linux/Windows E2E, Qt comparison, HDF5/export and
hardware/sanitizer/resource measurements also remain open.
