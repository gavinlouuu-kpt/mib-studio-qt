## 2026-10-19 — Serial I/O to the pumps can no longer block without limit (#720)

A stalled USB-serial adapter could hang a pump command for ever, holding the bridge lock, so every
other bridge command (and the client-loss stop-and-save, #711) waited behind it. Three calls had no
deadline; each is now bounded:

- **POSIX `waitForBytesWritten`** ignored its timeout and called `tcdrain()`. It now polls
  `ioctl(TIOCOUTQ)` to the timeout (1 ms steps, as Win32 polls `COMSTAT.cbOutQue`).
- **POSIX `write`** retried `EAGAIN` for ever. It now waits for room with `poll(POLLOUT)` up to
  `kSerialWriteTimeoutMs` (1 s, also the Win32 COM write timeout).
- **`ModbusBusSession::transact`** waited for its I/O thread with no deadline. It now gives up at
  `transactBudgetMs(timeoutMs)` (the transaction's own bounded steps + 500 ms; 3.51 s for the pumps)
  and returns `Timeout`. Requests carry sequence numbers: a late result of an abandoned request is
  dropped, a request not yet taken is withdrawn (never sent), and while the I/O thread is still
  stuck further calls fail fast. Once it returns, the next transaction drains late reply bytes
  before writing, as for any stale traffic.

Session teardown is bounded as well (review): the destructor interrupts a running port call
(`ISerialPort::interrupt()`: a self-pipe wakes POSIX waits, `CancelIoEx` on Win32), waits at most
`kShutdownJoinMs` (1 s) for the I/O thread and otherwise detaches it. The thread shares ownership of
all its state (`shared_ptr<IoState>`) and closes the port itself, so a pump connect whose first
transaction stalls fails in about 4.5 s and a disconnect after an abandoned transaction returns in
1 s, instead of joining for ever under the pump and bridge locks. The POSIX deadlines are checked on
every retry, `EINTR` included.

Until a detached thread has closed its port, the port stays reserved (review round 2): the registry
holds a weak reference to a reservation the I/O thread releases only after its close. A new session
on that port is refused at once with `PortBusy` ("serial port <name> is still closing after a
stalled transaction") and opens again once the thread is done; an acquire that races a normal close
waits for it (at most 1.25 s, never under the registry lock) and then opens. The session deleter no
longer takes the registry lock, so other ports open during a teardown. The session and its I/O
thread log through the logger captured at session creation, never spdlog's global default, so a
detached thread can still log after `spdlog::drop_all()` at exit. A pump reconnect refused that way
says so: "Pump connect failed: port busy: serial port COM3 is still closing after a stalled
transaction".

On a write or drain timeout the port discards the unsent bytes (POSIX `tcflush(TCOFLUSH)`, Win32
`PurgeComm`), so a stop or start reported as failed is not delivered when the adapter recovers.
`Transaction::detail` names the failure; `SyringePumpService::lastBusError()` keeps it per pump and
the facade appends it to a failed pump command ("Pump stop failed: write failed: serial write timed
out after 1000 ms (0 of 8 bytes taken, discarded)"). The happy path is unchanged: same bytes,
same messages, drain confirmation within a millisecond of `tcdrain`.

No bridge ABI or contract change. See [[services/SerialBus]], [[services/ISerialPort]],
[[services/SyringePumpService]], [[architecture/Rust-Bridge]]. Tests: `backend.serial_port_posix_timeout`
(never drains returns at 50 ms of 50; endless `EAGAIN` on a pty with `tcflow(TCOOFF)` returns at
999 ms of 1000; endless `EINTR` from `TIOCOUTQ` at 50 ms of 50 and from `write` at 999 ms of 1000;
`interrupt()` wakes a waiting write; fails and hangs without the fix), `backend.serial_bus_deadline`
(held port call: 1550 ms of a 1550 ms budget, fail-fast 0 ms, late result and late reply never
answer the next request, 240-call concurrent stress with no misattributed answer, teardown while
held: joined at once after `interrupt()`, detached at 1000 ms when the call ignores it, the
detached thread closing its port once released; the same port refused after 0 ms while it holds it
and reopened after, another port opening during a teardown, a racing acquire that is released only
once it is seen waiting (`closingWaitersForTesting()`, with a 10 s test bound from
`setShutdownJoinMsForTesting()`) and then opens, the late warning reaching the session's logger after
`spdlog::drop_all()`), stalled-adapter, stalled-connect (4.5 s) and
disconnect-after-abandon (1.0 s) cases in `backend.syringe_pump_fake_serial`, the stalled stop in
`backend.peristaltic_pump_fake_serial`, the Win32 purge and interrupt checks in
`backend.serial_port_win32_timeout`, and
`client_loss_with_a_stalled_pump_still_stops_and_saves` in `crates/mib-bridge-server/tests/ws.rs`
(stop-and-save done 2.5 s after the drop; the next `pump_stop` answers in 1.0 s, naming the
timeout). Not covered: a port call stuck inside the kernel keeps its detached thread (and the
port) until the call returns, and bytes already inside an adapter's own FIFO cannot be recalled.
