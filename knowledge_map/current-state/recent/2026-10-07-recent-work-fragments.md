## 2026-10-07 — Recent-Work entries are one file per change

Every PR used to add its entry at the top of `Recent-Work.md`, so each merge made every other open
PR conflict there and re-run CI (about 25 minutes, with ten PRs in flight the GitHub runners
saturated). New entries are now one file each in `knowledge_map/current-state/recent/`
(`YYYY-MM-DD-<slug>.md`, first line the matching dated H2), which never conflict.
`scripts/check_docs.py` enforces the name and heading and rejects new dated entries in the
archive (cutoff 2026-10-07, so PRs already in flight still pass). `scripts/recent_work.py` prints
the newest entries. See [[current-state/recent/README]] and [[Vault-Maintenance]].
