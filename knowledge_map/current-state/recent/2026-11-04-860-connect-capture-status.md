## 2026-11-04 — The Connect tab says when the camera is capturing (#860)

The Connect tab wrote "Connected to <camera> (not capturing)" once at selection and never again, so
it kept saying so while the camera ran at 50 or 5000 fps. It now follows the `CameraController`
state that [[frontend/MainWindow]] forwards ("(capturing)", "(starting capture…)", "(stopping
capture…)", "(capture failed)", "(not capturing)"), and a late discovery listing no longer replaces
the line with a device count ([[frontend/ConnectTab]]). Bench: "(capturing)" at 5000 fps on the
MC2066. Guard: `frontend.camera_action_state`.
