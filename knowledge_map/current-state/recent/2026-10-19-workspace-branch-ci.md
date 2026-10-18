## 2026-10-19 — CI runs on pull requests into `feat/egrabber-workspace`

The docs, desktop, bridge and backend workflows now run on pull requests into the workspace
branch. Before this, only the gold-reference guard ran, so kit and track slices merged into
the branch without tests. The trigger lists in `docs-ci.yml`, `desktop-ci.yml`, `bridge-ci.yml`
and `backend-ci.yml` gain `feat/egrabber-workspace`. Remove the entries when the branch merges
into `develop`.

CI configuration only: no code, ABI or schema changes.
