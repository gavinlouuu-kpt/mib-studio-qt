# Recent Work: one file per change

Every non-trivial feature or fix adds **one new file** here, in the same PR as the code. Do not
append to [[current-state/Recent-Work]] any more: that file is the archive of entries up to
2026-10-07. Because every PR used to add its entry to the top of that one file, each merge made
every other open PR conflict and re-run CI. Separate files never conflict.

**File name:** `YYYY-MM-DD-<slug>.md` (lowercase slug, `a-z0-9-`, usually the issue or branch,
e.g. `2026-10-08-542-lock-setup-during-run.md`). The date is the day the PR is opened.

**Content:** the same shape as the archive entries. The first line is an H2 heading with the same
date, then the body (wikilinks to the affected notes are welcome):

```markdown
## 2026-10-08 — Background and ROI setup locked during a run (#542)

What changed, why, and anything a future reader needs. See [[ExperimentCoordinator]].
```

**Reading:** the newest files sort last by name. `python3 scripts/recent_work.py` prints the
newest entries first (`-n 20` for more; `--archive` to continue into the archive).

`python3 scripts/check_docs.py` enforces the file name, the matching dated heading, and that the
archive gains no new dated entries.
