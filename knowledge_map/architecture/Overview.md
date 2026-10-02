# Architecture — Overview

> C++17 / Qt6 Widgets + Charts application for real-time microscopy image
> capture, processing, and analysis.

## Layers (top → bottom)

```
┌──────────────── Frontend (Qt Widgets, src/frontend/) ───────────────┐
│  MainWindow → tabs (Connect, Preview, Config, Monitoring, Review,  │
│                     Nanopositioner, SyringePump, Overview)          │
│  Controllers: CameraController, ExperimentController                │
└────────────────────────────┬────────────────────────────────────────┘
                             │ service getters
┌────────────────────────────▼────────────────────────────────────────┐
│                 AppBackend  (src/backend/AppBackend.cpp)            │
│                 composition root — owns every service + FrameStore   │
└────────────────────────────┬────────────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────────────┐
│                       Services (src/backend/services/)              │
│  Capture · Processing · Hdf5 · Playback · Autofocus · CameraControl │
│  Trigger · SyringePump · Yolo · Recorder · Sqlite                   │
├───────────────────────────────────────────────────────────────────────┤
│  mib_processing (Qt-free static lib, linked by mib_backend):        │
│    Processing · selected kernel/ABI loader · Hdf5 · FrameStore      │
│  — the portable core; see [[../services/ProcessingService]]         │
└────────────────────────────┬────────────────────────────────────────┘
                             │ grabFrame / config
┌────────────────────────────▼────────────────────────────────────────┐
│         Camera abstraction (src/camera/) — ICamera interface        │
│            EGrabberCamera (hardware) · MockCamera (dev)             │
└─────────────────────────────────────────────────────────────────────┘
```

## Core runtime objects

- **`AppBackend`** — see [[AppBackend]].
- **`FrameStore`** — shared ring buffer (capacity **5000**), shared between
  capture, processing, and playback. See [[../data-model/FrameStore]].

## Third-party stack

See [[../build-and-run/Dependencies]]. Key libs: Qt6, OpenCV, HDF5, spdlog,
ONNX Runtime, nlohmann_json, Euresys EGrabber SDK (hardware camera), Qt
SerialPort/standard Linux CH341 support for OEABT nanopositioners, and the
optional CoreMOR DLL on Windows.

`mib_processing` (the processing/HDF5 core above) depends only on OpenCV,
HDF5, spdlog, and the standard library — no Qt. This is the portable
boundary a non-Qt consumer (e.g. Biowork's `services/mib-processing`) can
build and link standalone; CI (`backend-ci.yml`) fails if a Qt symbol leaks
into it. See `docs/gold_standard_metrics.md` ("Portable Processing
Contract").

`bindings/python/` wraps `mib_processing` in a pybind11 extension module
(package `mib_processing`, importable as `import mib_processing`), so a
Python consumer gets the exact same algorithm without linking C++ directly.
See [[../build-and-run/Build]] ("Python bindings").

On Windows, the desktop can select an Authenticode-approved
`mib_processing_core-<version>-windows_x86_64.dll` at a between-operation
boundary. [[../frontend/ProcessingCoreDialog]] resolves the channel registry,
prepares a content-addressed cache entry, and hands a verified module to
`ProcessingService`; the bundled kernel remains the startup fallback.

## Reading order

See [[../Agent-Onboarding]].
