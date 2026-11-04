//! The bridge lock, with a bounded wait for status reads (#845).
//!
//! The C++ `BackendBridge` is not safe to call concurrently, so every command still runs under
//! one mutex. A long command (stop capture on a camera that gets no frames, start, a camera
//! script) holds that mutex for as long as the backend takes. Before #845 every other command,
//! including the 500 ms status pollers, queued behind it, so the window showed stale state and
//! every button waited.
//!
//! What this module changes:
//! - [`BridgeLock::lock_for`] records which long command holds the lock and since when, so a
//!   waiter can say what it is waiting for.
//! - [`status_scope`] marks the current thread as a status read. Inside it, [`BridgeLock::lock`]
//!   waits at most [`STATUS_WAIT`] and then fails with a message that starts with [`BUSY_PREFIX`].
//! - [`StatusCache`] keeps the last good reply of each status command, so a status read that
//!   finds the backend busy answers with that reply instead of an error.
//!
//! Commands outside a status scope (control commands, the WebSocket server) keep the blocking
//! lock they always had: they must not run against a half-stopped backend.

use std::cell::Cell;
use std::collections::HashMap;
use std::sync::{Mutex, MutexGuard, TryLockError};
use std::time::{Duration, Instant};

/// How long a status read waits for the bridge before it answers from the cache.
pub const STATUS_WAIT: Duration = Duration::from_millis(250);

/// Every busy error starts with this text; [`is_busy`] tests for it.
pub const BUSY_PREFIX: &str = "bridge busy:";

thread_local! {
    static STATUS_DEADLINE: Cell<Option<Duration>> = const { Cell::new(None) };
}

/// Run `f` as a status read: bridge locks taken inside wait at most [`STATUS_WAIT`].
pub fn status_scope<R>(f: impl FnOnce() -> R) -> R {
    status_scope_with(STATUS_WAIT, f)
}

/// [`status_scope`] with an explicit wait (tests, shutdown).
pub fn status_scope_with<R>(wait: Duration, f: impl FnOnce() -> R) -> R {
    struct Restore(Option<Duration>);
    impl Drop for Restore {
        fn drop(&mut self) {
            STATUS_DEADLINE.with(|d| d.set(self.0));
        }
    }
    let _restore = Restore(STATUS_DEADLINE.with(|d| d.replace(Some(wait))));
    f()
}

/// True when `error` is the busy error of a bounded status wait.
pub fn is_busy(error: &str) -> bool {
    error.starts_with(BUSY_PREFIX)
}

/// The long command that holds the bridge, and for how long it has held it.
#[derive(Clone, Debug, PartialEq)]
pub struct Holder {
    pub command: &'static str,
    pub held_for: Duration,
}

/// The process's one bridge mutex, plus the name of the long command that holds it.
pub struct BridgeLock<T> {
    inner: Mutex<T>,
    holder: Mutex<Option<(&'static str, Instant)>>,
}

/// A held bridge lock that clears the holder record when it drops.
pub struct BridgeGuard<'a, T> {
    guard: MutexGuard<'a, T>,
    holder: &'a Mutex<Option<(&'static str, Instant)>>,
}

impl<T> std::ops::Deref for BridgeGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.guard
    }
}

impl<T> std::ops::DerefMut for BridgeGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.guard
    }
}

impl<T> Drop for BridgeGuard<'_, T> {
    fn drop(&mut self) {
        if let Ok(mut holder) = self.holder.lock() {
            *holder = None;
        }
    }
}

impl<T> BridgeLock<T> {
    pub fn new(value: T) -> Self {
        BridgeLock { inner: Mutex::new(value), holder: Mutex::new(None) }
    }

    /// Lock the bridge. Blocks, except inside a [`status_scope`], where it waits at most the
    /// scope's time and then returns a [`BUSY_PREFIX`] error naming the holder.
    pub fn lock(&self) -> Result<MutexGuard<'_, T>, String> {
        match STATUS_DEADLINE.with(Cell::get) {
            None => self.inner.lock().map_err(|e| e.to_string()),
            Some(wait) => self.lock_within(wait),
        }
    }

    /// Lock the bridge for a long command and record its name, so status reads that time out
    /// can say what holds the backend. Always blocks: a control command must not be skipped.
    pub fn lock_for(&self, command: &'static str) -> Result<BridgeGuard<'_, T>, String> {
        let guard = self.inner.lock().map_err(|e| e.to_string())?;
        if let Ok(mut holder) = self.holder.lock() {
            *holder = Some((command, Instant::now()));
        }
        Ok(BridgeGuard { guard, holder: &self.holder })
    }

    /// Lock with an explicit bound, whatever the thread's scope (used by shutdown on exit).
    pub fn lock_within(&self, wait: Duration) -> Result<MutexGuard<'_, T>, String> {
        let deadline = Instant::now() + wait;
        loop {
            match self.inner.try_lock() {
                Ok(guard) => return Ok(guard),
                Err(TryLockError::Poisoned(e)) => return Err(e.to_string()),
                Err(TryLockError::WouldBlock) => {
                    if Instant::now() >= deadline {
                        return Err(self.busy_message());
                    }
                    std::thread::sleep(Duration::from_millis(2));
                }
            }
        }
    }

    /// The long command that holds the bridge now, if it was taken with [`Self::lock_for`].
    pub fn holder(&self) -> Option<Holder> {
        let holder = self.holder.lock().ok()?;
        holder.map(|(command, since)| Holder { command, held_for: since.elapsed() })
    }

    fn busy_message(&self) -> String {
        match self.holder() {
            Some(h) => format!(
                "{BUSY_PREFIX} {} has held the backend for {:.1} s",
                h.command,
                h.held_for.as_secs_f64()
            ),
            None => format!("{BUSY_PREFIX} another backend command holds the bridge"),
        }
    }
}

