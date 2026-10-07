## 2026-10-07 — FCS 3.1 native detection export (#520)

`HdfExportService` now writes contract-aware FCS 3.1 files through the reusable
Qt-free `FcsWriter` registry and publishes the FCS file plus exact event-map
CSV in one transaction. Valid detections are the default; invalid/both rows are
explicit. HDF compound member presence controls channels, timestamps become
relative seconds, non-finite values fail closed, and duplicate object IDs are
preserved per detection. The DATA pass is streamed after a range pass, and
contract provenance rejects malformed or conflicting HDF5 attributes while
legacy files default safely to Contract 1. `scripts/export_hdf5.py --format
fcs` delegates to `hdf_export_cli`, resolves relative native paths, forwards
Ctrl-C, and reports a clear missing-binary error. The optional FlowIO round-trip
script is available when FlowIO is installed; FlowJo import remains manually
unverified. See
[[services/HdfExportService]] and [[../docs/howto/hdf5-export-app]].
