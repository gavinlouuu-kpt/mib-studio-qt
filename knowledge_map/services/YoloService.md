# YoloService

> Owns an optional ONNX Runtime session for YOLO segmentation. Loaded from
> `resources/models/yolo11n-seg.onnx` during `AppBackend::initialize`.

**Source:** `src/backend/services/YoloService.cpp`,
`src/backend/services/YoloService.stub.cpp`,
`include/backend/services/YoloService.h`

## Responsibility

- `initialize(modelPath)` — load the model; returns false on failure (app
  continues without YOLO features).
- `isLoaded()` — whether the model is usable.
- `getSession()` / `getEnv()` — hand out the raw `Ort::Session*` /
  `Ort::Env*` for callers that want to run inference.

## Threading

Stateless after load; callers must serialise their own inference runs.

## Gotchas

- `ProcessingService` does not use YOLO today; this is a placeholder for
  future segmentation.
- Model path resolution is relative to the executable directory (see
  `AppBackend::initialize` wiring).
- If the model is missing, `AppBackend` logs a warning via spdlog and
  continues.
- CMake now treats ONNX Runtime as optional (`find_package(onnxruntime CONFIG QUIET)`):
  - with `onnxruntime::onnxruntime`, real implementation (`YoloService.cpp`) is built
  - without it, a stub (`YoloService.stub.cpp`) is built and `initialize()`
    returns `false` after one warning log

## Model asset (2026-09-21)

`yolo11n-seg.onnx` is no longer a tracked file. It is the asset `yolo11n-seg`
in `env/assets.json` (Hub repo `gavinlouuu/mib-yolo11n-seg`, pinned commit +
SHA-256), provisioned by `python3 scripts/provision-assets.py --asset
yolo11n-seg` into `build/vendor/assets/models/yolo11n-seg/`. CMake resolves
that path as `MIB_YOLO_MODEL_PATH`, fails configure with the fix command when
ONNX Runtime is found but the file is missing, and the Windows post-build step
copies it to the unchanged runtime location `<exe>/resources/models/`. See
[[../build-and-run/Assets]].
