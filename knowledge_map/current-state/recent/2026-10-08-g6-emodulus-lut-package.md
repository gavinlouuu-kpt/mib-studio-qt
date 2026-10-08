## 2026-10-08 — The standing package ships the E-modulus LUT (#651 G6)

The unit passed no `--resource-dir` and the package had no `resources/`, so the backend looked for the
bundled LUT under `/var/lib/resources/isoelastic_curve/...` (the data dir's parent), found nothing,
and E-modulus gating could not compile on the PZ7035. The package now carries
`resources/isoelastic_curve/` (both LUT files, in `MD5SUMS`), `install.sh` installs it under
`/usr/share/yofo-studio/resources/isoelastic_curve`, and the unit passes `--resource-dir /usr/share/yofo-studio`.
Check on the board: the journal line "Young's modulus LUT source=bundled-fallback ... path=/usr/share/yofo-studio/resources/..."
(and no "Young's modulus LUT not loaded"). See [[Build]].
