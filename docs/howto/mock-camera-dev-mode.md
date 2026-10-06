# Mock Camera Dev Mode

We support running the Qt frontend without camera hardware by swapping the capture pipeline over to a folder-backed mock camera. The mock reuses the same `CaptureService` interface used by the EGrabber implementation, so the frontend and processing services behave exactly as if frames were streaming from the device.

## Quick start: mock configuration

You can enable mock camera mode in two ways:

1. **Via ConnectTab UI**: Use the "Configure Mock Camera" button in the Connect tab to select a folder containing frame images and set the target frame rate.
2. **Via environment variables**: Set environment variables before launching `mib_studio_qt.exe` (see below).

## Enabling the mock via environment variables

Set the following environment variables before launching `mib_studio_qt`:

- `MIB_CAMERA_MODE=mock` &mdash; selects the folder-backed camera. Any other value (or unset) keeps the hardware path.
- `MIB_MOCK_CAMERA_DIR=<absolute-or-relative-path>` &mdash; directory containing the images to stream. Defaults to `<data>/mock_frames`.
- `MIB_MOCK_CAMERA_INTERVAL_MS` (optional) &mdash; delay between frames in milliseconds, default `33` (~30 fps). For finer control use the mock config dialog or set options programmatically (microsecond precision).
- `MIB_MOCK_CAMERA_LOOP` (optional) &mdash; set to `false`, `0`, or `no` to stop after the last frame. Otherwise the sequence loops.

## Getting real stream frames

A 5,000-frame 512x96 grayscale stream is published on the Hugging Face Hub and
declared in [`env/assets.json`](../../env/assets.json) as
`512x96stream-mock-frames`. Materialise as many frames as you need (default
1,000) and point the mock at the folder:

```bash
python3 scripts/provision-assets.py --asset 512x96stream-mock-frames --count 1000
MIB_CAMERA_MODE=mock MIB_MOCK_CAMERA_DIR=build/vendor/assets/datasets/512x96stream-mock-frames ./build/.../mib_studio_qt
```

The two frames under `data/mock_frames/` are only a smoke-test placeholder.

## Image requirements

The mock loads files with `QImageReader`, so any Qt-supported format works: `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tif`, `.tiff`, etc. Each image is converted to PFNC `Mono8` (`0x01080001`) before being forwarded through `CaptureService`, matching the grayscale payload produced by our EGrabber path.

Images are streamed in lexical order by filename. For deterministic playback, use a numeric prefix (e.g. `frame_0001.png`).

## Stats and logging

`MockCamera` preloads all images into memory on start and serves frames from an in-memory cache to maximize throughput. It tracks the measured frame rate and data rate based on delivery timestamps, exposing them through `CaptureService::stats()` like the hardware camera. Logging (via `spdlog`) reports the source folder, microsecond interval, and derived fps during backend initialization, and warns when images are missing or unreadable. There is no per‑frame logging; rely on the periodic capture stats instead.

## High-fps sanity check

To confirm 5000 fps playback:

1. Launch `mib_studio_qt.exe` and use the ConnectTab to configure mock camera with your frame folder and set the frame rate to `5000`, or set `MIB_MOCK_CAMERA_INTERVAL_MS=0.2` (200 microseconds) via environment variables.
2. Start capture and let it run for a few seconds. The backend log prints `interval=200 us (~5000.0 fps)` for the mock camera.
3. Open the capture stats panel (or tail the log) — `CaptureService` should report a frame rate close to 5000 fps once steady.
4. Pause playback; frames remain buffered at the configured rate, so you can scrub without waiting for rendering.

## References

- Hardware path mirrors the Euresys SDK high frame rate samples (e.g. `egrabber-sample-programs/python/310-high-frame-rate.py`).
- Mock implementation lives in `camera/mock/MockCamera` and is injected via `CaptureService::setCameraFactory`.

## Linux cloud/toolchain note (`-lstdc++` linker error)

When configuring in some cloud images, CMake may fail very early with:

`/usr/bin/ld: cannot find -lstdc++`

This usually means `/usr/bin/c++` is pointing to a clang alternative that
resolves an incomplete GCC runtime path in that image.

Quick fix:

```bash
sudo update-alternatives --set c++ /usr/bin/g++
```

Sanity-check link works:

```bash
printf 'int main(){return 0;}' | c++ -x c++ - -o /tmp/cxx-link-test
/tmp/cxx-link-test
```

After that, rerun configure. If configure then fails on package resolution
(e.g. Qt/Conan graph), the `-lstdc++` issue is resolved and you are now blocked
on dependency provisioning rather than compiler runtime linking.


