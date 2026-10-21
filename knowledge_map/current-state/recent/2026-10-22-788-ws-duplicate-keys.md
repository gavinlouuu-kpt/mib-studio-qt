## 2026-10-22 — WebSocket requests with duplicate JSON keys are refused (#788)

Codex round 1 on #786 found that the remote server parsed each `/ws` request into a
`serde_json::Value`, which keeps the last of two equal keys: `{"paths":[a],"paths":[b]}` reached
`run_quick_look` as `[b]`, and the backend's strict parsers, which refuse duplicates, only saw the
collapsed re-serialisation. This held for every dispatched command.

`mib-bridge-server` now parses the frame once from the raw text (`src/request.rs`): a hand-written
`Deserialize` for the request and a visitor that builds the same `Value` as before, failing on the
first key repeated in any object (the frame's own fields and unknown fields, `args`, nested objects,
objects inside arrays; a key equal only after unescaping counts). The refusal is the malformed-JSON
one, ``BAD_REQUEST: duplicate key `paths` at line 1 column n``, before `Server::run`, so nothing runs
and a viewer gets the same answer as the controller. A refused frame's reply now carries its
`request_id` when the frame names exactly one (a second lenient parse on the error path only), so
the waiting call settles; `wsTransport` drops replies without an id. A repeated `request_id`, or
text that is not JSON, keeps the old `{"error"}` shape. JSON documents inside string arguments
(`startRequestJson`, `apply_processing_config_json`) remain the backend's to parse. One behaviour
narrowed: a positional frame (`[1,"cmd",{}]`), which the derived struct accepted, is refused.

Cost: still one pass over the text; the extra work is a key lookup per object member. On a 1 MB
request (`request.rs` `one_megabyte_request_timing`, ignored by default, release build, two runs) the strict
parse took 14.4 to 14.7 ms against 13.9 to 15.2 ms for the plain serde parse it replaces (18 000
small objects, 1.01 MB), and 0.13 to 0.23 ms against the same for one 1 MiB string: within run-to-run
noise.

Tauri IPC: not applicable. Tauri 2 parses the IPC body with `serde_json::from_slice::<Value>`
(`tauri::ipc::protocol`) before any command sees it, so the raw text never reaches our code, and the
webview builds every body with `JSON.stringify` from a JS object, which cannot hold a repeated key.

No ABI change. Tests: `request.rs` unit tests (top level, nested, arrays of objects, unescaped
keys, no duplicates parsing to the very `Value` serde builds, the old malformed cases);
`ws.rs` `duplicate_keys_are_refused_before_dispatch` (a read, `configure_mock` and
`set_preferences`, `review_export_csv` and `fetch_review_metrics_page`, `run_quick_look`,
`take_control`, from the controller and a viewer; nothing ran; the same requests without duplicates
pass). See [[architecture/Desktop-Shell]] ("Remote server").

`cargo clippy --all-targets -- -D warnings` on `mib-bridge-server` (no CI lane runs it) now passes:
five lints on current stable in untouched code were fixed (`io::Error::other`, a `while let`,
`!is_empty`, a `type` alias, and an allowed `result_large_err` where the error is the response).
