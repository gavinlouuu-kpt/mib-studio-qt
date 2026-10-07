## 2026-10-07 — Save fresh Windows defaults after profile deletion (#583)

Fixed a false disk-change conflict when deleting an active Qt profile and saving
its default config. The inspector normalized CRLF text before hashing while the
checked writer hashed raw file bytes. Keep the disk revision separate from the
editor baseline for all three document editors. Watcher preparation already
runs synchronously before reload; no bridge ABI or watcher ordering change was
needed. See [[frontend/ConfigTabs]].

The regression seeds a complete bundled default with CRLF before deletion,
checks its clean state and exact disk baseline, saves threshold 20, and waits
for live processing to apply it. Original default bytes are restored afterward.
An absent LF default passed before the fix; the CRLF case reproduced the Windows
conflict on Linux before the fix.
