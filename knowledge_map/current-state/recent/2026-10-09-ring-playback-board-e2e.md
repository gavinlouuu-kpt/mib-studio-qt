## 2026-10-09 — The frame-ring playback has a repeatable board E2E

`scripts/yofo/e2e_ring_playback.py` drives YOFO Studio on the PZ7035 through the browser: the `ring.placement` gate, a ring frame read
while Run is live (a `MIBR` packet, no hang), Stop and the playback panel (about 5,000 frames, 1.00 s), scrub, step, play at 1/30/60 fps,
the four overlays, the cell table, Save clip disabled, the `run.frozen` gate, Camera & Alignment refused while stopped (frames kept), a
viewer's Resume refused, Resume and a second Stop with new frames. Slot B (develop 67fd5122 + pl-results13 + producer 2c9f2830, `mem=720M`,
ring 5000) passed 27 of 27. `scripts/yofo/README.md` says how to run it through the SSH tunnel. The Run-entry sampler stays the board
owner's tool.
