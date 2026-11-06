# Local tailnet review adapter

This separate Node entrypoint reuses the production API and job code with SQLite/filesystem
bindings and a loopback CAD container. It is **not** imported by the Worker build.
Tailnet reachability is the access boundary: all permitted tailnet visitors share one review
workspace; Cloudflare Access and real membership are not being demonstrated. Invitations
are unavailable (member role). Review registry identities must not be used in production.

Runtime on P5 (2026-11-04):
- HTTPS: https://gavin-p5-ubuntu.himalayan-coho.ts.net:19837/
- User service: `mib-dot-grid-review.service`, loopback 19837.
- Docker container: `mib-dot-grid-review-cad`, loopback 19838, memory 4 GiB / 2 CPUs.
- Persistent data: `/mnt/hdd/developer-data/mib-studio-qt/dot-grid-review`.
- Only this new Tailscale Serve port was added; existing handlers were preserved.

Build from portal directory: `npx esbuild ./review/server.ts --bundle --platform=node --format=esm --packages=external --outfile=review/server.mjs`.
Set `REVIEW_ORIGIN` to the exact external HTTPS origin and `REVIEW_DATA` to an isolated
persistent directory. Run `node review/server.mjs`; bind is always loopback. The adapter
requires the CAD image HTTP endpoint on loopback 19838. Queue work is serialized and
unfinished jobs resume on restart. No synthetic success responses are used.

`smoke.mjs` drives the actual HTTPS app, uploads `/tmp/mib-tailnet-review.dxf` (a 6 mm
rectangle with an internal line, units micrometres), confirms, generates, downloads all
three artifacts and opens the actual Driver.js tour. Set REVIEW_ORIGIN before running.
This creates review data. It does not prove access from a second physical tailnet device.

Stop only this review: `tailscale serve --https=19837 off`, then
`XDG_RUNTIME_DIR=/run/user/1000 systemctl --user stop mib-dot-grid-review.service` and
`docker stop mib-dot-grid-review-cad`. Do not reset the whole Serve configuration.
