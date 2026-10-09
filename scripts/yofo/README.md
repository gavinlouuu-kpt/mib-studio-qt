# scripts/yofo

Board tooling for the PZ7035 YOFO Studio: bundle packaging and the browser E2E scripts. The E2E scripts need Python Playwright with
Chromium and a reachable Studio server (its listener is `127.0.0.1:8427` on the board, no token).

Run an E2E script through an SSH tunnel to the board (the board owner agrees the slot first; start the tunnel under a transient unit and
stop it at hand-back):

```bash
systemd-run --user --unit=yofo501-tunnel ssh -N -o ExitOnForwardFailure=yes -L 127.0.0.1:8427:127.0.0.1:8427 amd-edf@192.168.137.2
python3 scripts/yofo/e2e_ring_playback.py http://127.0.0.1:8427 OUTDIR --take-control
```

- `e2e_studio_acceptance.py`: preflight, Run, an experiment and its file, the Files and Diagnostics views.
- `e2e_run_align_flips.py`: the Run to Align switch soak (`--flips N`); the board owner's read-only sampler reports the flag clears.
- `e2e_ring_playback.py`: the frame ring (#649): Stop in Run, scrub/step/play at 1/30/60 fps, overlays, the cell table, the gates while
  stopped (`run.frozen`, Camera & Alignment refused), a viewer's Stop/Resume refused, Resume and a second Stop. Screenshots and
  `results.json` go to OUTDIR. It never touches the pumps and starts no experiment. `--take-control` claims the controller role over
  the page's own socket: use it only when the operator has agreed.
