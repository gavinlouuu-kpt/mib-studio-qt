#!/bin/sh
# Mount the instrument's eMMC data partition (ext4, label yofo-data) at Studio's data directory
# (#651 G2). The RAM root forgets mounts at every boot, so install.sh runs this before the unit starts.
#
#   sh mount-data.sh
#
# - already a mount point: nothing to do;
# - no partition with that label: the data dir stays on the RAM root (volatile) and says so (Studio's
#   Diagnostics view shows "data directory is volatile" from the storage status); exit 0;
# - otherwise: stop the unit if it runs, move any files already in the RAM directory aside, mount, and put the
#   files back. They go to the partition root when it is empty (a fresh partition), else into a
#   from-ram-<time>/ folder on it: nothing is deleted and nothing on the partition is overwritten. The unit is
#   started again if it was running. A failed mount puts the RAM files back and exits 1.
# Overrides for tests: YOFO_DATA_DIR, YOFO_DATA_LABEL, YOFO_UNIT.
set -eu
DATA=${YOFO_DATA_DIR:-/var/lib/yofo-studio}
LABEL=${YOFO_DATA_LABEL:-yofo-data}
UNIT=${YOFO_UNIT:-yofo-studio}

if mountpoint -q "$DATA" 2>/dev/null; then
    echo "mount-data: $DATA is already a mount point"
    exit 0
fi
if ! blkid -L "$LABEL" >/dev/null 2>&1; then
    echo "mount-data: no partition labelled $LABEL: $DATA stays on the RAM root (the data directory is volatile)" >&2
    exit 0
fi

was_active=0
if systemctl is-active --quiet "$UNIT" 2>/dev/null; then
    was_active=1
    systemctl stop "$UNIT"
fi
restart_unit() { [ "$was_active" = 1 ] && systemctl start "$UNIT" || true; }

seed="$DATA.seed.$$"
mkdir -p "$DATA"
if [ -n "$(ls -A "$DATA" 2>/dev/null)" ]; then
    mv "$DATA" "$seed"
    mkdir -p "$DATA"
fi
if ! mount -o noatime,commit=30 -L "$LABEL" "$DATA"; then
    echo "mount-data: mounting $LABEL at $DATA failed; the data directory stays on the RAM root" >&2
    if [ -d "$seed" ]; then
        rmdir "$DATA" 2>/dev/null || true
        mv "$seed" "$DATA"
    fi
    restart_unit
    exit 1
fi
if [ -d "$seed" ]; then
    if [ -z "$(ls -A "$DATA" 2>/dev/null | grep -v '^lost+found$' || true)" ]; then
        cp -a "$seed"/. "$DATA"/
        echo "mount-data: the files that were in the RAM directory are on the partition now"
    else
        target="$DATA/from-ram-$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$target"
        cp -a "$seed"/. "$target"/
        echo "mount-data: the partition already had data; the RAM directory's files are in $target"
    fi
    rm -rf "$seed"
fi
echo "mount-data: $LABEL mounted at $DATA"
restart_unit
