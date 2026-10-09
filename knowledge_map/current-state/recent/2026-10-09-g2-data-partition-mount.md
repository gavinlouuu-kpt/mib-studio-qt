## 2026-10-09 — The data directory lives on the eMMC partition (#651 G2)

The board's eMMC is an ext4 partition labelled `yofo-data` (7 GiB free, about 22 MB/s). The RAM root forgets mounts at boot, so
the bundle's `install.sh` now runs `mount-data.sh` before it installs the server (and so before the unit starts): unless
`/var/lib/yofo-studio` is already a mount point, it mounts `-L yofo-data` there with `noatime,commit=30`. The running unit is
stopped around the mount and started again; files already in the RAM directory are moved aside and put back, onto the
partition root when it is empty (first use) or into a `from-ram-<time>/` folder when it already has data, so nothing is
deleted or overwritten; a failed mount puts them back and exits non-zero (install.sh carries on and says so). Without the
label the directory stays on the RAM root: the storage status (`recordingTarget`: tmpfs/ramfs = RAM, ext4 = persistent) keeps
the "Recording to RAM" warning in the preflight, and the Diagnostics view shows "The data directory is volatile". With the
partition the warning goes away. Files view, Save paths, logs and the run window land on the eMMC. The SD card is untouched.
`scripts.yofo_standing_package` runs the script against fake mount/blkid/mountpoint/systemctl (label present or absent, empty
or non-empty partition, already mounted, unit stopped and restarted, failed mount). See [[Build]].
