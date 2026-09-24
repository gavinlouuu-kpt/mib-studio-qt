# MinidumpUploader

> Posts pending crash dumps straight to Sentry's minidump endpoint and
> reports the HTTP status per dump, so [[CrashReporter]] can advance its
> on-disk crash queue on delivery evidence instead of on a hand-off
> assumption.

**Source:** `src/backend/services/MinidumpUploader.cpp`,
`include/backend/services/MinidumpUploader.h`
**Tests:** `tests/backend/crash_reporter_pending_upload_test.cpp`
(`backend.crash_reporter_pending_upload`: DSN parsing, multipart shape, a
local fake endpoint answering 200 / 400 / 503, an unreachable port, the
per-launch cap, legacy recovery, retention)
**Related:** [[CrashReporter]], [[../diagnostics/CrashStateMirror]]

## Why it exists

sentry-native 0.7.20's transport is a black box from the caller's side:
`sentry_capture_minidump` returns `void`, a failed send is freed without
retry, the queue lives in memory, `sentry_close()` flushes for 2 s (library
default) and writes the remainder into the database `.run` dir, and the
next launch re-sends an old run exactly once (`sentry__process_old_runs`
deletes the files before the send). With short sessions and a 16 MB
backlog on a slow uplink the app never delivered a single dump for a month
(`docs/evidence/2026-09-08-crash-dump-review.md` §2). Doing the upload
ourselves gives a status code per dump.

## API

```cpp
namespace backend::services::crash_upload {
Dsn parseDsn(const std::string&);                 // scheme://key[:secret]@host[:port][/prefix]/project
std::string minidumpUrl(const Dsn&);              // <base>/api/<project>/minidump/?sentry_key=<key>
MultipartBody buildMultipart(dumpBytes, UploadFields, boundary);
UploadResult postMinidump(const Dsn&, dumpPath, UploadFields, timeoutMs);
UploadResult postMultipart(const Dsn&, url, MultipartBody, timeoutMs);
bool httpClientAvailable();                       // WinHTTP or libcurl in this build
}
```

`UploadResult::delivered()` is HTTP 2xx; `rejected()` is a 4xx other than
408/429 (the server will never take the dump: too large, malformed, wrong
project). Everything else — no response, timeout, 5xx, 429 — is "retry
next launch".

## Request shape

`multipart/form-data` to `/api/<project>/minidump/?sentry_key=<key>`
(the same endpoint crashpad_handler uses):

| field | content |
|---|---|
| `sentry` | JSON: `release`, `environment`, tags `crash_recovery=pending_dump`, `original_dump_file`, `exe_build_id`; extra `crash_message` (terminate-path `what()`) |
| `upload_file_minidump` | the `.dmp` bytes, `application/octet-stream` |
| `state_snapshot` | the `.json` [[../diagnostics/CrashStateMirror]] sidecar as attachment `state_snapshot.json` |

## Transport

- Windows: WinHTTP (`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`, falling back
  to the default proxy), 10 s resolve/connect, `timeoutMs` send/receive.
  `mib_backend` links `winhttp`.
- POSIX: libcurl when CMake found it (`MIB_HAVE_CURL=1`; the Linux
  backend-only preset has it because sentry-native's curl transport needs
  it). Without libcurl `httpClientAvailable()` is false and dumps stay
  pending.

## Gotchas

- The uploader never touches sentry-native state; it can run before, after
  or without `sentry_init`. Only a parseable DSN is required.
- A dump is uploaded at most once per launch; the whole exchange is bounded
  by `uploadTimeoutMs` (60 s default). Do not raise the per-launch cap
  without thinking about the close path: `CrashReporter::shutdown()` waits
  for the dump in flight.
- The minidump endpoint creates a new event per request. If the process
  dies between a 2xx and the rename, the next launch re-sends that dump
  (one duplicate on the server); acceptable, the alternative is loss.
