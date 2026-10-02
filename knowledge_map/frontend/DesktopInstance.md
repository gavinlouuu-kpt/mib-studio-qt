# Desktop Instance

> One hardware-owning Qt desktop per user, across install paths and channels.

**Source:** `include/frontend/system/DesktopInstance.h`,
`src/frontend/system/DesktopInstance.cpp`, `src/frontend/core/main.cpp`

`DesktopInstance::acquire()` uses `QLockFile` at
`GenericDataLocation/MIB_Studio_Qt/desktop.lock` before settings migration,
logging, SDK initialization, or hardware discovery. The guard outlives the
backend. A second launch reports the owning PID and exits without opening
hardware. It does not terminate or activate the existing process.

The stale age is zero: a long experiment never loses its lock due to age.
Qt can recover a dead process's lock automatically. Unwritable lock storage
fails closed with an explanatory message. This protects cooperating new
builds, not older versions without a guard or separate vendor tools.

**Tests:** `frontend.desktop_instance` exercises duplicate subprocesses,
clean release, and recovery after killing an owning test subprocess.

**Related:** [[MainWindow]], [[System-Utilities]],
[[../task/2026-09-15-hardware-shutdown]]