/// The last good reply of each status command, keyed by command name and arguments.
#[derive(Default)]
pub struct StatusCache {
    replies: Mutex<HashMap<String, serde_json::Value>>,
}

impl StatusCache {
    /// Run a status read in a [`status_scope`]. On success, cache and return its JSON. When the
    /// bridge is busy, return the cached reply for `key`, or the busy error if there is none yet.
    pub fn read<T: serde::Serialize>(
        &self,
        key: impl Into<String>,
        f: impl FnOnce() -> Result<T, String>,
    ) -> Result<serde_json::Value, String> {
        let key = key.into();
        match status_scope(f) {
            Ok(value) => {
                let json = serde_json::to_value(value).map_err(|e| e.to_string())?;
                if let Ok(mut replies) = self.replies.lock() {
                    replies.insert(key, json.clone());
                }
                Ok(json)
            }
            Err(error) if is_busy(&error) => self
                .replies
                .lock()
                .ok()
                .and_then(|replies| replies.get(&key).cloned())
                .ok_or(error),
            Err(error) => Err(error),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{mpsc, Arc};

    struct MockBridge {
        status: u32,
    }

    impl MockBridge {
        fn slow_stop(&mut self) {
            std::thread::sleep(Duration::from_millis(1500));
            self.status += 1;
        }
    }

    #[test]
    fn slow_command_does_not_block_a_concurrent_status_read() {
        let lock = Arc::new(BridgeLock::new(MockBridge { status: 7 }));
        let cache = StatusCache::default();
        let read = |lock: &BridgeLock<MockBridge>| -> Result<u32, String> { Ok(lock.lock()?.status) };

        // Prime the cache while the bridge is idle.
        assert_eq!(cache.read("status", || read(&lock)).unwrap(), serde_json::json!(7));

        let (held, wait_held) = mpsc::channel();
        let slow = {
            let lock = Arc::clone(&lock);
            std::thread::spawn(move || {
                let mut guard = lock.lock_for("stop_capture").unwrap();
                held.send(()).unwrap();
                guard.slow_stop();
            })
        };
        wait_held.recv().unwrap();

        let started = Instant::now();
        let reply = cache.read("status", || read(&lock)).unwrap();
        let elapsed = started.elapsed();
        assert_eq!(reply, serde_json::json!(7), "answered from the last good reply");
        assert!(elapsed < Duration::from_millis(1000), "status waited {elapsed:?} behind the slow command");

        let holder = lock.holder().expect("the slow command is recorded");
        assert_eq!(holder.command, "stop_capture");

        // A read with nothing cached gets the busy error, which names the holder.
        let error = cache.read("other", || read(&lock)).unwrap_err();
        assert!(is_busy(&error) && error.contains("stop_capture"), "{error}");

        slow.join().unwrap();
        assert!(lock.holder().is_none(), "the holder clears when the guard drops");
        assert_eq!(cache.read("status", || read(&lock)).unwrap(), serde_json::json!(8));
    }

    #[test]
    fn control_commands_outside_a_status_scope_still_wait() {
        let lock = Arc::new(BridgeLock::new(0u32));
        let (held, wait_held) = mpsc::channel();
        let slow = {
            let lock = Arc::clone(&lock);
            std::thread::spawn(move || {
                let mut guard = lock.lock_for("apply_camera_script").unwrap();
                held.send(()).unwrap();
                std::thread::sleep(Duration::from_millis(400));
                *guard = 1;
            })
        };
        wait_held.recv().unwrap();
        // A control command blocks until the slow one finishes and sees its result.
        assert_eq!(*lock.lock().unwrap(), 1);
        slow.join().unwrap();
    }

    #[test]
    fn status_scope_restores_the_blocking_lock_after_it_returns() {
        let lock = BridgeLock::new(0u32);
        status_scope(|| assert!(lock.lock().is_ok()));
        assert!(STATUS_DEADLINE.with(Cell::get).is_none());
    }
}
