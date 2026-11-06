# Evidence boundaries

- `api.test.ts`: real SQLite ownership and constraints, RSA JWT verification; cloud I/O mocked.
- `runtime.test.ts`: actual local workerd/D1/R2 streaming upload; queue delivery mocked.
- `jobs.test.ts`: actual Python subprocess generation over HTTP with real CAD, SQLite and
  storage adapters; injected output-write failure and replay. Not Cloudflare container lifecycle.
- `test_pipeline.py`: actual CAD/codec/GDS/ZIP content, units, checksums and fail-closed paths.
  The DWG case requires the image's LibreDWG tools (skipped in a plain host venv).
- `ui.spec.ts`: actual browser and Driver.js, fixture API responses; no live login or CAD job.

Run the DWG-inclusive suite from repository root after building the image:

```sh
docker run --rm --network none --memory 4g --cpus 2 \
  -v "$PWD/tools/dot_grid_portal/tests:/app/tools/dot_grid_portal/tests:ro" \
  mib-dot-grid-portal:local python -m pytest /app/tools/dot_grid_portal/tests/test_pipeline.py -q -p no:cacheprovider
```

No passing local test establishes fabrication readiness, full-wafer capacity, or live cloud deployment.

The DWG fixture is GNU LibreDWG 0.14 `test/test-data/2000/Line.dwg`, copied from the SHA-256-pinned source archive during image build (GPL license retained). It tests real DWG decoding, not a synthetic writer round-trip. Customer DWG variants remain unverified.
