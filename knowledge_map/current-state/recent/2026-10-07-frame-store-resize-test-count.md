## 2026-10-07 — frame_store_resize_under_load runs to a resize count

`backend.frame_store_resize_under_load` used to resize for a fixed 1.5 s and required more than 10 resizes. On develop's Windows build (e71e4fe4), `resize()` under live producer and consumer load got through only 10 in that window, so the test failed with no fault in the code. It now runs until 25 resizes (12 s deadline, 20 s watchdog) and requires all of them. This follows the #529 fairness-test fix: assert work done, not speed.
