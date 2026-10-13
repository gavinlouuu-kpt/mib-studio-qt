## 2026-10-13 — armv7 bundles build reproducibly (#706)

Two clean `package_studio.sh` builds of one commit differed in `dist.tar` (tar mtimes) and in
`yofo-studio-server` (a version string from `git describe --tags`, absolute `__FILE__` paths). The
version now comes from the commit, `SOURCE_DATE_EPOCH` is the commit time, `dist.tar` is packed
deterministically, the compilers remap the checkout and cargo/rustup homes, and cargo uses `--locked`.
A bundle can therefore reuse the previous bundle's `yofo-studio-server` and `dist.tar` after a
rebuild. See [[Build]].
