## 2026-11-04 — Beta release notes stay under GitHub's body limit

Since 2026-10-21 no beta had published: `Create GitHub Release` in `build-windows.yml` failed with
`HTTP 422: body is too long (maximum is 125000 characters)` (run 41775840096, develop `5f906ac2`).
Beta notes list every `recent/` fragment since the last stable tag (`v1.2.0`), so they only grew:
161083 characters on that commit, 146 fragments in `recent/` and 76 of them newer than the tag.

`scripts/release_notes.py` now keeps beta notes within `NOTES_LIMIT` (120000, a margin under
GitHub's 125000). Over the limit it drops whole entries from the oldest end and puts one line right
after the heading: how many of the entries are not listed, and `compare/<stable tag>...<HEAD sha>`
for the full list. On the same commit the notes become about 116300 characters (the newest 50 of 76
entries). Entries keep their order, and notes that already fit are unchanged. If one entry alone is
over the limit it is cut at a line break and says so. The file is written with LF on Windows too, so
the length that is checked is the length GitHub counts.

Curated stable notes are not trimmed (that would drop reviewed content). `--check` now fails above
120000 characters, so the stable gate catches it before the build, not at `gh release create` after
it. Tests: `scripts.release_notes` (`tests/tools/test_release_notes.py`). See [[build-and-run/Build]].
