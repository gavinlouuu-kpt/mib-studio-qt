# Architecture — MOC

> Map of Content for the architecture cluster.

- [[Overview]] — layered design summary
- [[AppBackend]] — composition root, owns + wires all services
- [[ExperimentCoordinator]] — readiness transaction + immutable run snapshot (issue #369)
- [[Threading-Model]] — main, capture, processing, realtime, autofocus, etc.
- [[Data-Flow]] — camera → FrameStore → processing → HDF5
- [[Rust-Bridge]] — cxx bridge over BackendFacade (React + Tauri, epic #246)
- [[Desktop-Shell]] — React + Tauri v2 desktop app (Phase 3, epic #246)
- [[Analysis-Helper]] — bounded Toolkit helper transport and process ownership (#399)

**Up**: [[../README|Vault home]] · **See also**: [[../services/_MOC|Services MOC]]
