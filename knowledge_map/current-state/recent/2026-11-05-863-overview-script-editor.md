## 2026-11-05 — Overview camera-script editor and the remaining Overview details in the desktop (#863 A2)

The Tauri Overview now has Qt's `overviewConfig.js` editor: a text area with the path and the "Unsaved
changes" label, Reset (the default file is made from the bundled `resources/defaults/overviewConfig.js`
when it is missing), Save ("Saved." as an inline status, not Qt's information box), Apply to Camera (saves,
stops a running camera, applies with the Overview role, "Applied to camera. Capture remains stopped."),
Browse... (the external path is kept in `localStorage` `mib.overview.script-path`) and Clear (Qt's Yes/No
question). It is hidden for a MindVision camera and replaces the Overview's old script controls, which
applied with the Experiment role. The stage change applies the same file. New `camera_document` kind
`overview`. Also: the "ROI Overlay: On/Off" toggle (off until a camera is connected; off = no box, no
grab), the MindVision box and drag direction mirrored for a profile that flips the image, MindVision W/H
saved on every change with the old value restored and "ROI was not saved: <reason>" kept on a refusal,
Qt's MindVision mode label (measured capture rate from the new geometry member `capture_fps`, the 400 Hz
trigger marker, the profile error), and the Experiment header's "ROI: W × H @ (x, y)" following the
Overview rectangle. No ABI change: `fetch_camera_geometry` gained `flip_horizontal`, `flip_vertical` and
`capture_fps`. Decision: the Overview rectangle does not set the processing ROI as Qt does. Qt passes
the sensor offsets for an eGrabber camera, and the Experiment frame is already cropped to that window, so
`ProcessingService` (`computeProcessedFrame`, clamping the ROI into the frame) would reduce it to a sliver
(read from the code, not tried on the camera); the processing ROI stays what the configuration and the
host ROI fields set. See
[[architecture/Desktop-Shell]], [[architecture/Rust-Bridge]] and [[architecture/AppBackend]].
