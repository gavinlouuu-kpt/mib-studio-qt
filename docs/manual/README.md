# MIB Studio Qt — User Manual

Operator-facing documentation for MIB Studio Qt, organized by workflow.
No build tools or developer setup required — everything here assumes you
are running the installed Windows application.

**Prefer a website?** This manual is published as a searchable user-guide
site at <https://kpt1020.github.io/mib-studio-qt/> — same pages, plus a
visual tour landing page ([index.md](index.md)). The site is built by
[`docs-site.yml`](../../.github/workflows/docs-site.yml) from `mkdocs.yml`
and redeploys whenever these pages or the generated screenshots change.

## Workflows

1. [Getting started](getting-started.md) — install, update, and find your
   way around the window.
2. [Connect a camera](connect.md) — hardware, MindVision, or the
   folder-backed mock camera.
3. [Acquire & record](acquire-and-record.md) — live view, ROI, processing
   preview, monitoring charts, and recording experiments to HDF5.
4. [Review & post-process](review-and-postprocess.md) — browse recorded
   files, export metrics/images, reanalyse, and the standalone tools.
   [YOFO Review](yofo-review.md) — the Review tab as a standalone
   macOS / Windows app: install, open files, export, updates.
5. [Troubleshooting](troubleshooting.md) — logs, crash reports, and common
   failure modes.

## About the screenshots

Every screenshot in this manual is **generated, not hand-captured**. The
`screenshot_tour` program launches the real application in mock-camera mode
and captures each documented view into [`images/`](images/) (one PNG per
named view, plus a `manifest.json`). The Build Windows workflow regenerates
them on each release, so the images always match the released UI — if your
screen does not look like the manual for your version, that difference is
itself a debugging clue worth including in a bug report.

Regenerate locally (from a built checkout). On Linux (system Qt packages,
see `docs/howto/linux-build.md`):

```bash
cmake --preset linux-system-release
cmake --build build/linux-system --target screenshot_tour
QT_QPA_PLATFORM=offscreen ./build/linux-system/screenshot_tour --out docs/manual/images
```

On Windows:

```powershell
cmake --build build --config Release --target screenshot_tour
$env:QT_QPA_PLATFORM = "offscreen"
build\Release\screenshot_tour.exe --out docs\manual\images
```

`python3 scripts/check_screenshots.py` verifies that manual pages and the
generated screenshot set stay in sync; CI runs it on every change.
