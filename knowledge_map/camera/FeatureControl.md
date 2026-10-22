# Typed feature control

**Source:** `include/backend/camera/features/`, `src/backend/camera/features/`.
**Related:** [[EGrabberCamera]], [[../services/CaptureService]].

Isolated E02 building block for existing camera ownership, not a second camera
service. It is not wired into the application or bridge yet. The synchronous
port borrows an existing grabber; callers must hold the capture session's
serialization guard for session-token acquisition, validation, SDK access and
readback. They must advance the capability revision when invalidating controls.

Requests carry exact source, uint64 generation and capability revision. Writes
refresh access and numeric bounds, require exact value types, reject commands,
and return requested values separately from authoritative readback. An exception
after submission means unknown outcome and must never trigger automatic retry.
Write-only features return acknowledged-but-unconfirmed, not fabricated values.

The SDK adapter supports System/Interface/Device/Stream/Remote and a single
SDK-inferred selector slot (`Feature[Value]`). It requires existing SDK selector
rollback and range checking; it never changes these module-wide options.
Multi-selector browsing, category enumeration, UI integration, bridge ABI,
engineering commands and physical qualification remain outstanding.

## Validation

See `tools/egrabber_acceptance/README.md`. Core tests use a fake port. Optional
adapter tests use a fake grabber with real SDK query builders; explicit template
instantiation checks the real SDK API. Neither proves physical selector rollback,
actual XML access behavior, Windows linking, or application session serialization.
