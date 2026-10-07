## 2026-10-07 — Release-notes script tests write UTF-8 explicitly

`scripts.release_notes` failed on develop's Windows build (f18c933c): the test wrote its fixture, whose title contains an em dash, with `Path.write_text()` and no encoding, so Windows used cp1252, and the script (which reads UTF-8) failed to decode it. The tests now pass `encoding="utf-8"` everywhere. The release step and `publish-update.py` already did. Rule for scripts and tests: always pass `encoding="utf-8"` to `read_text`/`write_text`/`open`.
