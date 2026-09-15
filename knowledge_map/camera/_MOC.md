# Camera — MOC

> All camera code lives under `src/backend/camera/` and
> `include/backend/camera/`.
> [[../services/CaptureService]] owns the acquisition thread; the camera
> abstraction just delivers frames.

- [[ICamera]] — abstract base + `CameraConfig`, `CameraStats`, `Frame`
- [[EGrabberCamera]] — hardware via Euresys EGrabber SDK
- [[MindVisionCamera]] — hardware via MindVision SDK
- [[MockCamera]] — folder-backed dev camera

**Up**: [[../README|Vault home]] · **See also**: [[../services/CaptureService]]
