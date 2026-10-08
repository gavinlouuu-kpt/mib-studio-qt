## 2026-10-08 — Raw Record is refused and hidden on a PL-science instrument (#651 G9)

On the PZ7035 Raw Record could only run in Align, where the producer delivers about 60 preview frames per second;
it saved those, not the 5 kHz PL results, to the RAM root with no free-space guard. `AppBackend::startFrameRecording`
now refuses on a PL-science instrument with a message that says so and points at experiments (and #649's ring save),
and the browser hides the Record button when the instrument has no host frame buffer (`caps.frame_buffer` false).
Desktop behaviour is unchanged. See [[Desktop-Shell]].
