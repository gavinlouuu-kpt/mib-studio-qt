# Safe start/stop for EGrabber and resolving -1012

This guide documents a safe pattern to start/stop Euresys EGrabber and how to treat GenTL `-1012 (ABORT)` during shutdown.

## Why `-1012 (ABORT)` happens

- `-1012` is raised when a blocking `pop()/processEvent()` is aborted. This is expected during shutdown if a thread is waiting in `pop()` while `stop()` is called.
- Samples treat ABORT as normal during stop and explicitly cancel blocking calls to wake the waiter.
- Never wait in `pop()` with the default `GENTL_INFINITE` timeout while holding a lock that `stop()` also needs: with no frames arriving the wait never ends, `stop()` waits for the lock forever, and a UI thread that called it hangs (#840, `EGrabberCamera`). Pass a finite timeout (`ScopedBuffer(grabber, timeoutMs)`; `GC_ERR_TIMEOUT` means "no frame yet", not an error) and call `cancelPop()` before taking the lock.

## Recommended patterns from SDK samples

- Wake blocked `pop()` during stop:

```104:116:egrabber-sample-programs/cpp/display-latest-buffer/displayLatestBuffer.cpp
            try
            {
                grabber.stop();
                grabber.cancelPop();
            }
            catch (...)
            {
            }
            WaitForSingleObject(refreshThread, INFINITE);
            CloseHandle(refreshThread);
            refreshThread = NULL;
```

- Ignore ABORT on shutdown in pop loop:

```372:377:egrabber-sample-programs/cpp/display-latest-buffer/displayLatestBuffer.cpp
        catch (const gentl_error &e)
        {
            if (e.gc_err != gc::GC_ERR_ABORT)
            {
                MessageBoxA(NULL, e.what(), NULL, MB_OK);
            }
        }
```

- Events mode: stop event thread first, then `grabber.stop()`, then disable events:

```56:63:egrabber-sample-programs/python/300-events-mt.py
# stop the events monitoring thread
stop_event.set()
process_events_thread.join()

grabber.stop()

grabber.stream.set('EventNotificationAll', 0)
grabber.disable_event(DataStreamData)
```

- Start/metrics baseline:

```21:28:egrabber-sample-programs/cpp/egrabber-snippets/samples/310-high-frame-rate.cpp
    grabber.reallocBuffers(20);
    grabber.start();

    uint64_t tStart = Tools::getTimestamp();
    uint64_t tStop = tStart + static_cast<uint64_t>(10e6);
    uint64_t tShowStats = tStart + static_cast<uint64_t>(1e6);
    for (uint64_t t = tStart; t < tStop; t = Tools::getTimestamp()) { // grab for 10 seconds
        ScopedBuffer buffer(grabber);
```

## Safe start sequence

1. Probe resolution with `BufferPartCount = 1` then read `Width`/`Height` (optional but useful).
2. Set desired `BufferPartCount` and `reallocBuffers(N)`.
3. Call `start()`.

## Safe stop sequence (single-threaded grab loop)

1. Set running flag false and stop producing new work.
2. Call `grabber.stop()`.
3. Call `grabber.cancelPop()` to wake any thread blocked in `pop()`.
4. Release resources: `reallocBuffers(0)`, then destroy the grabber/GenTL objects.
5. Catch `gentl_error` and ignore `gc::GC_ERR_ABORT` (log at debug); warn for other errors.

Example (structure only):

```cpp
try { grabber.stop(); }
catch (const gentl_error& e) {
  if (e.gc_err != gc::GC_ERR_ABORT) warn(e);
  else debug("expected abort on stop");
}
try { grabber.cancelPop(); } catch (...) {}
try { grabber.reallocBuffers(0); } catch (...) {}
```

## Handling ABORT in the grab loop

Wrap `ScopedBuffer buffer(grabber);` in `try/catch` and treat `GC_ERR_ABORT` as non-fatal when stopping:

```cpp
try {
  ScopedBuffer buffer(grabber);
  // consume parts...
} catch (const gentl_error& e) {
  if (e.gc_err == gc::GC_ERR_ABORT) {
    // normal during stop
    return;
  }
  throw;
}
```

## Final stats on shutdown

If end-of-run metrics are needed, read `StreamModule` statistics just before calling `stop()` (or immediately after your grab loop has ended but while the stream is still valid).

## Notes

- Keep logging via `spdlog` (no `std::cout` in application code).
- Reuse the sample patterns; avoid re-inventing shutdown orchestration.


