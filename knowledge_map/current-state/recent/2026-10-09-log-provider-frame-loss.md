## 2026-10-09 — The log names the frames behind an incompleteLoss run

A 20 s experiment on the board (bench scene, 100,100 frames) ended `incompleteLoss` with one malformed frame and one 55-frame
gap, and nothing said which frames: the accounting keeps counts, and the log had no line. `ProcessingService::ingestProviderFrame`
now logs a warning for a gap in the PL frame ids ("N frame(s) missing between frame A and frame B") and for a malformed frame
("PL frame N is malformed (ingress error)"), the first 32 per run, so the board owner can line them up with the receiver's heal
counters and the 10 ms sampler. No change to the accounting or the files.
