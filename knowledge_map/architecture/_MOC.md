# Architecture — MOC

> Map of Content for the architecture cluster.

- [[Overview]] — layered design summary
- [[AppBackend]] — composition root, owns + wires all services
- [[ExperimentCoordinator]] — readiness transaction + immutable run snapshot (issue #369)
- [[Threading-Model]] — main, capture, processing, realtime, autofocus, etc.
- [[Data-Flow]] — camera → FrameStore → processing → HDF5

**Up**: [[../README|Vault home]] · **See also**: [[../services/_MOC|Services MOC]]
