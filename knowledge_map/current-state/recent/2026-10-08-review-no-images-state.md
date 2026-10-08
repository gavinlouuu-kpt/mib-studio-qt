## 2026-10-08 — Review says plainly when a file has no images (#649 decision 7)

A run saved by a PL-science instrument holds one metadata row per cell and no image or mask datasets. Review
opens it, but the frame, thumbnail and mask views would only read 0 frames. Both review UIs (the Studio Review tab
and the standalone YOFO Review panel) now show a status line when an experiment file has rows but no images: the
metrics table, charts and export work; the pixel views have nothing to show. Raw recordings, files with images
and empty files are unaffected (`review/noImages.ts`). See [[Desktop-Shell]].
