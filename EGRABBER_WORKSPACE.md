# eGrabber-inspired MIB workspace

Implementation branch: `feat/egrabber-workspace`

Base: `develop` at `2a4e0a4f38055df5464400048ab9948d949319e2` (14 October 2026 setup).

This branch starts the reviewed wireframe/prototype implementation. The first commit
is a design and regression baseline, **not a replacement of the working application**.
No hardware commands, bridge ABI, dependencies or production routes are changed.

## Start here

- [Design brief and full-workspace scope](docs/design/egrabber-workspace/README.md)
- [Reviewed v2 interaction reference](docs/design/egrabber-workspace/prototype.html)
- [Staged execution plan](docs/exec-plans/active/2026-10-14-egrabber-workspace.md)
- [Reference provenance](docs/design/egrabber-workspace/reference.json)

The seven-view interaction reference covers the later engineering refinements. It
does not replace the full Prepare / Align / Experiment / Review workspace specification.

## Preview the reference

From the repository root:

```bash
python3 -m http.server 8765 --bind 127.0.0.1 --directory docs/design/egrabber-workspace
```

Open `http://127.0.0.1:8765/prototype.html`. It also opens directly as a local HTML
file. All values, devices, images and failure controls are explicitly simulated.
The preview never calls the native backend. Stop the local server with Ctrl+C.

Run the dependency-free behavior smoke tests:

```bash
node --test docs/design/egrabber-workspace/prototype.test.mjs
```

For the existing application, follow [the agent guide](AGENTS.md) and the current
[desktop-shell architecture](knowledge_map/architecture/Desktop-Shell.md). Do not
mount this simulation in an authenticated live instrument session.
