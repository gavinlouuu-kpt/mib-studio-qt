## 2026-10-08 — The armv7 server build defines the external-fmt spdlog flags

`scripts/yofo/cargo-armv7.sh build` for `mib-bridge-server` failed in `review_shim.cpp` with
`spdlog/fmt/bundled/core.h: No such file or directory`: the SDK's spdlog is built against the
external fmt, and the cargo build did not pass the defines the CMake targets use. The script now
adds `SPDLOG_COMPILED_LIB`, `SPDLOG_SHARED_LIB`, `SPDLOG_FMT_EXTERNAL` and `FMT_SHARED` to the
target C++ flags. See [[build-and-run/Build]].
