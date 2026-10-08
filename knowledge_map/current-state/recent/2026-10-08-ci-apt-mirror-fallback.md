## 2026-10-08 — CI apt install falls back from a crawling mirror

The hosted runners' Azure Ubuntu mirror sometimes serves packages at a crawl. On 2026-10-08 the
`thread` and backend jobs of #619 and #631 fetched 19 packages in 9 minutes, and one qt-ci run
spent 24 minutes in setup. apt never abandons a slow but live transfer, so the 10-minute setup
step timed out before any code ran.

`scripts/ci/apt-install.sh` now installs the packages for `.github/actions/setup-linux-env`. It
bounds update + install to 240 s, which is about twice the slowest healthy lane (40–124 s
measured across all Linux workflows). On failure it rewrites the Azure host to
archive.ubuntu.com in `/etc/apt/apt-mirrors.txt` and `ubuntu.sources`, then retries for up to
300 s, reusing any .debs already downloaded. Both attempts fit inside the 10-minute step timeout.
The fallback path was tested in an ubuntu:24.04 container with a forced 1 s first attempt.
[[build-and-run/Build]]
